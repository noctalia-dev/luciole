{ pkgs, luciole }:
pkgs.mkShell {
  inputsFrom = [ luciole ];
  hardeningDisable = [ "fortify" ];

  nativeBuildInputs = with pkgs; [
    just
    meson
    ninja
    pkg-config
    wayland-scanner
    llvmPackages_23.clang-tools
    llvmPackages_23.libclang
    python3
    dbus
    nixfmt
    statix
    deadnix
    gdb
    xdg-terminal-exec
    ffmpeg
    ffmpegthumbnailer
    bubblewrap
    poppler-utils
    _7zz
  ];

  shellHook = ''
    export LUCIOLE_ASSETS_DIR="$PWD/lib/assets"
    export GIO_EXTRA_MODULES="''${GIO_EXTRA_MODULES:+$GIO_EXTRA_MODULES:}${pkgs.gvfs}/lib/gio/modules:${pkgs.glib-networking}/lib/gio/modules:${pkgs.dconf.lib}/lib/gio/modules"
    export XDG_DATA_DIRS="''${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:${pkgs.adwaita-icon-theme}/share:${pkgs.shared-mime-info}/share:${luciole.glycinLoaders}/share:${pkgs.glib.getSchemaDataDirPath pkgs.gsettings-desktop-schemas}"
    ${pkgs.lib.optionalString pkgs.stdenv.cc.isGNU ''
      export AR="${pkgs.stdenv.cc.cc}/bin/gcc-ar"
      export RANLIB="${pkgs.stdenv.cc.cc}/bin/gcc-ranlib"
    ''}
    echo "Luciole dev shell | just --list"
  '';
}
