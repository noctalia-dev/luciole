# Packaging Luciole

Notes for distribution packagers.
The dependency and install declarations in [meson.build](meson.build), [lib/meson.build](lib/meson.build) and [nix/package.nix](nix/package.nix) are the source of truth. This guide describes the current implementation; planned features are not packaging requirements.

For application usage and settings, see the [user guide](docs/user/index.md). Implementation contracts live under [docs/design/](docs/design/README.md). These source documentation pages are not additional installed assets.

## Package description

Use the description from the Nix package metadata:

> A native Wayland file manager using Noctalia rendering and controls (in development)

## Identity

| Field                  | Value                                                                 |
| ---------------------- | --------------------------------------------------------------------- |
| Package / binary       | `luciole`                                                             |
| Application name       | Luciole                                                               |
| Wayland application ID | `dev.noctalia.luciole`                                                |
| Homepage               | <https://github.com/noctalia-dev/luciole>                             |
| Version                | [VERSION](VERSION), read by Meson                                     |
| License                | MIT ([LICENSE](LICENSE)); preserve the bundled Tabler font notice too |
| D-Bus names            | `org.freedesktop.FileManager1`; `dev.noctalia.luciole` (single instance) |

Use **Luciole** as the display name and lowercase `luciole` for the command/package and application-owned paths. Nix modules expose `programs.luciole`; environment overrides use the `LUCIOLE_` prefix. There are no legacy-name aliases or automatic configuration/state migrations.

Luciole is a Linux Wayland application. It uses OpenGL ES and Pango/Cairo directly, without a Qt or GTK widget layer. GdkPixbuf and Glycin are image-decoding dependencies.

The Nix flake declares `x86_64-linux` and `aarch64-linux`. Builds and runtime checks have been performed on x86_64; ARM outputs have only been evaluated. Other architectures and musl are not currently verified. Do not copy Noctalia's architecture or libc support claims into Luciole package metadata.

## Build

Required tools are Meson 1.1 or newer, Ninja, pkg-config, wayland-scanner, a C compiler and a C++23 compiler with a compatible standard library. Luciole uses library facilities including `std::print`, `std::expected` and `std::move_only_function`; accepting `-std=c++23` alone is insufficient. GCC/libstdc++ 16.2 is verified. An older compiler minimum and Clang/libc++ compatibility have not been established for Luciole. Meson adds the experimental-library arguments when it detects libc++.

Use the intended final prefix at configuration time and a separate staging directory for installation:

```sh
meson setup build-package --buildtype=release --prefix=/usr \
  -Dtests=disabled -Dnative_optimizations=false
meson compile -C build-package
DESTDIR="$PWD/pkgroot" meson install --no-rebuild -C build-package
```

`just` is a development convenience, not a package build dependency. The build works from a complete source archive without repository metadata. Include `lib/`, its assets/protocols and the application `assets/` tree in that archive. Do not fetch or sync Noctalia during configuration or compilation.

Luciole builds its copied Noctalia UI library as static internal archives. It does not use an installed Noctalia shared library, a sibling checkout, a submodule or a running Noctalia shell. Luciole supplies its own GdkPixbuf/Glycin decoder; the library's optional Wuffs codec pack is not enabled. Its JPEG XL/WebP codec-pack dependencies and third-party source tree are not Luciole build requirements.

### Meson options

| Option                 | Default   | Packaging guidance                                                                                                                    |
| ---------------------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `tests`                | `auto`    | Auto enables tests only for unsanitized debug builds. Use `disabled` for the ordinary release package or `enabled` for a check build. |
| `native_optimizations` | `false`   | Keep false for redistributable binaries; true adds `-march=native -mtune=native`.                                                     |
| `build_revision`       | `unknown` | Optional explicit revision supplied by the packager. No repository command runs to obtain it.                                         |

Standard Meson options such as `prefix`, `bindir`, `datadir`, `buildtype`, `b_lto` and `b_sanitize` remain available. For example, pass `-Dbuild_revision=distribution-release` when the distribution wants explicit build metadata.

