{
    inputs = {
        nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
        parts.url = "github:hercules-ci/flake-parts";
        treefmt.url = "github:numtide/treefmt-nix";
        precommit.url = "github:cachix/pre-commit-hooks.nix";
    };

    outputs =
        inputs:
        inputs.parts.lib.mkFlake { inherit inputs; } {
            imports = [
                ./nix/mop.nix
                ./nix/devShells.nix
                ./nix/checks.nix
                ./nix/utils/treefmt.nix
                ./nix/utils/precommit.nix
            ];
            systems = inputs.nixpkgs.lib.systems.flakeExposed;
        };
}
