# Packaging Luciole

Notes for distribution packagers. End-user setup lives in the [installation guide](docs/user/installation.md); features and settings belong in the [user guide](docs/user/index.md).

The dependency and install declarations in [meson.build](meson.build), [lib/meson.build](lib/meson.build) and [nix/package.nix](nix/package.nix) are authoritative.

## Package description

The package metadata in [nix/package.nix](nix/package.nix) uses:

> Luciole, a native Wayland file manager using Noctalia rendering and controls (in development)

## Identity

| Field | Value |
| --- | --- |
| Package / binary | `luciole` |
| Display name | Luciole |
| Application ID | `dev.noctalia.luciole` |
| File chooser application ID | `dev.noctalia.luciole-chooser` |
| Homepage | <https://github.com/noctalia-dev/luciole> |
| Version | [VERSION](VERSION), read by Meson |
| License | MIT ([LICENSE](LICENSE)); also ship the Tabler font notice |
| Desktop entry | `dev.noctalia.luciole.desktop` |
| File chooser desktop entry | `dev.noctalia.luciole-chooser.desktop` (hidden, reuses the application icon) |
| Application icon | `dev.noctalia.luciole.svg` |

## Platforms and toolchain

Linux with a Wayland compositor and EGL/OpenGL ES support is required. Neither the Noctalia shell nor Umbriel is a runtime dependency. Luciole uses a static internal copy of Noctalia's UI library, without a Qt or GTK widget dependency.

The flake declares `x86_64-linux` and `aarch64-linux`. Builds and runtime checks are verified on x86_64; ARM outputs have only been evaluated. Other architectures and musl are not verified.

Build tools: Meson 1.1+, Ninja, pkg-config, wayland-scanner, a C compiler and a C++23 compiler/standard library. Required library facilities include `std::print`, `std::expected` and `std::move_only_function`. GCC/libstdc++ 16.2 is verified; an older compiler minimum and Clang/libc++ compatibility have not been established. Meson supplies the experimental-library flags when it detects libc++.

## Build

Configure the final prefix and use `DESTDIR` for staging:

```sh
meson setup build-package --buildtype=release --prefix=/usr \
  -Dtests=disabled -Dnative_optimizations=false
meson compile -C build-package
DESTDIR="$PWD/pkgroot" meson install --no-rebuild -C build-package
```

`just` is a development convenience, not a build dependency. Source archives must include `lib/`, its assets/protocols and the application's `assets/`. Builds do not fetch or sync a sibling Noctalia checkout.

### Meson options

| Option | Default | Packaging guidance |
| --- | --- | --- |
| `tests` | `auto` | Auto enables tests for unsanitized debug builds; use `enabled` for check builds. |
| `native_optimizations` | `false` | Keep false for portable packages; true enables CPU-local code generation. |
| `build_revision` | `unknown` | Revision shown by `--version` and the About page. `unknown` derives it from `git describe` when building from a Git checkout; set it explicitly for archive builds. |
| `benchmarks` | `false` | Leave disabled for ordinary packages. |

Standard Meson prefix, directory, LTO and sanitizer options apply. GNU LTO needs compiler-aware archive tools such as `gcc-ar` and `gcc-ranlib`. Use a fresh build directory when changing toolchains.

## Dependencies

### Build and link dependencies

These are pkg-config modules or header paths. Install the corresponding development packages for the target distribution.

| Dependency | Requirement |
| --- | --- |
| `wayland-protocols` | 1.49+; build-time protocol definitions |
| `wayland-client`, `wayland-egl`, `wayland-cursor` | Wayland libraries |
| `egl`, `glesv2` | EGL/OpenGL ES; shared-library detection also supports wayland-egl/libepoxy fallbacks |
| `gio-2.0`, `gio-unix-2.0` | GLib/GIO 2.76+ |
| `glib-2.0` | GLib |
| `libcurl` | 7.85+; HTTP transport |
| `libxml-2.0` | 2.9+; XML parsing |
| `freetype2`, `fontconfig` | Font libraries |
| `cairo`, `cairo-ft` | Cairo with FreeType integration |
| `pango`, `pangocairo`, `pangoft2` | Pango 1.50+ |
| `harfbuzz`, `xkbcommon` | Text shaping and keyboard input |
| `gdk-pixbuf-2.0` | 2.42+; a Glycin-enabled 2.44+ build is recommended |
| `librsvg-2.0`, `libpng` | SVG and PNG libraries |
| `libarchive`, `liblzma` | Archive libraries |
| `libacl` | POSIX ACL library |
| `tomlplusplus` | 3.4+ |
| `nlohmann_json` | JSON library |
| `stb/stb_image_resize2.h` | System stb resize2 header; the older resize header is insufficient |
| Threads | System thread support |

