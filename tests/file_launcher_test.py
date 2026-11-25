"""Exercise real desktop dispatch with private associations and a recording handler."""

import configparser
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def run_case(binary, mode, names):
    with tempfile.TemporaryDirectory(prefix="luciole-launch-test-") as directory:
        root = Path(directory)
        for name in ("home", "config", "data/applications", "cache", "state", "runtime"):
            (root / name).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        for data_dir in os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(":"):
            mime = Path(data_dir) / "mime"
            # Flatpak exports ship a stub mime.cache; grouped-open needs real PNG detection.
            if (mime / "mime.cache").is_file() and (mime / "image/png.xml").is_file():
                (root / "data/mime").symlink_to(mime)
                break
        else:
            raise RuntimeError("shared MIME database prerequisite unavailable")
        handler = root / "handler.py"
        log = root / "launched.jsonl"
        token_log = root / "tokens.jsonl"
        cwd_log = root / "cwd.jsonl"
        handler.write_text(
            "import json,os,sys\nfrom pathlib import Path\n"
            f"with Path({str(log)!r}).open('a') as stream:\n"
            " stream.write(json.dumps([os.fsencode(arg).hex() for arg in sys.argv[1:]])+'\\n')\n"
            f"with Path({str(token_log)!r}).open('a') as stream:\n"
            " stream.write(json.dumps([sys.argv[1:],os.getenv('XDG_ACTIVATION_TOKEN'),os.getenv('DESKTOP_STARTUP_ID')])+'\\n')\n"
            f"with Path({str(cwd_log)!r}).open('a') as stream:\n"
            " stream.write(json.dumps(os.getcwdb().hex())+'\\n')\n"
        )
        desktop = root / "data/applications/fixture.desktop"
        desktop.write_text(
            "[Desktop Entry]\nType=Application\nName=Private launch fixture\n"
            f"Exec={sys.executable} {handler} {'%F' if mode == 'native-files' else '%u' if mode == 'batch-single' else '%U'}\n"
            + ("StartupNotify=true\n" if mode == "tokens" else "") +
            "MimeType=text/plain;\n" + ("NoDisplay=false\n" if mode in ("with", "other", "default", "default-error", "catalog", "batch", "batch-single", "grouped-open", "tokens", "activation-destroy", "mixed-catalog") else "NoDisplay=true\n")
        )
        # A file manager may own file:// without being the MIME handler for documents (#21).
        manager = root / "data/applications/file-manager.desktop"
        manager.write_text(
            "[Desktop Entry]\nType=Application\nName=Private file manager\nNoDisplay=true\n"
            f"Exec={sys.executable} {root / 'wrong-handler.py'} %U\n"
            "MimeType=inode/directory;x-scheme-handler/file;\n"
        )
        (root / "wrong-handler.py").write_text("raise SystemExit('document sent to the file manager')\n")
        associations = (
            "[Default Applications]\ntext/plain=fixture.desktop;\n"
            "inode/directory=file-manager.desktop;\nx-scheme-handler/file=file-manager.desktop;\n"
        )
        mime_cache = (
            "[MIME Cache]\ntext/plain=fixture.desktop;\n"
            "inode/directory=file-manager.desktop;\nx-scheme-handler/file=file-manager.desktop;\n"
        )
        if mode == "grouped-open":
            associations += "image/png=other.desktop;\n"
            mime_cache += "image/png=other.desktop;\n"
        (root / "config/mimeapps.list").write_text(associations)
        (root / "data/applications/mimeinfo.cache").write_text(mime_cache)
        if mode == "nodisplay":
            # MIME handlers can be hidden from launcher menus without being uninstalled.
            desktop.write_text(desktop.read_text().replace("Private launch fixture", "Luciole NoDisplay fixture default"))
            for filename, extra in (
                ("common.desktop", "NoDisplay=true\nMimeType=text/plain;image/png;"),
                ("unrelated.desktop", "NoDisplay=true\nMimeType=application/pdf;"),
                ("removed.desktop", "Hidden=true\nMimeType=text/plain;image/png;"),
                ("unavailable.desktop", "TryExec=/luciole-fixture-missing-program\nMimeType=text/plain;image/png;"),
                ("no-files.desktop", "NoDisplay=true\nMimeType=text/plain;image/png;"),
            ):
                arguments = "" if filename == "no-files.desktop" else "%U"
                (root / "data/applications" / filename).write_text(
                    "[Desktop Entry]\nType=Application\n" + f"Name=Luciole NoDisplay fixture {filename}\n"
                    f"Exec={sys.executable} {handler} {arguments}\n{extra}\n"
                )
            (root / "data/applications/mimeinfo.cache").write_text(
                "[MIME Cache]\ntext/plain=fixture.desktop;common.desktop;removed.desktop;unavailable.desktop;no-files.desktop;\n"
                "image/png=common.desktop;removed.desktop;unavailable.desktop;no-files.desktop;\n"
                "application/pdf=unrelated.desktop;\n"
            )
        if mode == "remote":
            (root / "config/mimeapps.list").write_text(associations + "x-scheme-handler/sftp=fixture.desktop;\n")
            (root / "data/applications/mimeinfo.cache").write_text(mime_cache + "x-scheme-handler/sftp=fixture.desktop;\n")
        if mode == "no-handler":
            desktop.unlink()
            (root / "config/mimeapps.list").write_text(associations.replace("text/plain=fixture.desktop;\n", ""))
            (root / "data/applications/mimeinfo.cache").write_text(mime_cache.replace("text/plain=fixture.desktop;\n", ""))
        if mode in ("other", "default", "default-error", "catalog", "batch", "batch-single", "grouped-open", "tokens", "activation-destroy", "mixed-catalog"):
            for filename, title, arguments, extra in (
                ("other.desktop", "Noctalia launch fixture Café", "%U", "MimeType=image/png;"),
                ("hidden.desktop", "Hidden Café", "%U", "NoDisplay=true"),
                ("no-files.desktop", "No document arguments", "", ""),
            ):
                (root / "data/applications" / filename).write_text(
                    "[Desktop Entry]\nType=Application\n" + f"Name={title}\nExec={sys.executable} {handler} {arguments}\n{extra}\n"
                )
            if mode == "catalog":
                for index in range(150):
                    title = "Lastneedle" if index == 149 else f"Bulk {index:03}"
                    (root / "data/applications" / f"bulk-{index:03}.desktop").write_text(
                        f"[Desktop Entry]\nType=Application\nName={title}\nExec={sys.executable} {handler} %U\n"
                    )
        if mode == "mixed-catalog":
            (root / "data/applications/common.desktop").write_text(
                "[Desktop Entry]\nType=Application\nName=Private common fixture\n"
                f"Exec={sys.executable} {handler} %U\nMimeType=image/png;text/plain;\n"
            )
            (root / "data/applications/mimeinfo.cache").write_text(
                "[MIME Cache]\ntext/plain=fixture.desktop;common.desktop;\nimage/png=other.desktop;common.desktop;\n"
            )
        if mode == "dbus":
            desktop.rename(root / "data/applications/org.example.Fixture.desktop")
            desktop = root / "data/applications/org.example.Fixture.desktop"
            desktop.write_text(desktop.read_text().replace("NoDisplay=true", "NoDisplay=false")
                               + "DBusActivatable=true\nStartupNotify=true\n")
            for filename in (root / "config/mimeapps.list", root / "data/applications/mimeinfo.cache"):
                filename.write_text(filename.read_text().replace("fixture.desktop", "org.example.Fixture.desktop"))
        if mode == "default-error":
            (root / "config/mimeapps.list").unlink()
            (root / "config").rmdir()
            (root / "config").write_text("not a directory")
        guard = root / "exec-guard.py"
        refused = root / "unexpected-exec"
        guard.write_text(
            f"#!{sys.executable}\nimport os,sys\nfrom pathlib import Path\n"
            f"if sys.argv[1:3] != {[sys.executable, str(handler)]!r}:\n"
            f" Path({str(refused)!r}).write_text('unexpected desktop handler')\n"
            " sys.exit(1)\n"
            "os.execv(sys.argv[1],sys.argv[1:])\n"
        )
        guard.chmod(0o700)
        if mode.startswith("terminal-"):
            desktop.write_text(desktop.read_text().replace("NoDisplay=true", "NoDisplay=false") + "Terminal=true\n")
            bin_dir = root / "bin"
            bin_dir.mkdir()
            foot = bin_dir / "foot"
            foot.write_text(
                f"#!{sys.executable}\nimport os,sys\na=sys.argv[1:]\n"
                "assert a[0].startswith(('--working-directory=', '--dir='))\n"
                "os.chdir(a.pop(0).split('=',1)[1])\nassert a.pop(0)=='--'\nos.execv(a[0],a)\n"
            )
            foot.chmod(0o700)
            kitty = bin_dir / "kitty"
            kitty.write_text(f"#!{sys.executable}\nraise SystemExit('wrong terminal selected')\n")
            kitty.chmod(0o700)
            helper = bin_dir / "xdg-terminal-exec"
            helper.symlink_to(foot)
        paths = []
        if mode == "symlink":
            (root / "target.txt").write_text("private link target\n")
        for name in names:
            path = os.fsencode(root) + b"/" + name
            if mode == "symlink":
                os.symlink(b"target.txt", path)
            elif mode != "error":
                with open(path, "wb") as stream:
                    stream.write(
                        b"\x89PNG\r\n\x1a\n"
                        if mode == "grouped-open" and name.endswith(b".png")
                        else b"private fixture\n"
                    )
            paths.append(path)
        env = dict(os.environ)
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "GIO_LAUNCH_DESKTOP"):
            env.pop(key, None)
        env.update(
            HOME=str(root / "home"),
            XDG_CONFIG_HOME=str(root / "config"),
            XDG_CONFIG_DIRS=str(root / "no-system-config"),
            XDG_DATA_HOME=str(root / "data"),
            XDG_DATA_DIRS=str(root / "no-system-data"),
            XDG_CACHE_HOME=str(root / "cache"),
            XDG_STATE_HOME=str(root / "state"),
            XDG_RUNTIME_DIR=str(root / "runtime"),
            XDG_CURRENT_DESKTOP="",
            DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-session-bus"),
            DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"),
            GIO_LAUNCH_DESKTOP=str(guard),
            GIO_USE_VFS="local",
            GIO_EXTRA_MODULES="",
            GIO_USE_PORTALS="0",
            LUCIOLE_LOG_LEVEL="debug",
            G_DEBUG="fatal-warnings",
            XDG_ACTIVATION_TOKEN="inherited-stale",
            DESKTOP_STARTUP_ID="inherited-startup",
        )
        if mode.startswith("terminal-"):
            env.pop("GIO_LAUNCH_DESKTOP", None)
            env["PATH"] = str(root / "bin")
            env["TERMINAL"] = "kitty" if mode == "terminal-setting" else "foot" if mode != "terminal-auto" else ""
            env["TERM"] = "must-not-be-used-as-an-executable"
        result = subprocess.run(
            [binary, mode, *paths], env=env, capture_output=True, timeout=10, check=False
        )
        if result.returncode != 0:
            raise AssertionError((result.returncode, result.stdout.decode(errors="replace"), result.stderr.decode(errors="replace")))
        if mode in ("open", "native-files", "remote", "symlink", "sequential", "with", "other", "default", "batch", "batch-single", "grouped-open", "tokens", "nodisplay", "terminal-setting", "terminal-env", "terminal-with", "terminal-auto"):
            deadline = time.monotonic() + 5
            expected = [b"sftp://fixture.invalid/private.txt".hex()] if mode == "remote" else sorted(path.hex() for path in paths)
            while True:
                try:
                    actual = sorted(
                        item
                        for line in log.read_text().splitlines()
                        for item in json.loads(line)
                    )
                except (FileNotFoundError, json.JSONDecodeError):
                    actual = []
                if actual == expected:
                    break
                if refused.exists():
                    raise AssertionError(("file scheme handler overrode the document MIME handler", result.stderr))
                if time.monotonic() >= deadline:
                    raise AssertionError((actual, expected, result.stderr))
                time.sleep(0.01)
        if mode in ("batch", "batch-single", "grouped-open"):
            invocations = [json.loads(line) for line in log.read_text().splitlines()]
            expected_count = 1 if mode == "batch" else 2 if mode == "grouped-open" else len(paths)
            if len(invocations) != expected_count:
                raise AssertionError("desktop multi/single-URI dispatch ignored")
            if mode == "grouped-open":
                expected_groups = [[paths[0].hex(), paths[2].hex()], [paths[1].hex()]]
                if sorted(invocations) != sorted(expected_groups):
                    raise AssertionError((invocations, expected_groups))
        if mode == "terminal-missing" and log.exists():
            raise AssertionError("invalid terminal preference silently fell back")
        if mode.startswith("terminal-") and mode != "terminal-missing":
            directories = [json.loads(line) for line in cwd_log.read_text().splitlines()]
            if directories != [os.fsencode(root).hex()] * len(paths):
                raise AssertionError(("terminal handler used the wrong folder", directories))
        if token_log.exists():
            values = [json.loads(line) for line in token_log.read_text().splitlines()]
            if mode == "tokens":
                expected_tokens = {os.fsdecode(paths[0]): "fresh-first", os.fsdecode(paths[1]): "fresh-second"}
                if len(values) != 2 or any(expected_tokens.get(args[0]) != token or startup != token for args, token, startup in values):
                    raise AssertionError(("incorrect fresh token handoff", values))
            elif any(token or startup for _, token, startup in values):
                raise AssertionError("inherited token escaped into a child")
        if mode in ("catalog", "default-error", "mixed-catalog", "activation-destroy", "dbus") and log.exists():
            raise AssertionError("application discovery/default change launched a handler")
        if mode in ("default", "other"):
            associations = configparser.ConfigParser(interpolation=None)
            associations.read(root / "config/mimeapps.list")
            expected_default = "other.desktop" if mode == "default" else "fixture.desktop"
            if associations["Default Applications"].get("text/plain", "").rstrip(";") != expected_default:
                raise AssertionError("unexpected persisted default application")
        if mode in ("error", "no-handler") and log.exists():
            raise AssertionError("a missing file or MIME association launched an application")
        if refused.exists():
            raise AssertionError("desktop dispatch escaped its fixture")
        # Debug diagnostics identify stages and requests, without recording file payloads.
        diagnostics = result.stderr.decode(errors="replace")
        if mode not in ("catalog", "default-error", "mixed-catalog", "activation-destroy", "dbus", "terminal-missing") and "GIO completion" not in diagnostics:
            raise AssertionError("missing launch timing diagnostics")
        if mode in ("open", "sequential") and ("handler=fixture.desktop" not in diagnostics or "desktop handoff" not in diagnostics):
            raise AssertionError("missing desktop handoff timing")
        if str(root) in diagnostics:
            raise AssertionError("launch diagnostics disclosed the file location")


