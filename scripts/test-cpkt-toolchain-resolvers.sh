#!/usr/bin/env bash
set -euo pipefail
skill_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
resolver="$skill_dir/scripts/cpkt-toolchains.sh"
work=$(mktemp -d "$TMPDIR/toolchain-fixture.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
fail() { printf 'toolchain fixture: %s\n' "$*" >&2; exit 1; }

# Synthetic local inputs; no real cache, network or compiler.
collection="$work/cache/roots/x86-64--glibc--stable-2026.08-1"
sysroot="$collection/x86_64-buildroot-linux-gnu/sysroot"
mkdir -p "$collection/bin" "$collection/runtime" "$sysroot/usr/include" "$sysroot/usr/lib"
touch "$sysroot/usr/include/stdio.h" "$sysroot/usr/lib/libc.so"
touch "$collection/runtime/libstdc++.a" "$collection/runtime/libgcc.a"
for tool in gcc ld ar ranlib strip nm objcopy objdump addr2line gdb readelf; do
  printf '#!/bin/sh\nexit 0\n' > "$collection/bin/x86_64-linux-$tool"
  chmod +x "$collection/bin/x86_64-linux-$tool"
done
cat > "$collection/bin/x86_64-linux-g++" <<'COMPILER'
#!/bin/sh
case "$1" in
  -print-file-name=libstdc++.a) printf '%s/runtime/libstdc++.a\n' "$TEST_COLLECTION";;
  -print-file-name=libgcc.a) printf '%s/runtime/libgcc.a\n' "$TEST_COLLECTION";;
  *) exit 1;;
esac
COMPILER
chmod +x "$collection/bin/x86_64-linux-g++"
export TEST_COLLECTION="$collection" CPKT_TOOLCHAIN_CACHE="$work/cache"
description=$("$resolver" discover x86_64-linux-gnu)
grep -Fxq 'status=ready' <<< "$description" || fail 'prepared collection unavailable'
environment=$("$resolver" env x86_64-linux-gnu)
(
  eval "$environment"
  [[ "$CC" = "$collection/bin/x86_64-linux-gcc" && "$LD" = "$collection/bin/x86_64-linux-ld" ]] ||
    fail 'wrong tools exported'
)
for required in "$collection/bin/x86_64-linux-gcc" "$collection/runtime/libstdc++.a"; do
  mv "$required" "$required.saved"
  description=$("$resolver" discover x86_64-linux-gnu)
  grep -Fxq 'status=missing' <<< "$description" || fail 'missing prerequisite accepted'
  if "$resolver" env x86_64-linux-gnu > "$work/out" 2> "$work/err"; then fail 'invalid exports succeeded'; fi
  [[ ! -s "$work/out" && ! -e "$required" ]] || fail 'discovery repaired or exported missing state'
  mv "$required.saved" "$required"
done
if CPKT_TOOLCHAIN_CACHE="$work/status=ready" "$resolver" env x86_64-linux-gnu > "$work/out" 2> "$work/err"; then
  fail 'cache pathname supplied readiness'
fi
[[ ! -s "$work/out" && ! -e "$work/status=ready" ]] || fail 'missing lookup generated state'

# A missing cache selection must fail before reporting paths or doing any work.
for mode in discover env ensure; do
  if env -u HOME -u XDG_CACHE_HOME -u CPKT_TOOLCHAIN_CACHE "$resolver" "$mode" x86_64-linux-gnu > "$work/out" 2> "$work/err"; then
    fail 'missing cache selection accepted'
  fi
  [[ ! -s "$work/out" ]] || fail 'missing cache selection reported paths'
  grep -q 'HOME, XDG_CACHE_HOME, or CPKT_TOOLCHAIN_CACHE is required' "$work/err" || fail 'wrong cache diagnostic'
done

# Hash file content: filename escaping must neither invalidate nor delete a hit.
source "$skill_dir/scripts/cpkt-archive-cache.sh"
archive="$work/archive\\name"
printf 'local archive fixture\n' > "$archive"
digest=$(printf 'local archive fixture\n' | sha256sum | awk '{print $1}')
[[ $(cpkt_cached_archive_sha256 "$archive") == "$digest" ]] || fail 'escaped filename changed digest'
cpkt_restore_cached_archive "$archive" "$digest"
[[ -f "$archive" ]] || fail 'valid escaped-path hit deleted'
printf 'toolchain discovery contract passed\n'
