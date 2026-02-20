{
  description = "Master of Puppets — Interactive examples (SDL3)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            gnumake
            pkg-config
          ];

          buildInputs = [
            pkgs.sdl3
            pkgs.lua5_4
            pkgs.vulkan-loader
          ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [
            pkgs.moltenvk
          ];

          shellHook = ''
            echo "Master of Puppets — examples shell (SDL3)"
            echo "  Build all:    make"
            echo "  Headless:     make headless && make run-headless"
            echo "  Interactive:  make interactive && make run"
            if pkg-config --exists sdl3 2>/dev/null; then
              echo "  SDL3:         $(pkg-config --modversion sdl3)"
            else
              echo "  SDL3:         NOT FOUND — check your nixpkgs version"
            fi
            if pkg-config --exists lua5.4 2>/dev/null; then
              echo "  Lua:          $(pkg-config --modversion lua5.4)"
            else
              echo "  Lua:          NOT FOUND"
            fi
          '';
        };
      });
}
