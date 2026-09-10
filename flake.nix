{
  description = "Howdy Next facial-recognition authentication for Linux";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = function: nixpkgs.lib.genAttrs systems function;

      pkgsFor = system: import nixpkgs { inherit system; };

      opencv5For = system:
        let
          pkgs = pkgsFor system;
        in
        pkgs.callPackage ./nix/opencv5.nix { };

      # nixpkgs 26.05 ships yyjson.pc with duplicated absolute prefixes.
      # CMake's imported pkg-config target would otherwise use invalid paths.
      yyjsonFor = system:
        let
          pkgs = pkgsFor system;
        in
        pkgs.yyjson.overrideAttrs (old: {
          postInstall = (old.postInstall or "") + ''
            sed -i "s|$out/$out|$out|g" "$out/lib/pkgconfig/yyjson.pc"
          '';
        });

      howdyNextFor = system:
        let
          pkgs = pkgsFor system;
        in
        pkgs.stdenv.mkDerivation {
          pname = "howdy-next";
          version = "3.4.0";
          src = ./.;

          strictDeps = true;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.gettext
          ];

          buildInputs = [
            pkgs.acl
            pkgs.curl
            pkgs.inih
            pkgs.libevdev
            pkgs.openssl
            (opencv5For system)
            pkgs.pam
            (yyjsonFor system)
          ];

          cmakeFlags = [
            "-GNinja"
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_INSTALL_PREFIX=${placeholder "out"}"
            "-DCMAKE_INSTALL_LIBDIR=lib"
            "-DCMAKE_INSTALL_BINDIR=bin"
            "-DCMAKE_INSTALL_LIBEXECDIR=libexec"
            "-DCMAKE_INSTALL_DATADIR=share"
            "-DCMAKE_INSTALL_MANDIR=share/man"
            "-DCMAKE_INSTALL_LOCALEDIR=share/locale"
            "-DBUILD_TESTING=OFF"
            "-DHOWDY_USE_CCACHE=OFF"
            "-DHOWDY_PAM_DIR=${placeholder "out"}/lib/security"
            "-DHOWDY_CONFIG_DIR=/etc/howdy"
            "-DHOWDY_MODELS_DIR=/var/lib/howdy/models"
            "-DHOWDY_USER_MODELS_DIR=/var/lib/howdy/user-models"
            "-DHOWDY_LOG_PATH=/var/log/howdy"
            "-DHOWDY_AUTH_HELPER_PATH=/run/wrappers/bin/howdy-auth-helper"
            "-DHOWDY_INSTALL_AUTH_HELPER_SETUID=OFF"
            "-DHOWDY_LICENSES_INSTALL_DIR=share/licenses/howdy-next"
          ];

          # CMake's install scripts must be staged: /etc and /var are host
          # paths at runtime, never paths the derivation may write directly.
          installPhase = ''
            runHook preInstall
            howdyInstallRoot=$(mktemp -d)
            DESTDIR="$howdyInstallRoot" cmake --install .
            mkdir -p "$out"
            cp -a "$howdyInstallRoot/$out/." "$out/"
            install -Dm0640 "$howdyInstallRoot/etc/howdy/config.ini" \
              "$out/share/howdy/config.ini"
            rm -rf "$out/etc" "$out/var"
            runHook postInstall
          '';

          meta = {
            description = "C++ facial-recognition authentication for Linux";
            homepage = "https://codeberg.org/nathawat/howdy-next";
            license = pkgs.lib.licenses.gpl3Plus;
            mainProgram = "howdy";
            platforms = pkgs.lib.platforms.linux;
          };
        };
    in
    {
      packages = forAllSystems (system: {
        default = howdyNextFor system;
        howdy-next = howdyNextFor system;
      });

      devShells = forAllSystems (system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.mkShell {
            strictDeps = true;

            packages = [
              pkgs.stdenv.cc
              pkgs.cmake
              pkgs.ccache
              pkgs.ninja
              pkgs.pkg-config
              pkgs.gettext
              pkgs.git
            ];

            buildInputs = [
              pkgs.acl
              pkgs.curl
              pkgs.inih
              pkgs.libevdev
              pkgs.openssl
              (opencv5For system)
              pkgs.pam
              (yyjsonFor system)
            ];

            shellHook = ''
              export CCACHE_DIR="''${XDG_CACHE_HOME:-$HOME/.cache}/ccache"
              export CMAKE_CXX_COMPILER_LAUNCHER=ccache
            '';
          };
        });

      nixosModules.default = { config, lib, pkgs, ... }:
        let
          cfg = config.services.howdy-next;
        in
        {
          options.services.howdy-next = {
            enable = lib.mkEnableOption "Howdy Next facial authentication";

            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.stdenv.hostPlatform.system}.howdy-next;
              defaultText = lib.literalExpression "inputs.howdy-next.packages.\${system}.howdy-next";
              description = "Howdy Next package to install.";
            };
          };

          config = lib.mkIf cfg.enable {
            environment.systemPackages = [ cfg.package ];

            # Extend the packaged Polkit helper unit without changing PAM policy.
            # PrivateDevices=yes upstream hides cameras and the prompt input device.
            systemd.services."polkit-agent-helper@" = lib.mkIf config.security.polkit.enable {
              serviceConfig = {
                PrivateDevices = false;
                DeviceAllow = [
                  "char-video4linux rw"
                  "/dev/uinput rw"
                ];
              };
            };

            # NixOS supplies setuid through /run/wrappers/bin. The helper in
            # /nix/store remains an ordinary executable.
            security.wrappers.howdy-auth-helper = {
              source = "${cfg.package}/libexec/howdy/howdy-auth-helper";
              owner = "root";
              group = "root";
              setuid = true;
            };

            # Runtime config and model/log state must stay mutable and outside
            # the immutable package output. C copies template only when absent.
            systemd.tmpfiles.rules = [
              "d /etc/howdy 0750 root root -"
              "C /etc/howdy/config.ini 0640 root root - ${cfg.package}/share/howdy/config.ini"
              "d /var/lib/howdy 0750 root root -"
              "d /var/lib/howdy/models 0750 root root -"
              "d /var/lib/howdy/user-models 0750 root root -"
              "d /var/log/howdy 0750 root root -"
            ];
          };
        };

      checks = forAllSystems (system:
        let
          pkgs = pkgsFor system;
          package = howdyNextFor system;
          moduleConfig = nixpkgs.lib.nixosSystem {
            inherit system;
            modules = [
              self.nixosModules.default
              ({ ... }: {
                services.howdy-next.enable = true;
                security.polkit.enable = true;
              })
            ];
          };
          moduleConfigWithoutPolkit = nixpkgs.lib.nixosSystem {
            inherit system;
            modules = [
              self.nixosModules.default
              ({ ... }: {
                services.howdy-next.enable = true;
                security.polkit.enable = false;
              })
            ];
          };
          howdyModule = moduleConfig.config.services.howdy-next;
          wrapper = moduleConfig.config.security.wrappers.howdy-auth-helper;
          polkitHelper = moduleConfig.config.systemd.services."polkit-agent-helper@";
        in
        {
          package = package;
          module =
            assert howdyModule.enable;
            assert howdyModule.package == package;
            assert polkitHelper.name == "polkit-agent-helper@.service";
            assert polkitHelper.overrideStrategy == "asDropinIfExists";
            assert polkitHelper.serviceConfig.PrivateDevices == false;
            assert polkitHelper.serviceConfig.DeviceAllow == [
              "char-video4linux rw"
              "/dev/uinput rw"
            ];
            assert polkitHelper.serviceConfig.StandardError == "journal";
            assert moduleConfigWithoutPolkit.config.services.howdy-next.enable;
            assert !(moduleConfigWithoutPolkit.config.systemd.services ? "polkit-agent-helper@");
            assert !(moduleConfigWithoutPolkit.config.systemd.units ? "polkit-agent-helper@.service");
            assert moduleConfig.config.security.wrapperDir == "/run/wrappers/bin";
            assert wrapper.setuid;
            assert wrapper.owner == "root";
            assert wrapper.group == "root";
            assert wrapper.source == "${package}/libexec/howdy/howdy-auth-helper";
            assert nixpkgs.lib.elem
              "C /etc/howdy/config.ini 0640 root root - ${package}/share/howdy/config.ini"
              moduleConfig.config.systemd.tmpfiles.rules;
            pkgs.runCommand "howdy-next-module-evaluation" { } "touch $out";
        });
    };
}
