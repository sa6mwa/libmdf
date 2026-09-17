#!/usr/bin/env bash
set -euo pipefail

# Resolve only lifecycle-pinned compiler collections. Linux must never fall
# back to a host-installed compiler or binutils collection.

die() {
  printf 'cpkt-toolchains: %s\n' "$*" >&2
  exit 1
}

cache_root() {
  if [[ -n "${CPKT_TOOLCHAIN_CACHE:-}" ]]; then
    printf '%s\n' "$CPKT_TOOLCHAIN_CACHE"
  elif [[ -n "${XDG_CACHE_HOME:-}" ]]; then
    printf '%s/c.pkt.systems/toolchains\n' "$XDG_CACHE_HOME"
  elif [[ -n "${HOME:-}" ]]; then
    printf '%s/.cache/c.pkt.systems/toolchains\n' "$HOME"
  else
    die 'HOME, XDG_CACHE_HOME, or CPKT_TOOLCHAIN_CACHE is required'
  fi
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    die 'sha256sum or shasum is required'
  fi
}

download_file() {
  local url=$1 destination=$2
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --connect-timeout 20 --output "$destination" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$destination" "$url"
  else
    die 'curl or wget is required to download Bootlin toolchains'
  fi
}

install_cleanup_trap() {
  local path=$1 remove_option=$2 cleanup
  printf -v cleanup 'status=$?; rm %s -- %q || :; trap - EXIT HUP INT TERM; exit "$status"' \
    "$remove_option" "$path"
  trap "$cleanup" EXIT
  trap 'exit 1' HUP INT TERM
}

with_cache_lock() {
  local lock_path=$1 lock_fd
  shift
  command -v flock >/dev/null 2>&1 || die 'flock is required to provision shared Linux toolchains'
  mkdir -p "$(dirname -- "$lock_path")"
  exec {lock_fd}>"$lock_path"
  flock -w "${CPKT_TOOLCHAIN_LOCK_TIMEOUT:-600}" "$lock_fd" || die "timed out waiting for shared toolchain lock: $lock_path"
  "$@"
  flock -u "$lock_fd"
  eval "exec ${lock_fd}>&-"
}

target_ids() {
  cat <<'TARGETS'
x86_64-linux-gnu
x86_64-linux-musl
aarch64-linux-gnu
aarch64-linux-musl
armhf-linux-gnu
armhf-linux-musl
arm64-apple-darwin
TARGETS
}

is_linux_target() {
  case "$1" in
    x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl) return 0 ;;
    *) return 1 ;;
  esac
}

require_target() {
  is_linux_target "$1" || [[ "$1" == arm64-apple-darwin ]] || die "unsupported target: $1"
}

bootlin_meta() {
  case "$1" in
    x86_64-linux-gnu)
      printf '%s\n' 'x86-64|x86-64--glibc--stable-2026.08-1|cde893afab04ac7dcd15c46aac214ff550441b982536124c88a71146a0eeedd3|x86_64-linux|x86_64-buildroot-linux-gnu/sysroot'
      ;;
    x86_64-linux-musl)
      printf '%s\n' 'x86-64|x86-64--musl--stable-2026.08-1|78d3a4683d6ac47b5ee73bd5bce210b55eb93dff1b137c61298af97eb0d2b5a6|x86_64-linux|x86_64-buildroot-linux-musl/sysroot'
      ;;
    aarch64-linux-gnu)
      printf '%s\n' 'aarch64|aarch64--glibc--stable-2026.08-1|0213efac9b5577f20d58de9431960a191347ffc2257b27ffe7250522bf1f7867|aarch64-linux|aarch64-buildroot-linux-gnu/sysroot'
      ;;
    aarch64-linux-musl)
      printf '%s\n' 'aarch64|aarch64--musl--stable-2026.08-1|b388c480a48e8e9f9b99e3d14e69219c4d61e5a2424a82faecb88a015b781a60|aarch64-linux|aarch64-buildroot-linux-musl/sysroot'
      ;;
    armhf-linux-gnu)
      printf '%s\n' 'armv7-eabihf|armv7-eabihf--glibc--stable-2026.08-1|9b7e25a74e87dac1e05d399444295e254a3073a056101e3197a859490e5701cd|arm-linux|arm-buildroot-linux-gnueabihf/sysroot'
      ;;
    armhf-linux-musl)
      printf '%s\n' 'armv7-eabihf|armv7-eabihf--musl--stable-2026.08-1|9147bafae4aa272321a3c6440d04d83b7e23411b2d344f875541d84c4444ba9b|arm-linux|arm-buildroot-linux-musleabihf/sysroot'
      ;;
    *) die "unsupported Bootlin target: $1" ;;
  esac
}

