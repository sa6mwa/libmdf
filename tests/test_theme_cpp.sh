#!/usr/bin/env bash
set -euo pipefail
cxx=$1
source_file=$2
generated_include=$3
source_include=$4
library=$5
output=$6
"$cxx" -std=c++98 -pedantic-errors -Wall -Wextra -Werror -x c++ "$source_file" \
  -I"$generated_include" -I"$source_include" -x none "$library" \
  -Wl,-rpath,"$(dirname "$library")" -o "$output"
"$output"
