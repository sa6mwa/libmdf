#!/usr/bin/env bash
set -euo pipefail
runner=$1
work=$2
mkdir -p "$work"
"$runner" --rounds 2 --warmups 0 --json --label 'a"b' -- /bin/true > "$work/result.json"
grep -Fq '"impl":"a\"b"' "$work/result.json"
if "$runner" --rounds 0 -- /bin/true > "$work/invalid.log" 2>&1; then exit 1; fi
if "$runner" --rounds 1 --warmups 0 -- /bin/false > "$work/failure.log" 2>&1; then exit 1; fi
"$runner" --compare 1.05 1 0.05
if "$runner" --compare 1.051 1 0.05; then exit 1; fi
if "$runner" --compare nan 1 0.05; then exit 1; fi
if "$runner" --compare 0 0 0.05; then exit 1; fi