GdkPixbuf decodes raster images; Luciole does not link Glycin itself. A Glycin-enabled GdkPixbuf (2.44+) decodes in sandboxed loader processes, and the installed Glycin loaders determine format coverage; AVIF/HEIF may need additional loader dependencies such as libheif. Without Glycin, GdkPixbuf uses its built-in loaders in-process; WebP, AVIF, HEIF and JPEG XL then need the matching GdkPixbuf loader packages, and PNG-compressed ICO icons are not decoded. Thumbnail decoding stays in Luciole's resource-limited helper processes either way. The Nix recipe selects a Glycin-enabled GdkPixbuf variant and builds librsvg against it; native distributions should use their own compatible package set.

The internal UI library's optional codec pack is disabled. Wuffs, JPEG XL and its WebP codec-pack dependencies are not required for Luciole's normal build. Archive support uses system libarchive and its enabled filters; encrypted 7z uses the system CLI below, without py7zr, SDK headers or a `7z.so` C++ ABI dependency.

### Runtime integration and helpers

In addition to linked-library runtime packages, ship or recommend the following as appropriate:

| Component | Packaging requirement |
| --- | --- |
| Text fonts and Fontconfig | Usable text fonts are required in addition to the bundled glyph font. |
| Shared MIME database | Provide `shared-mime-info` data. |
| Icon theme | Provide a usable theme; the Nix package supplies Adwaita. |
| Desktop settings | Expose `org.gnome.desktop.interface` schemas and a GSettings backend, normally gsettings-desktop-schemas and dconf. |
| Image loaders | Glycin loaders for a Glycin-enabled GdkPixbuf, otherwise GdkPixbuf loader modules for the formats to support. |
| Session D-Bus and GVfs | Required for the shipped Trash interface; see below. |
| GVfs protocol backends | Install the backends for the network protocols the package supports. |
| GVfs UDisks volume monitor and UDisks2 | Device discovery, mounting and supported encrypted-volume unlocking; expose the volume monitor's session activation and the UDisks2 system service. |
| GVfs DNS-SD backend and Avahi | Optional DNS-SD discovery; requires the backend, its schemas/mount data and a working Avahi service. |
| `glib-networking` | Supply GIO TLS support for secure WebDAV. |
| `ffmpeg`, `ffprobe` | Media thumbnails and metadata. |
| `ffmpegthumbnailer` | Recommended for shared video-thumbnail filmstrips; executable/runtime closure only, no development headers. |
| `pdftoppm`, `pdfinfo` | PDF support, usually from Poppler utilities. |
| `7zz`, `7z` or `7za` | Encrypted 7z input; searched in that order. `LUCIOLE_7Z` can select an absolute executable path. |
| `xdg-terminal-exec` | Recommended for automatic terminal selection; configured commands and supported terminals can also be used directly. |

Missing optional helpers do not prevent ordinary local browsing. File Roller, Ark, Qt/KIO, unrar and a Samba server are not Luciole dependencies. User scripts require their own interpreters; Luciole does not require a scripting language at runtime.

### GVfs and Trash

Declare GVfs as a runtime dependency, not just a network-access recommendation. Include its GIO module, `gvfsd`, `gvfsd-trash`, mount data and session D-Bus activation files. Distributions split these among packages differently; installing client libraries alone is insufficient. Add the SFTP, SMB and WebDAV backends needed by the package.

A usable session bus and daemon activation are required; a GNOME desktop session is not. Luciole uses GIO directly and does not require `gvfs-fuse`. Do not force `GIO_USE_VFS=local` in a packaged launcher: that test-only setting disables provider access.

Device mounting and LUKS unlocking additionally use `gvfs-udisks2-volume-monitor` and the UDisks2 system service over the system bus. Provide the monitor's GIO module/metadata and session D-Bus activation files. UDisks owns the encryption backend and filesystem helpers; include their normal device-mapper udev rules so unlocked mappings can be identified. Luciole does not invoke cryptsetup or require its development library. System authorization remains subject to UDisks/Polkit policy and an available desktop authentication agent when needed. An optional system secret service can retain provider credentials for the session. The NixOS module keeps service activation manual.

