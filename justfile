set positional-arguments
set quiet
set shell := ["bash", "-euo", "pipefail", "-c"]

mode := "debug"
prefix := "/usr/local"
# An empty string would be stored as ".", so pass an empty array instead.
pkg-config-path := if env("PKG_CONFIG_PATH", "") == "" { "[]" } else { env("PKG_CONFIG_PATH") }

default:
    just --list

# Configure build-<m> for debug, release, or asan.
[arg("m", pattern="debug|release|asan")]
configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    case "$1" in
        debug) args=(--buildtype=debug) ;;
        release) args=(--buildtype=release -Db_lto=true) ;;
        asan) args=(--buildtype=debug -Db_sanitize=address,undefined) ;;
    esac
    args+=(--prefix "$2" -Dpkg_config_path={{ quote(pkg-config-path) }})
    if [[ -f "build-$1/meson-private/coredata.dat" ]]; then
        args+=(--reconfigure)
    fi
    meson setup "build-$1" "${args[@]}"
    ln -sfn "build-$1/compile_commands.json" compile_commands.json

[arg("m", pattern="debug|release|asan")]
_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-$1/build.ninja" ]]; then
        just configure "$1"
        exit
    fi
    configured_project="$(meson introspect --projectinfo "build-$1" | python3 -c 'import json, sys; print(json.load(sys.stdin)["descriptive_name"])')"
    if [[ "$configured_project" != luciole ]]; then
        just configure "$1"
    fi
    configured_pkg_path="$(just _option "$1" pkg_config_path)"
    if [[ "$configured_pkg_path" != "${PKG_CONFIG_PATH-}" ]]; then
        meson configure "build-$1" -Dpkg_config_path={{ quote(pkg-config-path) }}
    fi
    ln -sfn "build-$1/compile_commands.json" compile_commands.json

# Print a configured Meson option; list values are joined with ':'.
_option m name:
    meson introspect "build-$1" --buildoptions | python3 -c 'import json, sys; value = next(o["value"] for o in json.load(sys.stdin) if o["name"] == sys.argv[1]); print(":".join(value) if isinstance(value, list) else value)' "$2"

[arg("m", pattern="debug|release|asan")]
build m=mode: (_ensure-configured m)
    meson compile -C "build-$1" luciole luciole-io-worker

[arg("m", pattern="debug|release|asan")]
run m=mode: (build m)
    LUCIOLE_ASSETS_DIR="$PWD/lib/assets" ./build-$1/luciole

[arg("m", pattern="debug|release|asan")]
test m=mode *args: (_ensure-configured m)
    if [[ "$(just _option "$1" tests)" != enabled ]]; then meson configure "build-$1" -Dtests=enabled; fi
    meson test -C "build-$1" --print-errorlogs "${@:2}"

[arg("m", pattern="debug|release|asan")]
install m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -x "build-$1/luciole" ]]; then
        echo "error: build-$1/luciole is missing; build it before installing" >&2
        exit 1
    fi
    meson install --no-rebuild -C "build-$1"

[arg("m", pattern="debug|release|asan")]
uninstall m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-$1/build.ninja" ]]; then
        echo "error: build-$1 is not configured" >&2
        exit 1
    fi
    ninja -C "build-$1" uninstall

_files kind:
    #!/usr/bin/env python3
    """Enumerate owned files without consulting repository metadata."""

    import os
    import sys
    from pathlib import Path

    EXCLUDED = {
        ".git",
        ".aws",
        ".cache",
        ".direnv",
        ".idea",
        ".vscode",
        "__pycache__",
        "node_modules",
        "result",
    }

    EXTENSIONS = {
        "code": {".cpp", ".h"},
        "nix": {".nix"},
        "python": {".py"},
        "text": {".md", ".mdx", ".cpp", ".h", ".in", ".py", ".nix", ".txt", ".build", ".toml", ".bash", ".fish"},
    }


    def project_files(kind):
        for directory, children, names in os.walk(".", followlinks=False):
            children[:] = sorted(
                name
                for name in children
                if name not in EXCLUDED
                and not (name == "lib" and kind == "text")
                and not name.startswith(("build-", "result-"))
                and not Path(directory, name).is_symlink()
            )
            for name in sorted(names):
                path = Path(directory, name)
                if path.is_symlink():
                    continue
                if path.suffix in EXTENSIONS[kind] or (
                    kind == "text"
                    and name in ("justfile", "VERSION", "LICENSE", ".clang-format", ".clang-tidy", "_luciole")
                ):
                    yield path


    if __name__ == "__main__":
        if len(sys.argv) != 2 or sys.argv[1] not in ("code", "nix", "python", "text"):
            sys.exit("usage: just _files <code|nix|python|text>")
        for item in project_files(sys.argv[1]):
            sys.stdout.buffer.write(os.fsencode(item) + b"\0")