Release builds enable section garbage collection and `--as-needed`. LTO is optional. GNU LTO requires compiler-aware archive tools such as `gcc-ar` and `gcc-ranlib`; the Nix package configures these explicitly. Configure a fresh build directory when changing the toolchain.

## Dependencies

### Build and link dependencies

Names below are pkg-config modules or header paths, not universal distribution package names. Install their development files as required by the distribution.

| Dependency                                        | Requirement / purpose                                                                          |
| ------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `wayland-protocols`                               | 1.49 or newer; build-time protocol definitions                                                 |
| `wayland-client`, `wayland-egl`, `wayland-cursor` | Wayland connection, EGL surfaces and cursor fallback                                           |
| `egl`, `glesv2`                                   | EGL and OpenGL ES; the shared library also has fallback detection through wayland-egl/libepoxy |
| `gio-2.0`, `gio-unix-2.0`                         | 2.76 or newer; filesystem integration and Unix desktop launchers, from GLib development files  |
| `glib-2.0`                                        | GLib core services                                                                             |
| `freetype2`, `fontconfig`                         | Font discovery and rendering                                                                   |
| `cairo`, `cairo-ft`                               | Cairo rendering with FreeType integration                                                      |
| `pango`, `pangocairo`, `pangoft2`                 | Pango 1.50 or newer, including Cairo/FreeType integration                                      |
| `harfbuzz`, `xkbcommon`                           | Text shaping and keyboard input                                                                |
| `gdk-pixbuf-2.0`                                  | 2.44 or newer, built with Glycin support                                                       |
| `glycin-2`                                        | 2.0.1 or newer, compatible with the chosen GdkPixbuf build                                     |
| `librsvg-2.0`, `libpng`                           | SVG/theme artwork and PNG cache support                                                        |
| `libarchive`, `liblzma`                           | Native archive handling and bounded XZ decoding                                                |
| `libacl`                                          | POSIX permission/ACL support                                                                   |
| `tomlplusplus`                                    | 3.4 or newer                                                                                   |
| `nlohmann_json`                                   | JSON support                                                                                   |
| `stb/stb_image_resize2.h`                         | System stb resize2 header; the older resize header is insufficient                             |
| Threads                                           | System thread support                                                                          |

Use mutually compatible GdkPixbuf, Glycin and loader packages. Meson's minimum checks do not establish compatibility between every later release. The pinned Nix package selects GdkPixbuf 2.44.6 with Glycin 2.1 and builds librsvg against that same GdkPixbuf variant; that is a Nix compatibility choice, not a request for every distribution to downgrade. Installed Glycin loaders determine raster format coverage. AVIF/HEIF can require an additional loader dependency such as libheif.

### Runtime integration and helpers

The linked libraries need their ordinary runtime packages. GVfs and a session D-Bus connection are required runtime dependencies for the shipped Trash interface. The remaining data, providers and executables supply the features listed below:

| Runtime component                             | Role                                                                               |
| --------------------------------------------- | ---------------------------------------------------------------------------------- |
| Wayland compositor with EGL/OpenGL ES support | Graphical window; Noctalia and Umbriel are not required                            |
| Fontconfig and usable text fonts              | Application text; the bundled glyph font does not replace ordinary fonts           |
| Shared MIME database                          | Type descriptions, application associations and provider format detection          |
| Icon theme                                    | File, folder and application artwork; the Nix wrapper supplies Adwaita             |
| Desktop settings schemas and GSettings backend | `org.gnome.desktop.interface` icon-theme selection and live changes; normally gsettings-desktop-schemas and dconf |
| Glycin loaders                                | Raster images in thumbnails, previews and metadata                                 |
| Session D-Bus and GVfs                        | Required for Trash browsing, restore and emptying; additional backends enable remote locations |
| `glib-networking`                             | GIO TLS implementation for secure WebDAV                                           |
| `ffmpegthumbnailer`                          | Recommended runtime helper for shared video-thumbnail filmstrip borders; CLI only, no development headers |
| `ffmpeg`, `ffprobe`                           | Representative video frames, audio artwork and media metadata                        |
| `pdftoppm`, `pdfinfo`                         | PDF thumbnails and metadata, normally supplied by Poppler utilities                |
| `7zz`, `7z` or `7za`                          | System 7-Zip CLI for encrypted 7z input, searched in that order                    |
| `xdg-terminal-exec`                           | Optional shared terminal preference/discovery for automatic terminal selection; explicit commands and known-terminal fallback also work |
| Associated applications                       | Opening files through the user's GIO MIME associations                             |