bootlin_values() {
  local meta arch name sha256 prefix sysroot_rel
  meta=$(bootlin_meta "$1")
  IFS='|' read -r arch name sha256 prefix sysroot_rel <<<"$meta"
  printf '%s|%s|%s|%s|%s|%s\n' "$arch" "$name" "$sha256" "$prefix" "$sysroot_rel" "$(cache_root)/roots/$name"
}

compiler_file() {
  "$1" -print-file-name="$2"
}

existing_compiler_file() {
  local path dir
  path=$(compiler_file "$1" "$2")
  [[ "$path" != "$2" && -f "$path" ]] || return 1
  dir=$(CDPATH= cd -- "$(dirname -- "$path")" && pwd -P)
  printf '%s/%s\n' "$dir" "$(basename -- "$path")"
}

bootlin_ready() {
  local root=$1 prefix=$2 sysroot=$3
  [[ -x "$root/bin/$prefix-gcc" ]] &&
    [[ -x "$root/bin/$prefix-g++" ]] &&
    [[ -x "$root/bin/$prefix-ld" ]] &&
    [[ -x "$root/bin/$prefix-ar" ]] &&
    [[ -x "$root/bin/$prefix-ranlib" ]] &&
    [[ -x "$root/bin/$prefix-strip" ]] &&
    [[ -x "$root/bin/$prefix-nm" ]] &&
    [[ -x "$root/bin/$prefix-objcopy" ]] &&
    [[ -x "$root/bin/$prefix-objdump" ]] &&
    [[ -x "$root/bin/$prefix-addr2line" ]] &&
    [[ -x "$root/bin/$prefix-gdb" ]] &&
    [[ -x "$root/bin/$prefix-readelf" ]] &&
    { [[ -f "$sysroot/usr/include/stdio.h" ]] || [[ -f "$sysroot/include/stdio.h" ]]; } &&
    { [[ -e "$sysroot/usr/lib/libc.so" ]] || [[ -e "$sysroot/lib/libc.so" ]] || [[ -e "$sysroot/lib/libc.so.6" ]]; } &&
    existing_compiler_file "$root/bin/$prefix-g++" libstdc++.a >/dev/null &&
    existing_compiler_file "$root/bin/$prefix-g++" libgcc.a >/dev/null
}

osxcross_candidate() {
  local root=${OSXCROSS_ROOT:-${HOME:-}/.local/cross/osxcross}
  local prefix=${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}
  [[ -x "$root/bin/$prefix-clang" ]] &&
    [[ -x "$root/bin/$prefix-clang++" ]] &&
    [[ -x "$root/bin/$prefix-ld" ]] &&
    [[ -x "$root/bin/$prefix-ar" ]] &&
    [[ -x "$root/bin/$prefix-ranlib" ]] &&
    [[ -x "$root/bin/$prefix-strip" ]] &&
    [[ -x "$root/bin/$prefix-nm" ]] &&
    [[ -x "$root/bin/$prefix-otool" ]] || return 1
  printf 'osxcross|%s|%s\n' "$root" "$prefix"
}

install_bootlin() {
  local target=$1 values arch name sha256 prefix sysroot_rel root
  values=$(bootlin_values "$target")
  IFS='|' read -r arch name sha256 prefix sysroot_rel root <<<"$values"
  if bootlin_ready "$root" "$prefix" "$root/$sysroot_rel"; then
    return
  fi
  with_cache_lock "$(cache_root)/locks/bootlin-$name.lock" install_bootlin_locked "$target"
}

