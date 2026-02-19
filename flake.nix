{
  description = "Master of Puppets — Backend-agnostic viewport rendering engine";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "master-of-puppets";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [ gnumake ];

          buildPhase = ''
            make RELEASE=1 CC=${pkgs.stdenv.cc}/bin/cc
          '';

          installPhase = ''
            make install PREFIX=$out
          '';

          meta = {
            description = "Backend-agnostic viewport rendering engine in C11";
            license = pkgs.lib.licenses.asl20;
            platforms = pkgs.lib.platforms.unix;
          };
        };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            gnumake
            clang-tools
          ] ++ pkgs.lib.optionals (!pkgs.stdenv.isDarwin) [
            valgrind
            gdb
          ];

          shellHook = ''
            echo "Master of Puppets — development shell"
            echo "  Build:    make"
            echo "  Clean:    make clean"
            echo "  Examples: cd examples && nix develop"
          '';
        };
      });
}