Missing optional helpers make their associated feature unavailable where no alternative provider exists; they do not prevent ordinary native directory browsing. Terminal actions can also use a configured command, inherited TERMINAL or a supported directly discovered terminal. Recommend or depend on them according to the features the package advertises. The Nix wrapper exposes FFmpeg, ffmpegthumbnailer, Poppler utilities, 7-Zip and xdg-terminal-exec on `PATH`.

Archive support uses the system libarchive and its enabled formats/filters. Unencrypted RAR input depends on that build's capabilities; encrypted RAR is outside the implemented scope. Encrypted 7z input uses the system CLI through the bounded worker transport. There is no py7zr requirement, 7-Zip SDK build dependency, `7z.so` path or private C++ ABI contract. `LUCIOLE_7Z` can select an absolute executable path for an unusual installation. File Roller, Ark, unrar and a Samba server are not required by Luciole.

### Incoming archive drags

Ordinary URI-list archive drags use existing transfer support. KDE deferred-extraction drags additionally use the existing GIO session D-Bus connection to reach the running source's advertised unique name and object path. The source must expose `org.kde.ark.DndExtract.extractSelectedFilesTo`; it owns extraction and its password/conflict/progress UI. The method acknowledgement is not extraction completion. This path requires a native destination directory and does not support provider-only URIs.

Producers that extract to temporary files are responsible for their own helper programs. In the isolated LXQt Archiver 1.4.0 check, supplying its selected `7z` helper allows multi-entry preparation and Copy-only URI export. That does not make this specific helper mandatory for Luciole's incoming URI handling or require installing every external archiver. Preparation and export capabilities depend on the producer's build and runtime packages. Ordinary URI drops allow up to 120 seconds for source data; the shorter text/metadata read limits remain separate.

No Ark, Qt, KIO, X11 library or separate helper package becomes a Luciole dependency. Do not add an archiver selector or install Ark merely to enable normal archive handling. A producer must export a supported drag contract. X11 Direct Save-only offers remain unsupported; an Xwayland source is not automatically compatible solely because its window appears in the compositor. The `extraction-launcher` check uses the existing test-only `dbus-daemon` requirement with a private bus and no desktop services.

### Shared video thumbnail appearance

