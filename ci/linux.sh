#!/usr/bin/env sh
#
# Linux CI command sequence.  Invoked by both .gitlab-ci.yml and by
# `make ci-linux` locally (which runs it inside the nixos/nix docker
# image to reproduce the exact CI environment on any dev machine).
#
# Do not add per-job branching logic here — if a job needs a different
# invocation, give it its own script.  The point is to keep CI/local
# commands byte-identical.

set -eu

# Warm the flake so timing later reflects real work, not eval.
nix develop .#ci --no-update-lock-file -c true

case "${1:-test}" in
build-gcc)
    nix develop .#ci --no-update-lock-file -c sh -c '
      make CC=gcc &&
      make clean &&
      make RELEASE=1 CC=gcc'
    ;;
build-clang)
    nix develop .#ci --no-update-lock-file -c sh -c '
      make CC=clang &&
      make clean &&
      make RELEASE=1 CC=clang'
    ;;
test-gcc | test)
    nix develop .#ci --no-update-lock-file -c sh -c '
      make CC=gcc &&
      make CC=gcc test'
    ;;
test-clang)
    nix develop .#ci --no-update-lock-file -c sh -c '
      make CC=clang &&
      make CC=clang test'
    ;;
docs)
    nix develop .#ci --no-update-lock-file -c sh -c '
      make CC=clang &&
      make docs-check'
    ;;
conformance)
    nix develop .#ci --no-update-lock-file -c sh -c '
      make lib RELEASE=1 &&
      make conformance RELEASE=1 &&
      ./build/conformance_runner --verbose'
    ;;
*)
    echo "usage: $0 {build-gcc|build-clang|test-gcc|test-clang|docs|conformance}" >&2
    exit 2
    ;;
esac
