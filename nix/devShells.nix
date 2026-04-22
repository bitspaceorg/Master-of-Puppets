{ ... }:
{
    perSystem =
        {
            pkgs,
            self',
            config,
            mop-deps,
            ...
        }:
        {
            devShells.default = pkgs.mkShell {
                nativeBuildInputs = mop-deps.nativeBuildInputs;
                buildInputs = mop-deps.buildInputs;
                inputsFrom = [
                    self'.devShells.treefmt
                    self'.devShells.precommit
                ];
                shellHook = ''
                    echo "Master of Puppets — development shell"
                    echo "  Build:          make"
                    echo "  Build (+ VK):   make MOP_ENABLE_VULKAN=1"
                    echo "  Test:           make test"
                    echo "  Clean:          make clean"
                    echo "  Format:         treefmt"
                    echo "  Examples:       cd examples && nix develop"
                '';
            };

            # Minimal shell used by GitLab CI: build/test matrix (gcc + clang)
            # plus lua for the conformance runner. No treefmt / pre-commit.
            devShells.ci = pkgs.mkShell {
                nativeBuildInputs =
                    mop-deps.nativeBuildInputs
                    ++ (with pkgs; [
                        gcc
                        clang
                        lua5_4
                    ]);
                buildInputs = mop-deps.buildInputs;
            };
        };
}