Recommend `ffmpegthumbnailer` when packaging video thumbnails. Luciole uses its `-f` filmstrip filter, matching the helper’s [standard desktop thumbnailer entry](https://github.com/dirkvdb/ffmpegthumbnailer/blob/master/dist/ffmpegthumbnailer.thumbnailer) commonly used by PCManFM-Qt. Those borders are cached PNG pixels, not icon-theme artwork. Only the executable and its ordinary runtime dependency closure are needed; do not add its development headers or link Luciole against its C++ library. Nix packages/development shells/checks expose it on PATH, and Arch CI installs it.

Luciole first selects and validates a bounded frame through its existing FFmpeg path, then passes a private re-encoded PNG to ffmpegthumbnailer. The helper never receives the original media path, retaining source pixel/protocol limits on older releases. This uses established `-i`, `-o`, `-s`, `-t`, `-f` and `-c` options, without requiring newer protocol flags. A missing/failing helper leaves a plain thumbnail available. Native video previews generate clean frames separately and do not replace the framed shared thumbnail. Existing third-party cached images retain their original appearance. Luciole does not currently execute arbitrary `.thumbnailer` registrations or apply icon-theme-dependent borders.

Availability checked in official catalogs on 2026-11-12:

| Distribution | Runtime package |
| --- | --- |
| Arch | [`ffmpegthumbnailer`](https://archlinux.org/packages/extra/x86_64/ffmpegthumbnailer/) in Extra |
| Fedora | [`ffmpegthumbnailer`](https://packages.fedoraproject.org/pkgs/ffmpegthumbnailer/ffmpegthumbnailer/); retain the library dependency closure |
| Debian 13 | [`ffmpegthumbnailer`](https://packages.debian.org/trixie/ffmpegthumbnailer); the executable package, rather than only `libffmpegthumbnailer-dev` |

The thumbnail test suite requires this helper in addition to its existing FFmpeg/image/PDF providers and compares generated pixels with its real filmstrip output. Missing/failing-helper cases separately verify the plain-frame fallback. Catalog checks establish availability, not runtime testing on every distribution.

### Required Trash runtime

Declare GVfs as a runtime dependency for packages shipping Luciole's Trash interface, rather than only recommending it for network access. Include the GIO module, `gvfsd`, `gvfsd-trash`, mount data and session D-Bus activation files. GLib can move native files into the local trash without GVfs, but that does not provide the `trash:///` view used for browsing, restoration, size reporting and Empty Trash. No GVfs C++ development dependency is needed.

The following package splits were checked in official catalogs on 2026-11-12:

| Distribution | Required Trash runtime |
| --- | --- |
| Arch | [`gvfs`](https://archlinux.org/packages/extra/x86_64/gvfs/files/), including the GIO module, daemon and Trash backend; install with `sudo pacman -S gvfs` |
| Fedora | [`gvfs`](https://packages.fedoraproject.org/pkgs/gvfs/gvfs/fedora-45.html), which depends on `gvfs-client` and includes `gvfsd-trash` |
| Debian 13 | [`gvfs`](https://packages.debian.org/trixie/gvfs) for the GIO module and [`gvfs-daemons`](https://packages.debian.org/trixie/amd64/gvfs-daemons/filelist) for the daemon, activation and Trash backend; retain the dependency closure |

A usable session bus and daemon activation are required in addition to the installed module. A GNOME desktop session is not required. Do not force `GIO_USE_VFS=local` in the packaged application or session: the headless tests use that override deliberately and do not establish working Trash integration. Verify installed-session access with `gio list trash:///`, then test Trash/Restore/Empty Trash with disposable files in an isolated session. Merely moving a file to Trash is insufficient verification of the full interface.

The Nix package wrapper and dev shell already expose the GVfs GIO module. On NixOS, enable `services.gvfs.enable = true;` to register the daemon and its session activation through the [system GVfs module](https://raw.githubusercontent.com/NixOS/nixpkgs/master/nixos/modules/services/desktops/gvfs.nix). Luciole's NixOS module installs the application; Home Manager/hjem configuration and the application wrapper do not independently enable that system service. Additional protocol providers below remain separate from this baseline requirement. `gvfs-fuse` is not needed for Luciole's direct GIO Trash access.

### GVfs providers across distributions

Luciole uses GIO directly, so these transfers do not require exposing remote mounts through FUSE. `gvfs-fuse` is useful when other applications need POSIX paths into those mounts. A session D-Bus connection and the corresponding backend must be available. The current Luciole UI uses locations already mounted/authenticated through another GIO client; its own connect-to-server and credential dialogs are pending.

The following provider packages were checked in official catalogs on 2026-11-10. These are runtime feature dependencies, not additional C++ headers. Their package managers supply the supporting client libraries.

| Distribution / catalog | SMB and WebDAV providers                                                                                                                                                                       | TLS                                                                                              |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| Arch Extra             | `gvfs`, [`gvfs-smb`](https://archlinux.org/packages/extra/x86_64/gvfs-smb/) and [`gvfs-dnssd`](https://archlinux.org/packages/extra/x86_64/gvfs-dnssd/files/); the latter contains `gvfsd-dav` | [`glib-networking`](https://archlinux.org/packages/extra/x86_64/glib-networking/)                |
| Fedora 44/45           | [`gvfs`](https://packages.fedoraproject.org/pkgs/gvfs/gvfs/fedora-44.html) includes WebDAV; [`gvfs-smb`](https://packages.fedoraproject.org/pkgs/gvfs/gvfs-smb/) adds SMB                      | [`glib-networking`](https://packages.fedoraproject.org/pkgs/glib-networking/glib-networking/)    |
| Debian 13              | `gvfs` and [`gvfs-backends`](https://packages.debian.org/trixie/gvfs-backends)                                                                                                                 | [`glib-networking`](https://packages.debian.org/trixie/glib-networking)                          |
| Ubuntu 26.04 LTS       | `gvfs` and [`gvfs-backends`](https://packages.ubuntu.com/resolute/gvfs-backends)                                                                                                               | [`glib-networking`](https://packages.ubuntu.com/resolute/glib-networking)                        |
| Alpine 3.22 community  | `gvfs`, [`gvfs-smb`](https://pkgs.alpinelinux.org/package/v3.22/community/x86_64/gvfs-smb) and [`gvfs-dav`](https://pkgs.alpinelinux.org/package/v3.22/community/x86_64/gvfs-dav)              | [`glib-networking`](https://pkgs.alpinelinux.org/package/v3.22/community/x86_64/glib-networking) |
| Pinned Nixpkgs         | `gvfs` supplies both backends; private real-server checks pass                                                                                                                                 | `glib-networking` in the Luciole wrapper/dev shell                                               |

Catalog availability does not establish that a distribution release satisfies all the other build requirements or that Luciole has been tested on that distro. openSUSE catalog/spec access was blocked during the audit, so its full package split is not marked verified here.

Normal native packages use the distribution's GIO module search paths. Do not copy Nix store paths or require users to export `GIO_EXTRA_MODULES`. The Nix wrapper and dev shell set it because those modules live in separate store outputs. The assignment inside `flake.nix`'s `preCheck` is scoped to that test derivation. TLS validation remains with the provider and system trust configuration.

## Installed layout

With the default relative `datadir=share`, Meson installs:

```text
<prefix>/bin/luciole
<prefix>/share/luciole/assets/fonts/noctalia-tabler.ttf
<prefix>/share/luciole/assets/fonts/tabler.json
<prefix>/share/luciole/assets/fonts/tabler-icons-license.txt
<prefix>/share/luciole/templates/colors.toml
<prefix>/share/dbus-1/services/org.freedesktop.FileManager1.service
<prefix>/share/applications/dev.noctalia.luciole.desktop
<prefix>/share/icons/hicolor/scalable/apps/dev.noctalia.luciole.svg
<prefix>/share/bash-completion/completions/luciole
<prefix>/share/zsh/site-functions/_luciole
<prefix>/share/fish/vendor_completions.d/luciole.fish
<prefix>/share/licenses/luciole/LICENSE
```

The fonts directory is required at runtime. Keep the Tabler license alongside its font/metadata and preserve applicable source notices. The palette template is a Noctalia integration resource, not an automatically installed user configuration.

The desktop entry and scalable icon are named after the Wayland app id `dev.noctalia.luciole`, so compositors and launchers match the window to its icon. The entry declares `inode/directory` support but does not make Luciole the default file manager. Current Meson rules do not install an AppStream/metainfo file, manual page or systemd unit. Do not assume the sibling projects' installed files exist here.

### Asset lookup and staging

The configured `<prefix>/<datadir>/luciole/assets` path is compiled into the binary. `LUCIOLE_ASSETS_DIR` explicitly overrides it; an invalid override is an error. Luciole does not use the shell's portable-bundle asset discovery chain. A source build can run with:

```sh
LUCIOLE_ASSETS_DIR="$PWD/lib/assets" ./build-package/luciole
```

Do not leave that development override in an installed launcher. `DESTDIR` stages files without changing the compiled runtime paths. A packaging smoke test that starts the staged binary before publication must supply its staged assets explicitly; the final installed binary should work without the override.

### D-Bus activation

The generated `org.freedesktop.FileManager1.service` invokes the configured absolute binary path with `--file-manager-service`. Keep that path aligned with the installation prefix. Its graphical activation needs the session's Wayland environment.

This is an on-demand session service, not an always-running background daemon. Installing it does not register an `inode/directory` MIME default. The generic service name can overlap another file manager's activation provider; handle provider selection through the distribution's packaging policy rather than silently overwriting another package's file.

Luciole also owns its application name, `dev.noctalia.luciole`, and exports `org.freedesktop.Application` at `/dev/noctalia/luciole`. A later `luciole` launch hands its folder and `XDG_ACTIVATION_TOKEN` to the running process and exits. No service file is installed for that name; the desktop entry still starts the binary, which performs the handoff.

### Shell completions

Meson installs the static Bash, Zsh and Fish scripts listed above. The binary also supports `completions bash`, `completions zsh` and `completions fish` without a compositor. Packaging does not need to execute the target binary to generate completion files, which also permits cross builds to install them.

## Configuration and writable data

The application loads internal defaults, then the user's base configuration and GUI overrides:

| Path                                     | Ownership                                            |
| ---------------------------------------- | ---------------------------------------------------- |
| `$XDG_CONFIG_HOME/luciole/config.toml`   | User/declarative base configuration                  |
| `$XDG_CONFIG_HOME/luciole/settings.toml` | Writable GUI preference overrides                    |
| `$XDG_CONFIG_HOME/luciole/colors.toml`   | Optional palette handoff                             |
| `$XDG_STATE_HOME/luciole/state.toml`     | Application-owned persistent state, including Places |
| `$XDG_CACHE_HOME/thumbnails/`            | Regenerable freedesktop thumbnail cache              |
| `$XDG_DATA_HOME/luciole/scripts/`        | User-installed executable context-menu scripts       |

Unset XDG config/state/cache roots use `~/.config`, `~/.local/state` and `~/.cache`. Luciole does not currently search `/etc/xdg` or a packaged default config. Do not install per-user `settings.toml`, state or caches from a system package. Keep the GUI override file writable even when `config.toml` is an immutable declarative file. Valid external configuration changes apply automatically. Unknown configuration keys warn and are ignored at runtime, allowing obsolete or newer-version preferences to coexist with recognized settings. GUI saves retain unknown values. The explicit `config validate` command still rejects unknown keys; known-value/schema/TOML errors remain errors. Do not package a migration that silently removes unknown settings.

Supported creation/rename/copy/native-move Undo/Redo is process-local and needs no additional package, provider or writable state directory. File/link creation and replay use private owner-only `.luciole-create-*` staging directories beside the destination and non-overwriting publication; folders are created with exclusive native mkdir. Mounted Linux procfs is required for pinned-descriptor access. Normal completion removes them; a failed compensation can retain an item there and reports its recovery path. Never recursively clean these directories as package cache. An undone document can retain disk space through an open unlinked inode until the current history is discarded or Luciole exits; final descriptor release runs on a worker. Native move history uses held parent/item descriptors for same-filesystem native moves and is optional when resources are insufficient. Plain moves need no private storage or content copy. Supported replacements retain the actual displaced destination, and merges retain emptied original source folders in owner-only `.luciole-create-*` directories at the job retention root, outside newly moved ancestors. These use the same verified storage/cleanup path as copy history, with a combined 4096 moved-root/merge-container cap, bounded 16384-entry/64-depth tree capture and descriptor headroom reserved for up to ten descriptors per effect. Restored source trees validate before Redo; unexpected children are refused, never recursively removed as merge cleanup. Copy history for native destinations, including provider downloads, and native link history retain inactive versions, including displaced previous destinations for supported replacements, in the same owner-only `.luciole-create-*` directories while Undo or Redo is available; these can contain complete folder trees. Capture is capped at 4096 changed roots, 16384 entries counting both versions, 64 levels per stored subtree and descriptor headroom reserved for up to seven descriptors per effect. Unreadable/unsupported/over-budget trees can leave Replace without history. Previous hard-link aliases use bounded job-wide revision tracking, capped at 32768 inode identities; outside aliases are never scanned or mutated. Normal cleanup unlinks only captured private entries, preserving remaining aliases and their data. Exact repeated destinations coalesce to one Copy/Link result; Move additionally retains displaced incoming versions for return to their original sources. These versions continue using disk space until history is discarded. Native Copy can coalesce a newly copied ancestor with later recorded merged children into one verified tree. Copy/link replacement stages live at the job destination, outside that copied ancestor; each stage pins its own parent descriptor. Native Move also preserves all original source inodes when later sources merge into a newly moved ancestor. These jobs capture bounded before/after tree proofs; initial snapshots and final overlapping proof views each have a shared 16384-entry limit. Retained predecessors and emptied source containers use a pinned same-filesystem job/branch root, outside moved ancestors. Plain unrelated whole-folder moves remain non-traversing. Copy can also restore an existing parent replaced after earlier child copies in the same job. It reconstructs the original tree only inside retained storage, keeps copied intermediate versions at the pinned job root outside that parent, and publishes history only after verification. Cancelled/failed reconstruction preserves affected versions for inspection. Native Move ancestor replacements retain each displaced parent/version separately and replay the original events in reverse for Undo and forward for Redo. Their derived original-parent manifests share a separate 16384-entry/64-depth bound; affected incoming directories are captured on demand. The existing pinned storage, worker cleanup and explicit inspection-path rules apply. Failed optional capture preserves affected retained versions for inspection. Copy merges retain only affected children. Provider downloads retain their history data locally and Redo does not require the server. SFTP new-name and replacement Copy history use the server-side retention described below. Other remote copy effects and provider/cross-filesystem/cross-mount/partial/retried moves remain outside history; separate bind mounts can require copy/remove even when they share a backing device. Redo republishes retained inodes without needing the original source. Normal record discard/clean shutdown verifies and removes only owned unchanged data on workers; incomplete/changed data is retained with an explicit recovery report and stderr path. Never package-clean these directories. A crash can leave retained items for manual recovery, and startup does not delete or resume them automatically. No recovery journal or restart restoration of history is implemented.

SFTP copies to new names or replacement destinations can retain one process-local history record using owner-only `.luciole-history-*` directories and private marker files on the destination server. Undo retains the copied payload there; Redo renames it back without the original source. Replacement history retains the verified predecessor as well; Undo restores it and Redo restores the copied version. These are separate server renames, not an atomic exchange. This uses existing GLib/GIO SHA-256 support and the installed GVfs SFTP backend, with no new dependency. Capture is bounded to 128 roots, 16384 entries, 64 levels and 256 MiB of regular-file contents; proofs require extra server reads. Both replacement versions count toward the content/tree budget. Folder Merge, dependent repeated destinations, other destination protocols and retried/partial jobs remain excluded. Verified cleanup runs on workers and requests cancellation after five seconds; backend cancellation support determines actual latency. Lost connectivity, changed data or unknown entries preserve remaining storage and produce an inspection path in File Operations and stderr. Never package-clean these directories or promise native inode/conditional-mutation guarantees for a provider. Crash recovery remains manual.

### Programs and custom scripts

Ordinary Open uses the user's associated application. The separate Run as a Program and Run in Terminal actions execute native ELF programs or executable scripts with an interpreter line. Luciole does not grant executable permissions automatically. Application `.desktop` files require an explicit Launch Once confirmation showing their captured name and command; Luciole does not save trust or change their permissions. Supported launchers have `Type=Application` and an `Exec` command. GIO handles their argument fields and declared working directory. Terminal=true launchers use Luciole's terminal selection policy, also used by ordinary Open/Open With and terminal actions: browser.terminal_command, then inherited TERMINAL, then xdg-terminal-exec and supported executable discovery. TERM is not used. The choice is application-local; Luciole does not rewrite shared terminal preferences or desktop files. Standalone `%k` receives the original launcher URI; quoted or embedded `%k` is rejected. Successful launching means handoff, not that an application has become ready or exited successfully.

The Scripts submenu lists executable files from `$XDG_DATA_HOME/luciole/scripts`, defaulting to `~/.local/share/luciole/scripts`. Open Scripts Folder creates it on demand. Packages should not populate a user's directory. No scripting language is required by Luciole itself; each user script needs its own declared interpreter. The script receives selected native paths as separate arguments, with the current native browse folder as its working directory. A background-menu invocation supplies no selected paths. Filenames are never interpolated into a shell command. Remote and Trash locations are outside this script increment.

The application/scripts directories and script targets must be owned by the user or root and not writable by group or others. Symlinks to qualifying executable targets are accepted, including declaratively managed read-only files. Hidden files, `.desktop` entries and non-executable files do not become script actions. Listing is flat and bounded to 64 actions and 1024 inspected children; launch arguments are limited to 4096 paths and 1 MiB. Changed files are refused on revalidation, but native execution uses the original pathname and does not promise atomic protection against a concurrent hostile writer. Launched programs continue independently of Luciole.

Validate a candidate base configuration without a compositor:

```sh
luciole config validate /path/to/config.toml
```

## Nix integration

[nix/package.nix](nix/package.nix) is the package recipe; [flake.nix](flake.nix) supplies the package, dev shell, checks, overlay and NixOS/Home Manager/hjem module exports. [default.nix](default.nix) exposes the non-flake package/module entry points.

```sh
nix build path:.#default
nix develop path:. -c just build debug
nix flake check path:.
```

The wrapper and development shell expose the desktop settings schema through `XDG_DATA_DIRS` and dconf through `GIO_EXTRA_MODULES`. Without the schema, icon selection falls back to GTK settings.ini, which can name an older theme. Native packages should expose the corresponding schema/backend using their normal desktop integration. Tests additionally require `glib-compile-schemas` for private schema fixtures.

The wrapper supplies runtime helper paths, GVfs/TLS modules, Glycin loader data, the shared MIME database and Adwaita icons without requiring the Noctalia shell. Assets use the package's own compiled store path. Home Manager and hjem can generate `config.toml` from an attribute set, TOML string or file path and validate it at build time when a package is selected. Their empty default settings leave the base file absent. GUI overrides stay separate. The NixOS module installs the selected package; none of these modules installs an always-running Luciole service or changes the default file manager.

## Verification

Enable the headless suite explicitly for a release check build:

```sh
meson setup build-check --buildtype=release --prefix=/usr -Dtests=enabled
meson compile -C build-check
meson test -C build-check --print-errorlogs
```

Tests additionally require Python 3, dbus-daemon, `wayland-server` development files and libwebp development files. Provide the media/PDF/archive helpers and Glycin loaders exercised by the fixtures, including AVIF/HEIF support. A compositor is not needed for the ordinary headless suites. Their local-VFS setting does not verify GVfs network providers; live provider checks use separate private fixtures.

Before publishing, check `--help`, `--version`, configuration validation, installed fonts and completions. Test an installed window with the development asset override unset, plus the image/media/archive and GVfs features advertised by the package. Keep packaging smoke tests isolated from users' configuration, desktop input and real files. x86_64 Nix release builds and isolated Wayland/runtime checks are verified; ARM and a full distro installation matrix remain follow-ups.

## Contact

- Issues: <https://github.com/noctalia-dev/luciole/issues>
- Discord: <https://discord.noctalia.dev>
