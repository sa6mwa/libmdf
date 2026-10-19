#!/usr/bin/env sh
# Run a bounded AFL++ job and preserve failures and crash reproducers.
set -eu
afl=$1
seconds=$2
corpus=$3
output=$4
shift 4
status=0
AFL_NO_UI=1 AFL_NO_AFFINITY=1 AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
  "$afl" -V "$seconds" -i "$corpus" -o "$output" -- "$@" || status=$?
crashes=$(find "$output" -type f -path '*/crashes/id:*' -print)
if [ -n "$crashes" ]; then
  printf 'AFL++ discovered crashes; retained reproducers:\n%s\n' "$crashes" >&2
  exit 1
fi
exit "$status"