SFTP needs `gvfsd-sftp`, its `sftp.mount` descriptor (which also advertises the `ssh` alias), and the SSH client used by that provider. An SSH client or the GIO client module alone does not supply SFTP browsing. Keep the backend executable, mount data and daemon activation available to the same desktop session as Luciole.

DNS-SD discovery is optional and separate from SMB access. For example, Arch supplies [gvfs-dnssd](https://archlinux.org/packages/extra/x86_64/gvfs-dnssd/). Include the provider's GSettings schemas, mount data, executable and D-Bus activation files. Luciole's native WS-Discovery client uses the linked libraries above; neither `gvfs-wsdd` nor a `wsdd`, `wsdd2` or `wsdd-native` package is required. Do not require or enable a discovery daemon for Luciole. Direct SMB access still needs the GVfs SMB backend. Local firewall policy must permit outgoing UDP multicast and the corresponding replies for WS-Discovery; no privileged port or system service is required.

Native packages should use normal system GIO module paths. Nix wrappers expose the separately installed modules through `GIO_EXTRA_MODULES`; users of ordinary distribution packages should not need that export. TLS uses the provider and system trust configuration.

## Installed layout

With relative `datadir=share`, Meson installs:

```text
<prefix>/bin/luciole
<prefix>/share/luciole/assets/fonts/noctalia-tabler.ttf
<prefix>/share/luciole/assets/fonts/tabler.json
<prefix>/share/luciole/assets/fonts/tabler-icons-license.txt
<prefix>/share/luciole/templates/colors.toml
<prefix>/share/dbus-1/services/dev.noctalia.luciole.FileManager1.service
<prefix>/share/dbus-1/services/org.freedesktop.impl.portal.desktop.luciole.service
<prefix>/share/xdg-desktop-portal/portals/luciole.portal
<prefix>/share/applications/dev.noctalia.luciole.desktop
<prefix>/share/applications/dev.noctalia.luciole-chooser.desktop
<prefix>/share/icons/hicolor/scalable/apps/dev.noctalia.luciole.svg
<prefix>/share/bash-completion/completions/luciole
<prefix>/share/zsh/site-functions/_luciole
<prefix>/share/fish/vendor_completions.d/luciole.fish
<prefix>/share/licenses/luciole/LICENSE
```

Ship the fonts directory and its license together. The palette template is an integration resource, not per-user configuration. No AppStream/metainfo file, manual page or systemd unit is currently installed.

### Asset lookup and staging

The configured `<prefix>/<datadir>/luciole/assets` path is compiled into the binary. `LUCIOLE_ASSETS_DIR` overrides it; an invalid override is an error. `DESTDIR` does not change compiled paths.

For a source-tree smoke test:

```sh
LUCIOLE_ASSETS_DIR="$PWD/lib/assets" ./build-package/luciole
```

A staged-install smoke test must point to its staged assets. Remove development overrides from installed launchers and verify that the final installation resolves its assets unaided.

### D-Bus activation

`dev.noctalia.luciole.FileManager1.service` registers `org.freedesktop.FileManager1` and starts the configured absolute binary path with `--file-manager-service`. Keep the prefix correct and make the session's Wayland environment available to activation. The file is named after Luciole, as Dolphin's `org.kde.dolphin.FileManager1.service` is, so it never conflicts with the generic `org.freedesktop.FileManager1.service` that Nautilus ships. When several file managers provide the name, the bus activates one of them. To prefer Luciole, a user copies this file to `~/.local/share/dbus-1/services/org.freedesktop.FileManager1.service`, which takes precedence over system directories.

Installing Luciole does not select it as the `inode/directory` MIME default. The application also owns `dev.noctalia.luciole` for single-instance forwarding, without a separate service file for that name.

### FileChooser portal backend

Ship `luciole.portal` and its generated session service together. They register `org.freedesktop.impl.portal.FileChooser` on `org.freedesktop.impl.portal.desktop.luciole`; activation runs `<bindir>/luciole --file-chooser-service`. Chooser windows use the Wayland app ID `dev.noctalia.luciole-chooser`; install its NoDisplay desktop entry so shells resolve the chooser's name and icon from the same Luciole artwork.

The portal file has no `UseIn` key. Session integration must select it in `portals.conf`, for example:

```ini
[preferred]
org.freedesktop.impl.portal.FileChooser=luciole;gtk
```

Keep the session's other portal preferences when adding this entry; configuration files are selected as a whole rather than merged. End-user setup for distribution packages and NixOS is in the [installation guide](docs/user/installation.md#use-luciole-as-the-file-picker).

Selection requires xdg-desktop-portal. The frontend discovers portal files at startup, so restart it after installing/removing the backend. A listed absent backend permits fallback; an installed backend that fails does not.

### Shell completions

Meson installs static Bash, Zsh and Fish scripts. Packaging does not need to run the target binary to generate them, including when cross-compiling.

## Configuration and writable data

| Path | Purpose |
| --- | --- |
| `$XDG_CONFIG_HOME/luciole/config.toml` | Optional user/declarative base configuration |
| `$XDG_CONFIG_HOME/luciole/settings.toml` | Writable GUI overrides |
| `$XDG_CONFIG_HOME/luciole/colors.toml` | Optional palette handoff |
| `$XDG_CONFIG_HOME/gtk-3.0/bookmarks` | Shared user bookmarks |
| `$XDG_STATE_HOME/luciole/state.toml` | Private application state |
| `$XDG_CACHE_HOME/thumbnails/` | Shared regenerable thumbnail cache |
| `$XDG_DATA_HOME/luciole/scripts/` | User-installed scripts |

XDG defaults apply when roots are unset. Luciole does not search `/etc/xdg` or a packaged default configuration. Do not populate users' settings, state, bookmarks, scripts or caches from a system package. Keep `settings.toml` writable when a declarative tool owns `config.toml`. See [configuration](docs/user/configuration.md) for format and precedence.

Private `.luciole-create-*` and `.luciole-history-*` directories can contain retained user data. They are not regenerable caches and must not be removed by package cleanup or tmpfiles rules targeting Luciole's scratch-name prefixes.

## Nix integration

[nix/package.nix](nix/package.nix) defines the package. [flake.nix](flake.nix) exports packages, checks, the development shell, an overlay and NixOS/Home Manager/hjem modules; [default.nix](default.nix) provides non-flake entry points.

End-user flake setup and module examples are in the [NixOS installation guide](docs/user/installation.md#nixos).

```sh
nix build path:.#default
nix flake check path:.
```

The package wrapper supplies helper paths, GIO/TLS/dconf modules, desktop settings schemas, Glycin loader data, MIME data and Adwaita icons. The development shell and checks provide the corresponding runtime environment. Native distributions should use their normal desktop integration rather than copying Nix store paths.

The Nix recipe supplies libcurl and libxml2 as link dependencies. Optional DNS-SD requires a system Avahi service; the application package does not enable system services.

Luciole's NixOS module installs the application; Home Manager/hjem manage its declarative base configuration. Enable `services.gvfs.enable = true;` explicitly in the NixOS system configuration for the GVfs daemons and session activation. Luciole's modules do not enable GVfs automatically, select a default file manager or install an always-running Luciole service.

## Verification

For a release check build:

```sh
meson setup build-check --buildtype=release --prefix=/usr -Dtests=enabled
meson compile -C build-check
meson test -C build-check --print-errorlogs
```

Tests additionally need Python 3, `dbus-daemon`, `glib-compile-schemas`, `wayland-server` development files and libwebp development files. Provide FFmpeg, ffmpegthumbnailer, Poppler utilities, 7-Zip and image loaders, including AVIF/HEIF support. The `thumbnail` suite's format checks expect a Glycin-enabled GdkPixbuf; without Glycin its PNG-compressed ICO check fails. Ordinary headless suites need no compositor; their local-VFS override does not verify GVfs activation.

Smoke-test the installed binary's help/version, configuration validation, fonts, completions and an installed window with asset overrides unset. Verify image/helper availability and session access to `trash:///`. Use disposable files for operation checks. Destructive whole-Trash checks require a private mount namespace hiding user drives, not just temporary HOME/XDG directories.

## Contact

- Issues: <https://github.com/noctalia-dev/luciole/issues>
- Discord: <https://discord.noctalia.dev>
