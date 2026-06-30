#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TARGET=${LIBMDF_TARGET_ID:-}
PRESET=
CACHE=

usage() {
  printf '%s\n' "usage: $0 [--root DIR] [--target TARGET] [--preset PRESET] [--cache CMakeCache.txt] <tool>" >&2
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --root)
      ROOT=$2
      shift 2
      ;;
    --target)
      TARGET=$2
      shift 2
      ;;
    --preset)
      PRESET=$2
      shift 2
      ;;
    --cache)
      CACHE=$2
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --*)
      usage
      exit 2
      ;;
    *)
      break
      ;;
  esac
done

if [ "$#" -ne 1 ]; then
  usage
  exit 2
fi

TOOL=$1
if [ -z "$TARGET" ] && [ -n "$PRESET" ]; then
  TARGET=${PRESET%-release}
fi

cache_value() {
  cache=$1
  key=$2
  sed -n "s/^$key:[^=]*=//p" "$cache" 2>/dev/null | sed -n '1p'
}

emit_if_tool() {
  candidate=$1
  if [ -n "$candidate" ] && [ -x "$candidate" ] && [ ! -d "$candidate" ]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  return 1
}

env_value() {
  name=$1
  eval "printf '%s\n' \"\${$name:-}\""
}

tool_env_names() {
  case "$TOOL" in
    otool)
      printf '%s\n' LIBMDF_OTOOL CPKT_OTOOL CMAKE_OTOOL
      ;;
    install_name_tool)
      printf '%s\n' LIBMDF_INSTALL_NAME_TOOL CPKT_INSTALL_NAME_TOOL CMAKE_INSTALL_NAME_TOOL
      ;;
    strip)
      printf '%s\n' LIBMDF_STRIP CPKT_STRIP CMAKE_STRIP
      ;;
    *)
      upper=$(printf '%s\n' "$TOOL" | tr 'abcdefghijklmnopqrstuvwxyz' 'ABCDEFGHIJKLMNOPQRSTUVWXYZ')
      printf '%s\n' "LIBMDF_$upper" "CPKT_$upper" "CMAKE_$upper"
      ;;
  esac
}

cache_tool_keys() {
  case "$TOOL" in
    otool)
      printf '%s\n' CMAKE_OTOOL CPKT_OTOOL
      ;;
    install_name_tool)
      printf '%s\n' CMAKE_INSTALL_NAME_TOOL CPKT_INSTALL_NAME_TOOL
      ;;
    strip)
      printf '%s\n' CMAKE_STRIP CPKT_STRIP
      ;;
  esac
}

cache_files() {
  if [ -n "$CACHE" ]; then
    [ -f "$CACHE" ] && printf '%s\n' "$CACHE"
    return 0
  fi
  if [ -n "$PRESET" ] && [ -f "$ROOT/build/$PRESET/CMakeCache.txt" ]; then
    printf '%s\n' "$ROOT/build/$PRESET/CMakeCache.txt"
    return 0
  fi
  if [ -n "$PRESET" ]; then
    return 0
  fi
  find "$ROOT/build" -name CMakeCache.txt -type f 2>/dev/null | sort
}

compiler_prefix() {
  compiler=$1
  base=$(basename "$compiler")
  case "$base" in
    *-clang|*-clang++|*-gcc|*-cc)
      printf '%s\n' "${base%-clang}" | sed 's/-clang++$//;s/-gcc$//;s/-cc$//'
      ;;
  esac
}

host_prefixes() {
  if [ -n "${CPKT_OSXCROSS_HOST:-}" ]; then
    printf '%s\n' "$CPKT_OSXCROSS_HOST"
  fi
  for cache in $(cache_files); do
    compiler=$(cache_value "$cache" CMAKE_C_COMPILER)
    if [ -n "$compiler" ]; then
      compiler_prefix "$compiler"
    fi
  done
  case "$TARGET" in
    arm64-apple-darwin)
      printf '%s\n' arm64-apple-darwin25
      ;;
    *-apple-darwin)
      printf '%s\n' "$TARGET" "${TARGET}25"
      ;;
  esac
}

is_cross_darwin() {
  case "$TARGET" in
    *-apple-darwin)
      if [ "$(uname -s 2>/dev/null || printf unknown)" != "Darwin" ]; then
        return 0
      fi
      ;;
  esac
  return 1
}

darwin_path_allowed() {
  candidate=$1
  base=$(basename "$candidate")
  if ! is_cross_darwin; then
    return 0
  fi
  case "$base" in
    *-"$TOOL")
      return 0
      ;;
  esac
  case "$candidate" in
    "${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}"/*)
      return 0
      ;;
  esac
  return 1
}

for name in $(tool_env_names); do
  value=$(env_value "$name")
  if emit_if_tool "$value"; then
    exit 0
  fi
done

for cache in $(cache_files); do
  for key in $(cache_tool_keys); do
    value=$(cache_value "$cache" "$key")
    if emit_if_tool "$value"; then
      exit 0
    fi
  done
done

for cache in $(cache_files); do
  compiler=$(cache_value "$cache" CMAKE_C_COMPILER)
  if [ -n "$compiler" ]; then
    dir=$(dirname "$compiler")
    for prefix in $(host_prefixes); do
      emit_if_tool "$dir/$prefix-$TOOL" && exit 0
    done
    emit_if_tool "$dir/$TOOL" && exit 0
  fi
done

OSXCROSS_ROOT=${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}
for prefix in $(host_prefixes); do
  emit_if_tool "$OSXCROSS_ROOT/bin/$prefix-$TOOL" && exit 0
done

for prefix in $(host_prefixes); do
  if command -v "$prefix-$TOOL" >/dev/null 2>&1; then
    command -v "$prefix-$TOOL"
    exit 0
  fi
done

if command -v "$TOOL" >/dev/null 2>&1; then
  candidate=$(command -v "$TOOL")
  if darwin_path_allowed "$candidate"; then
    printf '%s\n' "$candidate"
    exit 0
  fi
fi

exit 1
