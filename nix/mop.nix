{ ... }:
{
    perSystem =
        { pkgs, ... }:
        let
            python = pkgs.python3.withPackages (ps: [ ps.python-frontmatter ]);
        in
        {
            _module.args.mop-deps = {
                buildInputs =
                    with pkgs;
                    [
                        lua5_4
                        vulkan-loader
                        vulkan-headers
                        vulkan-validation-layers
                        shaderc
                    ]
                    ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ moltenvk ];
                nativeBuildInputs =
                    with pkgs;
                    [
                        gnumake
                        clang-tools
                        pkg-config
                        python
                    ]
                    ++ pkgs.lib.optionals (!pkgs.stdenv.isDarwin) [
                        valgrind
                        gdb
                    ];
            };

            packages.default = pkgs.stdenv.mkDerivation {
                pname = "master-of-puppets";
                version = "0.1.0";
                src = ./..;

                nativeBuildInputs = with pkgs; [
                    gnumake
                    pkg-config
                ];
                buildInputs = with pkgs; [ lua5_4 ];

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
        };
}