install_bootlin_locked() {
  local target=$1 values arch name sha256 prefix sysroot_rel root archive_dir archive tmp extract actual
  values=$(bootlin_values "$target")
  IFS='|' read -r arch name sha256 prefix sysroot_rel root <<<"$values"
  if bootlin_ready "$root" "$prefix" "$root/$sysroot_rel"; then
    return
  fi

  archive_dir="$(cache_root)/archives"
  archive="$archive_dir/$name.tar.xz"
  mkdir -p "$archive_dir" "$(cache_root)/roots"
  if [[ -f "$archive" ]]; then
    actual=$(sha256_file "$archive")
    if [[ "$actual" != "$sha256" ]]; then
      printf 'cpkt-toolchains: discarding corrupt cached archive: %s\n' "$archive" >&2
      rm -f -- "$archive"
    fi
  fi
  if [[ ! -f "$archive" ]]; then
    tmp="$archive.tmp.$$"
    install_cleanup_trap "$tmp" -f
    download_file "https://toolchains.bootlin.com/downloads/releases/toolchains/$arch/tarballs/$name.tar.xz" "$tmp"
    actual=$(sha256_file "$tmp")
    [[ "$actual" == "$sha256" ]] || die "checksum mismatch for $name.tar.xz: expected $sha256, got $actual"
    mv "$tmp" "$archive"
    trap - EXIT HUP INT TERM
  fi

  extract="$(cache_root)/roots/.extract-$name.$$"
  install_cleanup_trap "$extract" -rf
  mkdir -p "$extract"
  tar -C "$extract" -xf "$archive"
  [[ -d "$extract/$name/bin" ]] || die "unexpected archive layout for $name.tar.xz"
  rm -rf "$root"
  mv "$extract/$name" "$root"
  rm -rf "$extract"
  trap - EXIT HUP INT TERM
  bootlin_ready "$root" "$prefix" "$root/$sysroot_rel" || die "incomplete extracted Bootlin toolchain: $root"
}

print_bootlin_target() {
  local target=$1 values arch name sha256 prefix sysroot_rel root cc cxx
  values=$(bootlin_values "$target")
  IFS='|' read -r arch name sha256 prefix sysroot_rel root <<<"$values"
  printf 'target=%s\ncache=%s\nsource=bootlin\narchive=%s.tar.xz\n' "$target" "$(cache_root)" "$name"
  if ! bootlin_ready "$root" "$prefix" "$root/$sysroot_rel"; then
    printf 'status=missing\ndownloadable=yes\nurl=https://toolchains.bootlin.com/downloads/releases/toolchains/%s/tarballs/%s.tar.xz\n' "$arch" "$name"
    return
  fi
  cc="$root/bin/$prefix-gcc"
  cxx="$root/bin/$prefix-g++"
  printf 'status=ready\nroot=%s\nprefix=%s\nsysroot=%s\nlibc=%s\n' "$root" "$prefix" "$root/$sysroot_rel" "${target##*-}"
  printf 'cc=%s\ncxx=%s\nld=%s\nar=%s\nranlib=%s\nstrip=%s\nnm=%s\nobjcopy=%s\nobjdump=%s\naddr2line=%s\ngdb=%s\nreadelf=%s\n' \
    "$cc" "$cxx" "$root/bin/$prefix-ld" "$root/bin/$prefix-ar" "$root/bin/$prefix-ranlib" "$root/bin/$prefix-strip" "$root/bin/$prefix-nm" "$root/bin/$prefix-objcopy" "$root/bin/$prefix-objdump" "$root/bin/$prefix-addr2line" "$root/bin/$prefix-gdb" "$root/bin/$prefix-readelf"
  printf 'target_triple=%s\nlibstdcxx_a=%s\nlibgcc_a=%s\n' "${sysroot_rel%/sysroot}" "$(existing_compiler_file "$cxx" libstdc++.a)" "$(existing_compiler_file "$cxx" libgcc.a)"
}

print_darwin_target() {
  local candidate source root prefix
  printf 'target=arm64-apple-darwin\ncache=%s\nsource=osxcross\ndownloadable=no\n' "$(cache_root)"
  if ! candidate=$(osxcross_candidate); then
    printf 'status=missing\nnote=Configure OSXCROSS_ROOT with a complete local osxcross SDK toolchain.\n'
    return
  fi
  IFS='|' read -r source root prefix <<<"$candidate"
  printf 'status=ready\nroot=%s\nprefix=%s\ncc=%s\ncxx=%s\nld=%s\nar=%s\nranlib=%s\nstrip=%s\nnm=%s\notool=%s\n' \
    "$root" "$prefix" "$root/bin/$prefix-clang" "$root/bin/$prefix-clang++" "$root/bin/$prefix-ld" "$root/bin/$prefix-ar" "$root/bin/$prefix-ranlib" "$root/bin/$prefix-strip" "$root/bin/$prefix-nm" "$root/bin/$prefix-otool"
}

