#!/usr/bin/env bash
# Exercise the actual hardening stamp producers with tiny local test inputs.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-hardening-definition-reuse"
rm -rf -- "$work"
for scope in fuzz parity; do
  source="$work/$scope/source"
  build="$work/$scope/build"
  mkdir -p "$source/cmake" "$source/scripts" "$source/tests" "$source/fuzz/corpus" "$source/parityjudge" "$source/include/libmdf" "$source/testdata"
  touch "$source/fuzz/corpus/required" "$source/testdata/required"
  cp "$root/cmake/Fuzz.cmake" "$root/cmake/Parity.cmake" "$source/cmake/"
  cp "$root/cmake/StableInputs.cmake" "$root/cmake/CacheInput.cmake" "$source/cmake/"
  printf 'int fixture(void) { return 0; }\n' > "$source/fixture.c"
  printf 'int main(void) { return 0; }\n' > "$source/tests/afl_gate_fixture.c"
  printf '#define FIXTURE_HEADER 1\n' > "$source/include/libmdf/mdf.h"
  for script in run_fuzz run_afl_gate cpkt-aflpp parity stream_parity; do
    printf '#!/bin/sh\nprintf "fixture-parity\\n"\n' > "$source/scripts/$script.sh"
    chmod +x "$source/scripts/$script.sh"
  done
  cat > "$source/scripts/run_fuzz.sh" <<'SCRIPT'
#!/bin/sh
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
test -f "$root/fuzz/corpus/required"
SCRIPT
  cat > "$source/scripts/parity.sh" <<'SCRIPT'
#!/bin/sh
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
test -f "$root/testdata/required" || exit 1
printf 'fixture-parity\n'
SCRIPT
  printf '#!/bin/sh\nexit 0\n' > "$source/tests/test_afl_gate.sh"
  printf 'module fixture.local/parity\n\ngo 1.26.4\n' > "$source/parityjudge/go.mod"
  touch "$source/parityjudge/go.sum"
  cat > "$source/parityjudge/main.go" <<'GO'
package main
/* int requiredBridge(void); */
import "C"
func init() { C.requiredBridge() }
func main() {}
GO
  printf 'int requiredBridge(void) { return 0; }\n' > "$source/parityjudge/bridge.c"
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
  if [[ $scope == parity ]]; then
    cmake --build "$build" --target parity-tools > "$work/parity-producers-first.log" 2>&1
  fi
  cp "$build/CMakeCache.txt" "$work/$scope-first-cache.txt"
  cmake -S "$source" -B "$build" > "$work/$scope-warm-configure.log"
  cmake --build "$build" --target "$target" > "$work/$scope-warm.log"
  if grep -q 'Test project' "$work/$scope-warm.log"; then
    diff -u "$work/$scope-first-cache.txt" "$build/CMakeCache.txt" || true
    printf 'No-op configuration reran the %s suite\n' "$scope" >&2; exit 1;
  fi
  if [[ $scope == fuzz ]]; then input="$source/fuzz/corpus/required"; else input="$source/testdata/required"; fi
  rm "$input"
  if cmake --build "$build" --target "$target" > "$work/$scope-deleted.log" 2>&1; then
    printf 'Deleted %s input left a stale success record\n' "$scope" >&2; exit 1;
  fi
  test ! -f "$stamp"
  touch "$input"
  cmake --build "$build" --target "$target" > "$work/$scope-restored.log" 2>&1
  if [[ $scope == parity ]]; then
    rm "$source/parityjudge/bridge.c"
    if cmake --build "$build" --target parity-tools > "$work/parity-source-deleted.log" 2>&1; then
      printf 'Deleted parity bridge reused stale Go executables\n' >&2; exit 1;
    fi
    test ! -f "$build/parity-source/bridge.c"
    printf 'int requiredBridge(void) { return 0; }\n' > "$source/parityjudge/bridge.c"
    cmake --build "$build" --target parity-tools > "$work/parity-source-restored.log" 2>&1
    mv "$source/parityjudge/main.go" "$source/parityjudge/cli.go"
    printf '\nfunc renamedSourceProbe() {}\n' >> "$source/parityjudge/cli.go"
    cmake --build "$build" --target "$target" > "$work/parity-renamed.log" 2>&1
    test ! -f "$build/parity-source/main.go"
    test -f "$build/parity-source/cli.go"
    cp "$source/parityjudge/main_test.go" "$work/last-test.go"
    rm "$source/parityjudge/main_test.go"
    if cmake --build "$build" --target parity-tools > "$work/parity-empty-tests.log" 2>&1; then
      printf 'Empty Go test suite reused a stale test executable\n' >&2; exit 1;
    fi
    test ! -f "$build/parityjudge-test"
    cp "$work/last-test.go" "$source/parityjudge/main_test.go"
    cmake --build "$build" --target parity-tools > "$work/parity-tests-restored.log" 2>&1
    ctest --test-dir "$build" -R '^parityjudge_suite_stdin$' --output-on-failure --stop-on-failure --no-tests=error -j1 > "$work/parity-suite-valid.log"
    printf 'package main\n' > "$source/parityjudge/main_test.go"
    cmake --build "$build" --target parity-tools > "$work/parity-helper-only.log" 2>&1
    if ctest --test-dir "$build" -R '^parityjudge_suite_stdin$' --output-on-failure --stop-on-failure --no-tests=error -j1 > "$work/parity-suite-empty.log" 2>&1; then
      printf 'Go test executable with zero tests counted as passed coverage\n' >&2; exit 1;
    fi
    grep -q 'no tests to run' "$work/parity-suite-empty.log"
    cp "$work/last-test.go" "$source/parityjudge/main_test.go"
    cmake --build "$build" --target parity-tools > "$work/parity-suite-restored-build.log" 2>&1
    ctest --test-dir "$build" -R '^parityjudge_suite_stdin$' --output-on-failure --stop-on-failure --no-tests=error -j1 > "$work/parity-suite-restored.log"
  fi
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
