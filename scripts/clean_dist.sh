#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[[ ! -L $root/dist ]] || { printf 'Refusing to clean borrowed dist\n' >&2; exit 1; }
rm -rf -- "$root/dist"