def main():
    binary = sys.argv[1]
    if sys.argv[2:] == ["--no-handler"]:
        # Run with filesystem isolation: Nix's GLib also searches profiles outside the XDG directories.
        run_case(binary, "no-handler", [b"no-handler.txt"])
        return
    for mode in ("terminal-setting", "terminal-env", "terminal-with", "terminal-auto", "terminal-missing"):
        run_case(binary, mode, [b"plain.txt", "space ' $() % café\n.txt".encode(), b"raw-\xff.txt"])
    run_case(binary, "open", [b"plain.txt", "space ' $() <literal> café\n.txt".encode(), b"raw-\xff.txt", b"extensionless"])
    run_case(binary, "native-files", [b"file-only.txt", "space café.txt".encode(), b"raw-\xff.txt"])
    run_case(binary, "remote", [b"unused.txt"])
    run_case(binary, "symlink", [b"linked-document"])
    run_case(binary, "sequential", [b"first.txt", b"second.txt", b"third.txt"])
    run_case(binary, "with", [b"explicit.txt", "space ' $() café\n.txt".encode(), b"raw-\xff.txt"])
    run_case(binary, "other", ["other space café.txt".encode(), b"other-raw-\xff.txt"])
    run_case(binary, "default", [b"new-default.txt"])
    run_case(binary, "default-error", [b"never-launched.txt"])
    run_case(binary, "catalog", [b"unused.txt"])
    run_case(binary, "batch", [b"grouped.txt", "space ' $() café\n.txt".encode(), b"raw-\xff.txt"])
    run_case(binary, "batch-single", [b"individual1.txt", b"individual2.txt"])
    run_case(binary, "grouped-open", [b"episode-01.txt", b"artwork.png", b"episode-02.txt"])
    run_case(binary, "mixed-catalog", [b"unused.txt"])
    run_case(binary, "nodisplay", [b"handler.txt", "space ' $() café\n.txt".encode(), b"raw-\xff.txt"])
    run_case(binary, "dbus", [b"via-bus.txt", "space café.txt".encode(), b"raw-\xff.txt"])
    run_case(binary, "tokens", [b"explicit-token.txt", b"default-token.txt"])
    run_case(binary, "activation-destroy", [b"never-launched.txt"])
    run_case(binary, "error", [b"missing.txt"])
    run_case(binary, "destroy", [b"cancelled.txt"])
    print("file launcher desktop integration checks passed")


if __name__ == "__main__":
    main()