_check_formatter:
    #!/usr/bin/env bash
    set -euo pipefail
    version="$(clang-format --version)"
    echo "$version"
    if [[ ! "$version" =~ version\ ([0-9]+)\. ]] || [[ "${BASH_REMATCH[1]}" -lt 23 ]]; then
        echo "error: formatting requires clang-format 23 or newer; put its bin directory on PATH" >&2
        exit 1
    fi

format: format-code

format-check: format-code-check lint-prose

format-code: _check_formatter
    just _files code | python3 -c 'import sys; sys.stdout.buffer.write(b"\0".join(p for p in sys.stdin.buffer.read().split(b"\0") if p and not p.startswith(b"lib/"))+b"\0")' | xargs -0 -r clang-format -i

format-code-check: _check_formatter
    just _files code | xargs -0 -r clang-format --dry-run -Werror

format-nix:
    just _files nix | xargs -0 -r nixfmt

format-nix-check:
    just _files nix | xargs -0 -r nixfmt --check

[arg("m", pattern="debug|release|asan")]
_clang_tidy m=mode: (_ensure-configured m)
    meson compile -C "build-$1" luciole
    run-clang-tidy -quiet -p "build-$1" -j "$(nproc)" -warnings-as-errors='*' -header-filter="$(realpath .)/src/.*|^\.\./src/.*" "$(realpath .)/src/.*"

[arg("m", pattern="debug|release|asan")]
lint m=mode: (_clang_tidy m) lint-python lint-prose

lint-nix:
    just _files nix | xargs -0 -r -n1 statix check
    just _files nix | xargs -0 -r deadnix --fail

lint-prose:
    #!/usr/bin/env python3
    """Check owned source and guidance for the project's punctuation and whitespace rules."""

    import os
    import subprocess
    import sys
    from pathlib import Path

    def project_files(kind):
        paths = subprocess.check_output(["just", "_files", kind]).split(b"\0")
        return (Path(os.fsdecode(path)) for path in paths if path)

    errors = []
    for path in project_files("text"):
        text = path.read_text(encoding="utf-8")
        if not text.endswith("\n"):
            errors.append(f"{path}: missing final newline")
        for number, line in enumerate(text.splitlines(), 1):
            if "\u2014" in line:
                errors.append(f"{path}:{number}: em dashes are prohibited")
            if line != line.rstrip():
                errors.append(f"{path}:{number}: trailing whitespace")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        sys.exit(1)

# Palette roles blended with a literal alpha (`colorSpecFromRole(ColorRole::X, 0.4F)`) are banned in src/.
# lib/ is a copy of noctalia-shell and covered by its own role-alpha-check there.
role-alpha-check:
    #!/usr/bin/env python3
    """Fail on palette roles combined with a literal alpha in src/; same pattern as noctalia-shell."""

    import re
    import sys
    from pathlib import Path

    PATTERN = re.compile(
        r"\b(?:colorSpecFromRole|colorForRole)\(\s*ColorRole::\w+\s*,\s*"
        r"(?:\w+\s*\?\s*)?[0-9]*\.?[0-9]+F?(?:\s*:\s*[0-9]*\.?[0-9]+F?)?\s*\)"
    )

    errors = []
    for path in sorted(Path("src").rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        text = path.read_text(encoding="utf-8")
        for match in PATTERN.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            errors.append(f"{path}:{line}: {match.group(0)}")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        print(
            "error: palette role blended with a literal alpha. Use a palette role (surface_container_*, *_container,\n"
            "outline_variant) or a named Style constant for real transparency. See CONTRIBUTING.md, Palette colors.",
            file=sys.stderr,
        )
        sys.exit(1)

lint-python:
    just _files python | xargs -0 -r python3 -m py_compile

[arg("m", pattern="debug|release|asan")]
fix m=mode: (_ensure-configured m)
    meson compile -C "build-$1" luciole
    run-clang-tidy -quiet -p "build-$1" -j "$(nproc)" -fix -header-filter='(\.\./)?src/.*' "$(realpath src)/.*"
    just format-code

[arg("m", pattern="debug|release|asan")]
clean m=mode:
    rm -rf -- "build-$1"

[arg("m", pattern="debug|release|asan")]
rebuild m=mode: (clean m) (build m)
