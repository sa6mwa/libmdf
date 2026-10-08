#!/usr/bin/env bash
# Observe native suite reuse, input invalidation, failure and empty selection.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-check-reuse"
rm -rf -- "$work"
mkdir -p "$work/source/scripts" "$work/source/testdata"
printf pass > "$work/source/testdata/input"
touch "$work/source/Makefile"
printf '{"version":5,"configurePresets":[]}\n' > "$work/source/CMakePresets.json"
cat > "$work/source/scripts/probe.sh" <<'PROBE'
#!/usr/bin/env bash
set -euo pipefail
source=$1 build=$2
printf 'run\n' >> "$build/runs"
[[ $(cat "$source/testdata/input") != fail ]]
PROBE
cat > "$work/source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
project(check_reuse NONE)
enable_testing()
set(LIBMDF_BUILD_TESTS ON)
set(LIBMDF_BUILD_HOST_TESTS ON)
add_executable(test_public_api IMPORTED)
add_executable(test_contracts IMPORTED)
set_target_properties(test_public_api test_contracts PROPERTIES IMPORTED_LOCATION /bin/true)
option(ENABLE_PROBE "Register the fixture check" ON)
if(ENABLE_PROBE)
  add_test(NAME probe COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/probe.sh" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
endif()
include("${CHECKS_MODULE}")
CMAKE
cmake -S "$work/source" -B "$work/build" -G Ninja -DCHECKS_MODULE="$root/cmake/Checks.cmake" > "$work/configure.log"
cmake --build "$work/build" --target check-native > "$work/first.log"
[[ $(wc -l < "$work/build/runs") == 1 ]]
cmake --build "$work/build" --target check-native > "$work/warm.log"
[[ $(wc -l < "$work/build/runs") == 1 ]]
# Ordinary edits create newer timestamps; no hash service or forced timestamp repair.
sleep 1
printf changed > "$work/source/testdata/input"
cmake --build "$work/build" --target check-native > "$work/changed.log"
[[ $(wc -l < "$work/build/runs") == 2 ]]
sleep 1
printf fail > "$work/source/testdata/input"
if cmake --build "$work/build" --target check-native > "$work/failed.log" 2>&1; then exit 1; fi
test ! -f "$work/build/checks/native.pass"
printf pass > "$work/source/testdata/input"
cmake -S "$work/source" -B "$work/build" -DENABLE_PROBE=OFF > "$work/empty-configure.log"
if cmake --build "$work/build" --target check-native > "$work/empty.log" 2>&1; then exit 1; fi
grep -q 'No tests were found' "$work/empty.log"
test ! -f "$work/build/checks/native.pass"
