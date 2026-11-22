{
  lib,
  stdenv,
  fetchurl,
  fetchFromGitHub,
  meson,
  ninja,
  pkg-config,
  python3,
  wayland-scanner,
  wayland,
  wayland-protocols,
  libglvnd,
  libxkbcommon,
  freetype,
  fontconfig,
  cairo,
  glib,
  glib-networking,
  gsettings-desktop-schemas,
  dconf,
  dbus,
  gvfs,
  curl,
  libxml2,
  pango,
  harfbuzz,
  libpng,
  libarchive,
  xz,
  acl,
  libwebp,
  librsvg,
  gdk-pixbuf,
  libglycin,
  glycin-loaders,
  stb,
  ffmpeg,
  ffmpegthumbnailer,
  poppler-utils,
  _7zz,
  nlohmann_json,
  tomlplusplus,
  autoAddDriverRunpath,
  makeWrapper,
  adwaita-icon-theme,
  shared-mime-info,
  xdg-terminal-exec,
  buildRevision ? "unknown",
}:
let
  stb' = stb.overrideAttrs (_: {
    version = "unstable-2025-10-26";
    src = fetchFromGitHub {
      owner = "nothings";
      repo = "stb";
      rev = "f1c79c02822848a9bed4315b12c8c8f3761e1296";
      hash = "sha256-BlyXJtAI7WqXCTT3ylww8zoG0hBxaojJnQDvdQOXJPE=";
    };
  });
  pixbufWithGlycin = gdk-pixbuf.overrideAttrs (previous: {
    # 2.44.8 requires Glycin 2.2; the pinned collection provides its 2.1 API.
    version = "2.44.6";
    src = fetchurl {
      url = "https://download.gnome.org/sources/gdk-pixbuf/2.44/gdk-pixbuf-2.44.6.tar.xz";
      hash = "sha256-FAwtC4mfz4U+6SsmNzydwijbzeCCCkJGaT9DKKJ0Zvo=";
    };
    buildInputs = (previous.buildInputs or [ ]) ++ [
      libglycin
      glycin-loaders
      shared-mime-info
    ];
    preBuild = (previous.preBuild or "") + ''
      export XDG_DATA_DIRS="${glycin-loaders}/share:${shared-mime-info}/share:''${XDG_DATA_DIRS:-}"
    '';
    mesonFlags =
      builtins.filter (
        flag:
        !(lib.any (name: lib.hasPrefix "-D${name}=" flag) [
          "glycin"
          "builtin_loaders"
          "png"
          "jpeg"
          "gif"
          "tiff"
          "others"
          "legacy_xpm"
        ])
      ) previous.mesonFlags
      ++ [
        "-Dglycin=enabled"
        "-Dbuiltin_loaders=glycin"
        "-Dpng=disabled"
        "-Djpeg=disabled"
        "-Dgif=disabled"
        "-Dtiff=disabled"
        "-Dothers=disabled"
        "-Dlegacy_xpm=disabled"
      ];
  });
  svgWithGlycin = librsvg.override { gdk-pixbuf = pixbufWithGlycin; };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "luciole";
  version = lib.fileContents ../VERSION;

  src = lib.cleanSourceWith {
    src = ./..;
    filter =
      path: type:
      let
        name = baseNameOf path;
      in
      lib.cleanSourceFilter path type
      && !(builtins.elem name [
        ".agents"
        ".aws"
        ".codex"
        ".cache"
        ".direnv"
        ".envrc"
        ".idea"
        ".vscode"
        "__pycache__"
        "result"
        "AGENTS.md"
      ])
      && !(lib.hasPrefix "build-" name)
      && !(lib.hasPrefix "result-" name);
  };

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    python3
    wayland-scanner
    autoAddDriverRunpath
    makeWrapper
  ];

  nativeCheckInputs = [
    dbus
    glib
  ];

  buildInputs = [
    wayland
    wayland-protocols
    libglvnd
    libxkbcommon
    freetype
    fontconfig
    cairo
    glib
    curl
    libxml2
    pango
    harfbuzz
    libpng
    libarchive
    xz
    acl
    libwebp
    pixbufWithGlycin
    svgWithGlycin
    glycin-loaders
    stb'
    nlohmann_json
    tomlplusplus
  ];

  # GNU LTO archives need the compiler's plugin-aware tools.
  preConfigure = lib.optionalString stdenv.cc.isGNU ''
    export AR="${stdenv.cc.cc}/bin/gcc-ar"
    export RANLIB="${stdenv.cc.cc}/bin/gcc-ranlib"
  '';

  mesonBuildType = "release";
  postFixup = ''
    wrapProgram "$out/bin/luciole" \
      --suffix PATH : "${
        lib.makeBinPath [
          xdg-terminal-exec
          ffmpeg
          ffmpegthumbnailer
          poppler-utils
          _7zz
        ]
      }" \
      --suffix GIO_EXTRA_MODULES : "${
        lib.makeSearchPath "lib/gio/modules" [
          gvfs
          glib-networking
          dconf.lib
        ]
      }" \
      --suffix XDG_DATA_DIRS : "${glib.getSchemaDataDirPath gsettings-desktop-schemas}:${
        lib.makeSearchPath "share" [
          adwaita-icon-theme
          shared-mime-info
          glycin-loaders
        ]
      }"
  '';
  mesonFlags = [
    "-Dtests=disabled"
    (lib.mesonOption "build_revision" buildRevision)
  ];
  passthru.glycinLoaders = glycin-loaders;

  meta = {
    description = "Luciole, a native Wayland file manager using Noctalia rendering and controls (in development)";
    homepage = "https://github.com/noctalia-dev/luciole";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = finalAttrs.pname;
  };
})
