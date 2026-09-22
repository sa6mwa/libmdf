#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "$ROOT/build/lua-build-layout.XXXXXX")

cleanup() {
  rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

assert_no_source_module() {
  if [[ -e "$ROOT/libmdf/core.so" || -d "$ROOT/libmdf" ]]; then
    printf '%s\n' "Lua rock build leaked a source-tree module under $ROOT/libmdf" >&2
    exit 1
  fi
}

assert_no_source_module
(
  cd "$ROOT"
  "$ROOT/scripts/build_lua_rock.sh"
)
assert_no_source_module
(
  cd "$WORK"
  "$ROOT/scripts/build_lua_rock.sh"
)
assert_no_source_module
