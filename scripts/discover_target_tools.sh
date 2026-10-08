#!/usr/bin/env bash
# Target-specific tools: explicit selection, configured cache, matching sibling,
# then the pinned resolver. Never inspect unrelated build trees or host binutils.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
build_root=$root
target=${LIBMDF_TARGET_ID:-}
preset= cache=
while [[ $# -gt 1 ]]; do
  case "$1" in
    --root) build_root=$2;;
    --target) target=$2;;
    --preset) preset=$2;;
    --cache) cache=$2;;
    *) printf 'Unknown tool selection option: %s\n' "$1" >&2; exit 2;;
  esac
  shift 2
done
[[ $# == 1 ]] || { printf 'usage: discover_target_tools.sh [--root DIR] [--target TARGET] [--preset PRESET] [--cache FILE] <tool>\n' >&2; exit 2; }
tool=$1
case "$tool" in cc|cxx|ld|ar|ranlib|strip|nm|objcopy|objdump|addr2line|readelf|otool|install_name_tool) ;; *) printf 'Unknown target tool: %s\n' "$tool" >&2; exit 2;; esac
[[ -n $target || -z $preset ]] || target=${preset%-release}
[[ -n $cache || -z $preset ]] || cache="$build_root/build/$preset/CMakeCache.txt"
cache_value() { [[ ! -f $cache ]] || sed -n "s/^$1:[^=]*=//p" "$cache" | head -1; }
emit() { [[ -n $1 && -x $1 && ! -d $1 ]] && { printf '%s\n' "$1"; return 0; }; return 1; }
upper=${tool^^}
for variable in "LIBMDF_$upper" "CPKT_$upper" "CMAKE_$upper"; do
  if [[ -n ${!variable:-} ]]; then
    emit "${!variable}" && exit 0
    printf 'Selected %s is unavailable: %s\n' "$variable" "${!variable}" >&2
    exit 1
  fi
done
cached_target=$(cache_value LIBMDF_TARGET_ID)
[[ -z $target || -z $cached_target || $target == "$cached_target" ]] || {
  printf 'Configured target %s does not match selected target %s\n' "$cached_target" "$target" >&2; exit 1;
}
[[ -n $target ]] || target=$cached_target
emit "$(cache_value "CMAKE_$upper")" && exit 0
compiler=$(cache_value CMAKE_C_COMPILER)
if [[ -n $compiler ]]; then
  prefix=$(basename "$compiler")
  prefix=${prefix%-clang}; prefix=${prefix%-gcc}; prefix=${prefix%-cc}
  emit "$(dirname "$compiler")/$prefix-$tool" && exit 0
fi
[[ -n $target ]] || { printf 'Select a target or configured preset before resolving %s\n' "$tool" >&2; exit 1; }
description=$("$root/scripts/cpkt-toolchains.sh" discover "$target")
grep -Fxq status=ready <<< "$description" || { printf 'Missing pinned target tools; prepare: %s/scripts/cpkt-toolchains.sh ensure %s\n' "$root" "$target" >&2; exit 1; }
emit "$(sed -n "s/^$tool=//p" <<< "$description")" && exit 0
printf 'The selected target does not provide %s: %s\n' "$tool" "$target" >&2
exit 1
