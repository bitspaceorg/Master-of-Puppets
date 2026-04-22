{
    description = "Master of Puppets — Examples";

    inputs = {
        nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
        parts.url = "github:hercules-ci/flake-parts";
    };

    outputs =
        inputs:
        inputs.parts.lib.mkFlake { inherit inputs; } {
            systems = inputs.nixpkgs.lib.systems.flakeExposed;

            perSystem =
                { pkgs, self', ... }:
                let
                    mop = pkgs.stdenv.mkDerivation {
                        pname = "libmop";
                        version = "0.1.0";
                        src = ./..;
                        nativeBuildInputs = with pkgs; [
                            gnumake
                            pkg-config
                        ];
                        buildPhase = "make RELEASE=1 CC=${pkgs.stdenv.cc}/bin/cc";
                        installPhase = ''
                            make install PREFIX=$out
                            mkdir -p $out/lib
                            cp build/lib/libmop.a $out/lib/
                        '';
                    };

                    mkExample =
                        name: src: extraBuildInputs: extraCflags: extraLdflags:
                        pkgs.stdenv.mkDerivation {
                            pname = "mop-example-${name}";
                            version = "0.1.0";
                            inherit src;
                            nativeBuildInputs = [
                                pkgs.gnumake
                                pkgs.pkg-config
                            ];
                            buildInputs = [ mop ] ++ extraBuildInputs;
                            dontUnpack = true;
                            buildPhase = ''
                                ${pkgs.stdenv.cc}/bin/cc -std=c11 -O2 -Wall -Wextra \
                                    -Wno-unused-parameter -fno-strict-aliasing \
                                    -I${mop}/include/mop/.. \
                                    ${extraCflags} \
                                    $src \
                                    -L${mop}/lib -lmop -lm -lpthread \
                                    ${extraLdflags} \
                                    ${if pkgs.stdenv.isDarwin then "-lc++" else "-lstdc++"} \
                                    -o ${name}
                            '';
                            installPhase = ''
                                mkdir -p $out/bin
                                cp ${name} $out/bin/
                            '';
                        };
                in
                {
                    packages = {
                        showcase = mkExample "showcase" ./showcase.c [ ] "" "";
                        interactive = mkExample "interactive" ./interactive.c [ pkgs.SDL2 ] "$(pkg-config --cflags sdl2)" "$(pkg-config --libs sdl2)";
                        default = self'.packages.showcase;
                    };

                    devShells.default = pkgs.mkShell {
                        nativeBuildInputs = with pkgs; [
                            gnumake
                            pkg-config
                        ];
                        buildInputs = [
                            mop
                            pkgs.SDL2
                            pkgs.vulkan-headers
                            pkgs.vulkan-loader
                        ]
                        ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.moltenvk ];
                        shellHook = ''
                            echo "MOP Examples — development shell"
                            echo "  Build:      make"
                            echo "  Run:        nix run .#showcase"
                            echo "  Interact:   nix run .#interactive"
                        '';
                    };
                };
        };
}