report_target() {
  require_target "$1"
  if is_linux_target "$1"; then
    print_bootlin_target "$1"
  else
    print_darwin_target
  fi
}

ensure_target() {
  require_target "$1"
  if is_linux_target "$1"; then
    install_bootlin "$1"
  else
    osxcross_candidate >/dev/null || die 'arm64-apple-darwin requires a complete local osxcross SDK toolchain'
  fi
  report_target "$1"
}

print_env() {
  local target=$1 description key value
  description=$(report_target "$target")
  [[ "$description" == *$'status=ready'* ]] || die "target is missing; run: $0 ensure $target"
  for key in source root prefix sysroot cc cxx ld ar ranlib strip nm objcopy objdump addr2line gdb readelf libstdcxx_a libgcc_a otool; do
    value=$(printf '%s\n' "$description" | sed -n "s/^${key}=//p")
    [[ -z "$value" ]] || printf 'export %s=%q\n' "CPKT_TOOLCHAIN_${key^^}" "$value"
  done
  printf 'export CPKT_TARGET=%q\n' "$target"
  printf 'export CC=%q\n' "$(printf '%s\n' "$description" | sed -n 's/^cc=//p')"
  printf 'export CXX=%q\n' "$(printf '%s\n' "$description" | sed -n 's/^cxx=//p')"
  printf 'export LD=%q\n' "$(printf '%s\n' "$description" | sed -n 's/^ld=//p')"
  printf 'export AR=%q\n' "$(printf '%s\n' "$description" | sed -n 's/^ar=//p')"
  printf 'export RANLIB=%q\n' "$(printf '%s\n' "$description" | sed -n 's/^ranlib=//p')"
  printf 'export STRIP=%q\n' "$(printf '%s\n' "$description" | sed -n 's/^strip=//p')"
  printf 'export NM=%q\n' "$(printf '%s\n' "$description" | sed -n 's/^nm=//p')"
  value=$(printf '%s\n' "$description" | sed -n 's/^sysroot=//p')
  [[ -z "$value" ]] || printf 'export CPKT_SYSROOT=%q\n' "$value"
}

usage() {
  cat <<'USAGE'
usage: cpkt-toolchains.sh <command> [target-id]

Commands:
  targets              List lifecycle target ids.
  discover [target]    Report pinned collection status; without a target, report all.
  ensure <target|all>  Download, checksum-verify, and install pinned Linux collections.
  env <target>         Print shell exports for a ready collection.

Linux policy: every compiler, linker, binutil, and libc comes from the pinned
Bootlin collection. Host GCC, Clang, and binutils are never candidates.
Darwin policy: discover a local osxcross collection; do not download Apple SDKs.
USAGE
}

case "${1:-}" in
  -h|--help|'') usage ;;
  targets) target_ids ;;
  discover)
    if [[ $# -eq 2 ]]; then
      report_target "$2"
    elif [[ $# -eq 1 ]]; then
      first=1
      while IFS= read -r target; do
        [[ $first -eq 1 ]] || printf '\n'
        first=0
        report_target "$target"
      done < <(target_ids)
    else
      die 'usage: cpkt-toolchains.sh discover [target]'
    fi
    ;;
  ensure)
    [[ $# -eq 2 ]] || die 'usage: cpkt-toolchains.sh ensure <target|all>'
    if [[ "$2" == all ]]; then
      while IFS= read -r target; do
        if is_linux_target "$target"; then
          ensure_target "$target"
        else
          report_target "$target"
        fi
      done < <(target_ids)
    else
      ensure_target "$2"
    fi
    ;;
  env)
    [[ $# -eq 2 ]] || die 'usage: cpkt-toolchains.sh env <target>'
    print_env "$2"
    ;;
  *) die "unknown command: $1" ;;
esac
