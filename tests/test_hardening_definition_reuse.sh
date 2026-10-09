#!/usr/bin/env bash
# Exercise the actual hardening stamp producers with tiny local test inputs.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-hardening-definition-reuse"
rm -rf -- "$work"
for scope in fuzz parity; do
  source="$work/$scope/source"
  build="$work/$scope/build"
  mkdir -p "$source/cmake" "$source/scripts" "$source/tests" "$source/fuzz/corpus" "$source/parityjudge" "$source/include/libmdf"
  cp "$root/cmake/Fuzz.cmake" "$root/cmake/Parity.cmake" "$source/cmake/"
  printf 'int fixture(void) { return 0; }\n' > "$source/fixture.c"
  printf 'int main(void) { return 0; }\n' > "$source/tests/afl_gate_fixture.c"
  printf '#define FIXTURE_HEADER 1\n' > "$source/include/libmdf/mdf.h"
  for script in run_fuzz run_afl_gate cpkt-aflpp parity stream_parity; do
    printf '#!/bin/sh\nprintf "fixture-parity\\n"\n' > "$source/scripts/$script.sh"
    chmod +x "$source/scripts/$script.sh"
  done
  printf '#!/bin/sh\nexit 0\n' > "$source/tests/test_afl_gate.sh"
  printf 'module fixture.local/parity\n\ngo 1.26.4\n' > "$source/parityjudge/go.mod"
  touch "$source/parityjudge/go.sum"
  cat > "$source/parityjudge/main.go" <<'GO'
package main
import "C"
func main() {}
GO
  cat > "$source/parityjudge/main_test.go" <<'GO'
package main
import "testing"
func TestFixture(t *testing.T) {}
GO
  cat > "$source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
project(hardening_reuse C)
enable_testing()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS lifecycle=1)
function(libmdf_configure_c89_target target)
endfunction()
set(LIBMDF_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(SCOPE STREQUAL "fuzz")
  add_executable(fuzz_smoke IMPORTED)
  set_target_properties(fuzz_smoke PROPERTIES IMPORTED_LOCATION /bin/true)
  include(cmake/Fuzz.cmake)
else()
  add_library(mdf_shared SHARED fixture.c)
  set_target_properties(mdf_shared PROPERTIES OUTPUT_NAME mdf)
  include(cmake/Parity.cmake)
endif()
CMAKE
  cmake -S "$source" -B "$build" -G Ninja -DSCOPE="$scope" \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/x86_64-linux-gnu.cmake" > "$work/$scope-configure.log"
  if [[ $scope == fuzz ]]; then target=check-fuzz; stamp="$build/checks/fuzz.pass";
  else target=check-quick-parity-ansi; stamp="$build/checks/quick-parity_ansi.pass"; fi
  cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" --target "$target" > "$work/$scope-first.log" 2>&1
  test -f "$stamp"
  if [[ $scope == fuzz ]]; then
    printf '\nadd_test(NAME introduced_failure COMMAND /bin/false)\nset_tests_properties(introduced_failure PROPERTIES LABELS fuzz)\n' >> "$source/cmake/Fuzz.cmake"
  else
    printf '\nset_tests_properties(quick-parity_ansi PROPERTIES FAIL_REGULAR_EXPRESSION fixture-parity)\n' >> "$source/cmake/Parity.cmake"
  fi
  if cmake --build "$build" --target "$target" > "$work/$scope-changed.log" 2>&1; then
    printf 'Changed %s test definitions left a stale success record\n' "$scope" >&2; exit 1;
  fi
  test ! -f "$stamp"
done
