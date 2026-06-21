#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ "$#" -lt 2 ]; then
  printf '%s\n' "usage: $0 <preset> <dev|release|package|cmdf-static|lua|fuzz> [cmake-args...]" >&2
  exit 2
fi

preset=$1
profile=$2
shift 2

common='-DLIBMDF_BUILD_STATIC=ON -DLIBMDF_BUILD_SHARED=ON'

case "$profile" in
  dev)
    exec cmake --preset "$preset" \
      $common \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=ON \
      -DLIBMDF_BUILD_EXAMPLES=ON \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=ON \
      -DLIBMDF_INSTALL_BINARY=ON \
      -DLIBMDF_CMDF_STATIC_RUNTIME=OFF \
      "$@"
    ;;
  release)
    exec cmake --preset "$preset" \
      $common \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=ON \
      -DLIBMDF_BUILD_EXAMPLES=ON \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=ON \
      -DLIBMDF_INSTALL_BINARY=ON \
      -DLIBMDF_CMDF_STATIC_RUNTIME=OFF \
      "$@"
    ;;
  package)
    exec cmake --preset "$preset" \
      $common \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=OFF \
      -DLIBMDF_BUILD_EXAMPLES=OFF \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=ON \
      -DLIBMDF_INSTALL_BINARY=OFF \
      -DLIBMDF_CMDF_STATIC_RUNTIME=ON \
      "$@"
    ;;
  cmdf-static)
    exec cmake --preset "$preset" \
      -DLIBMDF_BUILD_STATIC=ON \
      -DLIBMDF_BUILD_SHARED=OFF \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=OFF \
      -DLIBMDF_BUILD_EXAMPLES=OFF \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=OFF \
      -DLIBMDF_INSTALL_BINARY=OFF \
      -DLIBMDF_CMDF_STATIC_RUNTIME=ON \
      "$@"
    ;;
  lua)
    exec cmake --preset "$preset" \
      $common \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=ON \
      -DLIBMDF_BUILD_EXAMPLES=ON \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=ON \
      -DLIBMDF_INSTALL_BINARY=OFF \
      -DLIBMDF_CMDF_STATIC_RUNTIME=OFF \
      "$@"
    ;;
  fuzz)
    exec cmake --preset "$preset" \
      -DLIBMDF_BUILD_STATIC=ON \
      -DLIBMDF_BUILD_SHARED=OFF \
      -DLIBMDF_BUILD_BINARY=OFF \
      -DLIBMDF_BUILD_TESTS=OFF \
      -DLIBMDF_BUILD_EXAMPLES=OFF \
      -DLIBMDF_BUILD_FUZZERS=ON \
      -DLIBMDF_INSTALL=OFF \
      -DLIBMDF_INSTALL_BINARY=OFF \
      -DLIBMDF_CMDF_STATIC_RUNTIME=OFF \
      "$@"
    ;;
  *)
    printf '%s\n' "unknown configure profile: $profile" >&2
    exit 2
    ;;
esac
