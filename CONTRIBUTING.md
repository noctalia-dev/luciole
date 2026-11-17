# Contributing

luciole shares its conventions with [noctalia](https://github.com/noctalia-dev/noctalia) and umbriel: same team, same style. If in doubt, match what noctalia does.

## Documentation

[docs/user/](docs/user/index.md) contains guides for using Luciole. [docs/design/](docs/design/README.md) documents subsystem ownership and implementation contracts. Update relevant user guides with behavior, configuration and shortcut changes; update design notes with architectural or contract changes in the same increment. Keep code references and index links accurate.

[PACKAGING.md](PACKAGING.md) remains the dependency/install guide and must change with packaging requirements. Documentation is ordinary Markdown checked by `just lint-prose`. `tools/sync-docs.sh` publishes docs/user/ to [docs.noctalia.dev/luciole](https://docs.noctalia.dev/luciole/) through a sibling checkout of noctalia-docs; no documentation formatter is needed.

## Code Style

Run `just format` before committing and `just format-check` to check. Code formatting requires [clang-format](https://clang.llvm.org/docs/ClangFormat.html) 23 or newer and uses the same `.clang-format` as noctalia; the recipes print the version in use. Keep editor formatting on the same version as your command-line formatter.

`just configure` creates a root `compile_commands.json` symlink to the selected Meson build directory, so clangd and clang-tidy see the build you are working in.

The repo includes `lefthook.yml`. Run `lefthook install` to install the pre-commit hook; it runs `just format-code` when C++ files are staged and refreshes the git index for tracked formatting changes.

Owned text files must not contain em dashes or trailing whitespace and must end with a newline; `just lint-prose` checks this.

### Lint

Code must pass clang-tidy with warnings treated as errors, using the same `.clang-tidy` check set as noctalia. `src/.clang-tidy` adds one project rule on top: functions in `src/` stay under 250 lines (`readability-function-size`); split a feature out instead of growing an orchestrator. `just lint` checks all of `src/`; `lib/` is linted in noctalia-shell. While iterating, lint only the `.cpp` files you touched:

```sh
clang-tidy -p build-debug --warnings-as-errors='*' <touched .cpp files...>
```

Headers are linted through the `.cpp` files that include them. Fix every finding before opening a pull request. Prefer the modern idioms the checks enforce: `auto`, ranges, `std::format`/`std::print`, `make_unique`, scoped locks, no C-style casts, uppercase literal suffixes (`1.0F`).

### Naming Conventions

|                        | Convention           | Example             |
| ---------------------- | -------------------- | ------------------- |
| Files                  | snake_case           | `directory_model.h` |
| Directories            | snake_case           | `browser/`          |
| Types / Classes        | PascalCase           | `DirectoryModel`    |
| Functions / Methods    | camelCase            | `selectAll()`       |
| Variables / Parameters | camelCase            | `scrollAnchor`      |
| Private members        | m_camelCase          | `m_selection`       |
| Enumerators            | PascalCase           | `SortField::Name`   |
| Macros                 | SCREAMING_SNAKE_CASE | `TEST_CHECK`        |

Scoped enums (`enum class`) use an explicit `std::uint8_t` underlying type where it makes sense.

Getters are the noun, without a `get` prefix: `snapshot()`, `selection()`.

clang-format inserts braces around every control statement body and clang-tidy enforces the naming table for enumerators and private members.

### Source Conventions

- `src/` and `lib/` are the include roots; project includes are relative to them (`#include "app/application.h"`, `#include "ui/style.h"`).
- Headers use `#pragma once` and sit beside their sources; there is no separate `include/` tree.
- Brace the body of every `if`, `else`, `for`, and `while`, including single statements.
- Use `[[nodiscard]]` only where ignoring the result is likely a bug: a call with side effects that reports failure or hands back something the caller must own, consume or keep (a handle, token or id), and `&&` builders. Mark result and status types with `struct [[nodiscard]] Name` instead of each function that returns them. Getters and pure functions go without it.

### Comments

Comments explain what the current code does and any non-obvious constraint a reader needs. Do not narrate history, rejected alternatives, or "why we don't do X"; git holds that. If a comment is longer than the code it describes, cut it down.

### Module layering

Each directory under `src/` builds as one static library in `meson.build` (`ui/browser`, `ui/controls`, and `ui/dialogs` together form `ui`), and its dependencies list only the modules it may include. A module includes only these:

- `model`: plain option and enum types; nothing from `src/` or `lib/`.
- `fs`: GIO file access, locations, atomic writes, transfer result types; no other module.
- `event_loop`: poll sources for the main loop; `lib/core`.
- `config`: settings and state; `model`, `fs`.
- `browser`: directory models and controllers without view code; `model`, `fs`, `config`, `lib/core`. Never `lib/render`, `lib/ui`, `lib/wayland`, or `src/ui`.
- `operations`: file jobs; `fs`, `model`.
- `imaging`: image decoders and the shared `image_limits.h`; `fs`, `lib`.
- `system`: icon themes and desktop entries; `lib`.
- `thumbnails`: thumbnail cache, workers, previews, and the file icon cache; `imaging`, `system`, `fs`, `model`, `lib`.
- `clipboard` (Wayland clipboard and drag and drop payloads) and `platform` (launchers, desktop integration): `fs`, `lib`.
- `ui`: views and controls; any module above. `ui/controls` may include `browser/`, never the reverse.
- `app`: everything. Its internal `app-jobs` static library contains OperationHub and the activity snapshot adapter, depending only on `browser` and `operations`, so lifecycle tests can link it without window orchestration.

All modules share the `src/` include root, so Meson cannot reject an include of a forbidden `src/` module; review catches those. `model`, `fs`, `config`, `browser`, and `operations` do not depend on `noctalia_ui_dep`, so any `lib/` include there fails to compile; give `browser` that dependency only once it needs `lib/core`.

GIO file types stay behind `fs/`: public headers outside `fs/`, `thumbnails/`, and `imaging/` never mention `GFile`, `GFileInfo`, `GFileType`, or `GCancellable`. `browser`, `config`, `ui`, and `app` use the typed `fs/` API (`Location`, `FileType`, `CancelToken`, `runJob`, `fs/file_query.h`). Modules that drive GIO themselves (`operations`, `thumbnails`, `imaging`, `platform`, `clipboard`) reach it through `fs/gio_interop.h` in their `.cpp` files.

Inside `app`, `WorkspaceApplication` owns windows, tabs, and the `OperationHub`, which runs every file job independently of tabs and hands a job to another tab when its tab closes. Each tab is a `BrowserPage` (`app/page/`) that only orchestrates feature controllers; a new feature gets its own controller behind the `PageContext` interface (see the comment in `app/page/page_context.h`). Actions are declared once in `browser/action_registry.cpp`, with their handler row in `BrowserPage::runAction`.

### Palette colors

Fills, tints, and borders use a palette role: `SurfaceContainerLow` to `SurfaceContainerHighest` for raised backgrounds, the `*Container` roles for tinted fills, and `OutlineVariant` for borders. Do not blend a role with a hardcoded alpha (`colorSpecFromRole(ColorRole::Primary, 0.16F)`) to get a color the palette does not have; if no role fits, propose a new one in noctalia-shell. Alpha is for real transparency only: shadows, scrims, overlays, and fading content that is disabled, cut, hidden, or being dragged (`Node::setOpacity`), ideally through a named constant. `just role-alpha-check` (run in CI) rejects any role-plus-literal-alpha in `src/`; `lib/` is checked by the same rule in noctalia-shell.

### Code shared with noctalia-shell

`lib/` (UI controls, render, Wayland surfaces, core helpers) is copied from [noctalia-shell](https://github.com/noctalia-dev/noctalia); do not edit it here. Make generic fixes there first and resync the copy, so the two trees do not drift apart.

## Testing

Write test checks with `TEST_CHECK` and `TEST_NEAR` from `tests/test_check.h`, never `assert` or a per-file helper. `assert` compiles away under `NDEBUG`, so a release build would pass a broken test silently; `TEST_CHECK` prints the failed expression with its file and line and exits non-zero. The header refuses to compile with `NDEBUG`, so test executables set `override_options: ['b_ndebug=false']` in `tests/meson.build`. Wrap an expression that contains a top-level comma in an extra pair of parentheses: `TEST_CHECK((ids == std::vector<int>{1, 2}))`.

Tests are listed in the table in `tests/meson.build`: each links the module libraries it exercises and never lists `src/` files.

## Commit Messages

Use [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): summary`, or `type: summary` when no scope helps, with lowercase types such as `feat`, `fix`, `refactor`, `chore`, `test`, or `docs`. Keep the summary concise, imperative, and specific.

Default to the summary line alone. Add a body only when it carries information a reader would otherwise miss: a non-obvious root cause, a constraint that forced the approach, a behavior change users will notice, or a `Fixes #N` reference. Do not restate the diff or list touched files.
