#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
for script in "$root"/scripts/*.sh "$root"/tests/*.sh; do bash -n "$script"; done
