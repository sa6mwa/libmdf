#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
source "$root/scripts/release_targets.sh"
for target in "${release_targets[@]}"; do "$root/scripts/cpkt-toolchains.sh" ensure "$target"; done
