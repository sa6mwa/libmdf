#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
kind=${1:-api}; shift "$(( $# > 0 ? 1 : 0 ))"
rounds=10 warmups=2 repeat=1 no_build=0 no_lua=0 json=0
case "$kind" in api) input=testdata/code-review-is-a-dead-end.md;; cli) input=testdata/deck-corpus/comprehensive.md;; *) printf 'Benchmark kind must be api or cli\n' >&2; exit 2;; esac
while [[ $# -gt 0 ]]; do
  case "$1" in
    --rounds) rounds=$2; shift;; --warmups) warmups=$2; shift;; --repeat) repeat=$2; shift;;
    --input) input=$2; shift;; --no-build) no_build=1;; --no-lua) no_lua=1;; --json) json=1;;
    *) printf 'Unknown benchmark option: %s\n' "$1" >&2; exit 2;;
  esac
  shift
done
[[ $rounds =~ ^[1-9][0-9]*$ && $warmups =~ ^[0-9]+$ && $repeat =~ ^[1-9][0-9]*$ ]] || { printf 'Invalid benchmark sample counts\n' >&2; exit 2; }
[[ $kind != cli || $repeat == 1 ]] || { printf 'CLI samples measure one invocation; --repeat applies to API benchmarks\n' >&2; exit 2; }
[[ $input == /* ]] || input="$root/$input"
test -f "$input"
if [[ $no_build == 0 ]]; then "$root/scripts/build.sh" debug >&2; fi
runner="$root/build/debug/benchmark_command"
test -x "$runner"
export LUA_PATH="$root/build/debug/lua-tree/share/lua/5.5/?.lua;;"
export LUA_CPATH="$root/build/debug/lua-tree/lib/lua/5.5/?.so;;"
json_args=(); [[ $json == 0 ]] || json_args=(--json)
if [[ $json == 1 ]]; then printf '{"results":[\n'; else printf '| impl | mode | median_s | mean_s | min_s | max_s | rounds |\n| --- | --- | ---: | ---: | ---: | ---: | ---: |\n'; fi
first=1
for impl in C Lua; do
  [[ $impl != Lua || $no_lua == 0 ]] || continue
  modes=(ansi html); [[ $kind != cli ]] || modes+=(deck)
  for mode in "${modes[@]}"; do
    if [[ $kind == api ]]; then
      label="$impl API"
      args=("--$mode" --repeat "$repeat"); [[ $mode != ansi ]] || args+=(-w 80)
      if [[ $impl == C ]]; then command=("$root/build/debug/golden_render"); else command=("$root/build/debug/lua-runtime/bin/lua" "$root/scripts/benchmark_lua_render.lua"); fi
    else
      label=$impl; args=("--$mode"); [[ $mode != ansi ]] || args=(-b)
      if [[ $impl == C ]]; then command=("$root/build/debug/cmdf"); else command=("$root/build/debug/cmdf.sh"); fi
    fi
    [[ $json == 0 || $first == 1 ]] || printf ',\n'
    first=0
    "$runner" --rounds "$rounds" --warmups "$warmups" --repeat "$repeat" --label "$label" --mode "$mode" "${json_args[@]}" -- "${command[@]}" "${args[@]}" "$input"
  done
done
[[ $json == 0 ]] || printf ']}\n'
