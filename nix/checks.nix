{ ... }:
{
    perSystem =
        { self', pkgs, ... }:
        {
            checks.unit-test = self'.packages.default.overrideAttrs (oldAttrs: {
                name = "mop-unit-test";

                doCheck = true;
                checkPhase = ''
                    make test CC=${pkgs.stdenv.cc}/bin/cc
                '';
            });
        };
}
