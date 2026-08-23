#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE="$ROOT/build/test-cmake-options"

unset CC CFLAGS CPPFLAGS LDFLAGS

expect_rejected() {
  name=$1
  message=$2
  shift 2
  build="$BASE/$name"
  log="$BASE/$name.log"

  rm -rf "$build"
  mkdir -p "$(dirname "$log")"
  if cmake -S "$ROOT" -B "$build" -G Ninja "$@" >"$log" 2>&1; then
    printf '%s\n' "expected CMake configure to reject $name" >&2
    exit 1
  fi
  grep -q "$message" "$log"
}

write_consumer_sources() {
  dir=$1

  mkdir -p "$dir"
  cat >"$dir/main.c" <<'EOF'
#include <libmdf.h>

int main(void)
{
    mdf_options opts;
    mdf_options_init(&opts);
    return opts.width == 0 ? 0 : 1;
}
EOF

  cat >"$dir/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(libmdf_consumer C)
find_package(libmdf CONFIG REQUIRED)
add_executable(consumer main.c)
if(TARGET libmdf::mdf_static)
  target_link_libraries(consumer PRIVATE libmdf::mdf_static)
elseif(TARGET libmdf::mdf_shared)
  target_link_libraries(consumer PRIVATE libmdf::mdf_shared)
else()
  message(FATAL_ERROR "libmdf package exports no usable library target")
endif()
EOF
}

verify_install_tree() {
  name=$1
  pcdir=$2
  cmakedir=$3
  includedir=$4
  shift
  shift
  shift
  shift
  build="$BASE/$name/build"
  install="$BASE/$name/install"
  consumer="$BASE/$name/consumer"
  pkg="$BASE/$name/pkg"
  log="$BASE/$name.log"

  rm -rf "$BASE/$name"
  mkdir -p "$BASE/$name"
  cmake -S "$ROOT" -B "$build" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$install" \
    -DLIBMDF_BUILD_BINARY=OFF \
    -DLIBMDF_BUILD_TESTS=OFF \
    -DLIBMDF_BUILD_EXAMPLES=OFF \
    -DLIBMDF_BUILD_FUZZERS=OFF \
    "$@" >"$log" 2>&1
  cmake --build "$build" >>"$log" 2>&1
  cmake --install "$build" >>"$log" 2>&1

  test -f "$install/$includedir/libmdf.h"
  test -f "$install/$includedir/libmdf/mdf.h"
  test -f "$install/$cmakedir/libmdfConfig.cmake"
  test -f "$install/$cmakedir/libmdfTargets.cmake"
  test -f "$install/$pcdir/libmdf.pc"
  test -f "$install/share/doc/libmdf/OFL.txt"
  libdir=$(dirname "$install/$pcdir")
  if test -f "$libdir/libmdf.so"; then
    test -f "$libdir/libmdf.so.2"
  fi

  write_consumer_sources "$consumer/src"
  cmake -S "$consumer/src" -B "$consumer/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$install" >>"$log" 2>&1
  cmake --build "$consumer/build" >>"$log" 2>&1

  mkdir -p "$pkg"
  cc $(PKG_CONFIG_PATH="$install/$pcdir" pkg-config --cflags libmdf) \
    "$consumer/src/main.c" \
    $(PKG_CONFIG_PATH="$install/$pcdir" pkg-config --libs libmdf) \
    -o "$pkg/consumer" >>"$log" 2>&1
}

verify_absolute_pkg_config() {
  name=$1
  build="$BASE/$name/build"
  install="$BASE/$name/install"
  abs_lib="$BASE/$name/absolute-lib"
  pcdir="$abs_lib/pkgconfig"
  consumer="$BASE/$name/consumer"
  pkg="$BASE/$name/pkg"
  log="$BASE/$name.log"

  rm -rf "$BASE/$name"
  mkdir -p "$BASE/$name"
  cmake -S "$ROOT" -B "$build" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$install" \
    -DCMAKE_INSTALL_LIBDIR="$abs_lib" \
    -DLIBMDF_BUILD_STATIC=ON \
    -DLIBMDF_BUILD_SHARED=OFF \
    -DLIBMDF_BUILD_BINARY=OFF \
    -DLIBMDF_BUILD_TESTS=OFF \
    -DLIBMDF_BUILD_EXAMPLES=OFF \
    -DLIBMDF_BUILD_FUZZERS=OFF >"$log" 2>&1
  cmake --build "$build" >>"$log" 2>&1
  cmake --install "$build" >>"$log" 2>&1

  test -f "$abs_lib/libmdf.a"
  test -f "$pcdir/libmdf.pc"
  write_consumer_sources "$consumer/src"
  mkdir -p "$pkg"
  cc $(PKG_CONFIG_PATH="$pcdir" pkg-config --cflags libmdf) \
    "$consumer/src/main.c" \
    $(PKG_CONFIG_PATH="$pcdir" pkg-config --libs libmdf) \
    -o "$pkg/consumer" >>"$log" 2>&1
}

expect_rejected no-libraries 'libmdf requires LIBMDF_BUILD_STATIC or LIBMDF_BUILD_SHARED' \
  -DLIBMDF_BUILD_STATIC=OFF \
  -DLIBMDF_BUILD_SHARED=OFF \
  -DLIBMDF_INSTALL=OFF \
  -DLIBMDF_BUILD_BINARY=OFF \
  -DLIBMDF_BUILD_TESTS=OFF \
  -DLIBMDF_BUILD_EXAMPLES=OFF \
  -DLIBMDF_BUILD_FUZZERS=OFF

verify_install_tree static-only lib/pkgconfig lib/cmake/libmdf include \
  -DLIBMDF_BUILD_STATIC=ON \
  -DLIBMDF_BUILD_SHARED=OFF

verify_install_tree shared-only lib/pkgconfig lib/cmake/libmdf include \
  -DLIBMDF_BUILD_STATIC=OFF \
  -DLIBMDF_BUILD_SHARED=ON

verify_install_tree static-and-shared lib/pkgconfig lib/cmake/libmdf include \
  -DLIBMDF_BUILD_STATIC=ON \
  -DLIBMDF_BUILD_SHARED=ON

verify_install_tree multiarch-libdir lib/x86_64-linux-gnu/pkgconfig lib/x86_64-linux-gnu/cmake/libmdf include \
  -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu \
  -DLIBMDF_BUILD_STATIC=ON \
  -DLIBMDF_BUILD_SHARED=OFF

verify_install_tree multi-component-include lib/pkgconfig lib/cmake/libmdf include/libmdf-sdk \
  -DCMAKE_INSTALL_INCLUDEDIR=include/libmdf-sdk \
  -DLIBMDF_BUILD_STATIC=ON \
  -DLIBMDF_BUILD_SHARED=OFF

verify_absolute_pkg_config absolute-libdir
