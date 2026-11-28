{
  description = "Luciole, a native Wayland file manager using Noctalia rendering and controls.";

  inputs.nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.zst";

  outputs =
    { self, nixpkgs }:
    let
      buildRevision = self.shortRev or self.dirtyShortRev or "unknown";
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forEachSystem = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      withPackage =
        module:
        { pkgs, lib, ... }:
        {
          imports = [ module ];
          programs.luciole.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
    in
    {
      overlays.default = final: _prev: {
        luciole = final.callPackage ./nix/package.nix { inherit buildRevision; };
      };

      packages = forEachSystem (pkgs: {
        default = pkgs.callPackage ./nix/package.nix { inherit buildRevision; };
      });

      apps = forEachSystem (pkgs: {
        default = {
          type = "app";
          program = nixpkgs.lib.getExe self.packages.${pkgs.stdenv.hostPlatform.system}.default;
          meta.description = self.packages.${pkgs.stdenv.hostPlatform.system}.default.meta.description;
        };
      });

      devShells = forEachSystem (pkgs: {
        default = pkgs.callPackage ./nix/devshell.nix {
          luciole = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
      });

      checks = forEachSystem (pkgs: {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        module-config = import ./nix/module-checks.nix {
          inherit pkgs;
          package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
        headless-cli = self.packages.${pkgs.stdenv.hostPlatform.system}.default.overrideAttrs (_: {
          mesonFlags = [ "-Dtests=enabled" ];
          doCheck = true;
          preCheck = ''
            export GIO_EXTRA_MODULES="${pkgs.gvfs}/lib/gio/modules:${pkgs.glib-networking}/lib/gio/modules"
            export XDG_DATA_DIRS="${pkgs.shared-mime-info}/share:${pkgs.glycin-loaders}/share"
            export PATH="${
              nixpkgs.lib.makeBinPath [
                pkgs.ffmpeg
                pkgs.ffmpegthumbnailer
                pkgs.bubblewrap
                pkgs.poppler-utils
                pkgs._7zz
              ]
            }:$PATH"
          '';
        });
      });

      formatter = forEachSystem (pkgs: pkgs.nixfmt);
      homeModules.default = withPackage ./nix/home-module.nix;
      hjemModules.default = withPackage ./nix/hjem-module.nix;
      nixosModules.default = withPackage ./nix/nixos-module.nix;
    };
}
