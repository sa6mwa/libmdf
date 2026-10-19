#!/usr/bin/env bash
# Observe native suite reuse, input invalidation, failure and empty selection.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-check-reuse"
rm -rf -- "$work"
mkdir -p "$work/source/scripts" "$work/source/testdata" "$work/source/assets/c89"
printf pass > "$work/source/testdata/input"
touch "$work/source/Makefile"
for input in README.md LICENSE libmdf-scm-1.rockspec.in assets/c89/style.css; do printf 'valid\n' > "$work/source/$input"; done
printf '{"version":5,"configurePresets":[]}\n' > "$work/source/CMakePresets.json"
cat > "$work/source/scripts/probe.sh" <<'PROBE'
#!/usr/bin/env bash
set -euo pipefail
source=$1 build=$2
printf 'run\n' >> "$build/runs"
test -f "$source/testdata/input"
[[ $(cat "$source/testdata/input") != fail ]]
for input in README.md LICENSE libmdf-scm-1.rockspec.in assets/c89/style.css; do grep -Fxq valid "$source/$input"; done
PROBE
cat > "$work/source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
project(check_reuse NONE)
enable_testing()
set(LIBMDF_BUILD_TESTS ON)
set(LIBMDF_BUILD_HOST_TESTS ON)
add_executable(test_public_api IMPORTED)
add_executable(test_contracts IMPORTED)
add_executable(test_c89 IMPORTED)
set_target_properties(test_public_api test_contracts test_c89 PROPERTIES IMPORTED_LOCATION /bin/true)
option(ENABLE_PROBE "Register the fixture check" ON)
if(ENABLE_PROBE)
  add_test(NAME probe COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/probe.sh" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
endif()
include("${CHECKS_MODULE}")
CMAKE
cmake -S "$work/source" -B "$work/build" -G Ninja -DCHECKS_MODULE="$root/cmake/Checks.cmake" > "$work/configure.log"
cmake --build "$work/build" --target check-native > "$work/first.log"
[[ $(wc -l < "$work/build/runs") == 1 ]]
mkdir -p "$work/source/build/generated"
ln -s absent "$work/source/build/generated/README.md"
cmake -S "$work/source" -B "$work/build" > "$work/warm-configure.log"
cmake --build "$work/build" --target check-native > "$work/warm.log"
[[ $(wc -l < "$work/build/runs") == 1 ]]
# Ordinary edits create newer timestamps; no hash service or forced timestamp repair.
sleep 1
printf changed > "$work/source/testdata/input"
cmake --build "$work/build" --target check-native > "$work/changed.log"
[[ $(wc -l < "$work/build/runs") == 2 ]]
rm "$work/source/testdata/input"
if cmake --build "$work/build" --target check-native > "$work/deleted.log" 2>&1; then
  printf 'Deleted verification input left a stale native success record\n' >&2; exit 1;
fi
test ! -f "$work/build/checks/native.pass"
# Root packaging inputs participate in the native fixture suite as well.
printf changed > "$work/source/testdata/input"
cmake -S "$work/source" -B "$work/build" -DENABLE_PROBE=ON > "$work/root-configure.log"
cmake --build "$work/build" --target check-native > "$work/root-first.log"
for input in README.md LICENSE libmdf-scm-1.rockspec.in assets/c89/style.css; do
  sleep 1
  printf 'invalid\n' > "$work/source/$input"
  if cmake --build "$work/build" --target check-native > "$work/root-changed.log" 2>&1; then
    printf 'Changed root fixture input reused native success: %s\n' "$input" >&2; exit 1;
  fi
  test ! -f "$work/build/checks/native.pass"
  printf 'valid\n' > "$work/source/$input"
  cmake --build "$work/build" --target check-native > "$work/root-restored.log"
done
printf '\nadd_test(NAME introduced_failure COMMAND /bin/false)\n' >> "$work/source/CMakeLists.txt"
if cmake --build "$work/build" --target check-native > "$work/definitions.log" 2>&1; then
  printf 'Root CMake test definition changes left a stale success record\n' >&2; exit 1;
fi
grep -q introduced_failure "$work/definitions.log"
test ! -f "$work/build/checks/native.pass"
sed -i '/introduced_failure/d' "$work/source/CMakeLists.txt"
sleep 1
printf fail > "$work/source/testdata/input"
if cmake --build "$work/build" --target check-native > "$work/failed.log" 2>&1; then exit 1; fi
test ! -f "$work/build/checks/native.pass"
printf pass > "$work/source/testdata/input"
cmake -S "$work/source" -B "$work/build" -DENABLE_PROBE=OFF > "$work/empty-configure.log"
if cmake --build "$work/build" --target check-native > "$work/empty.log" 2>&1; then exit 1; fi
grep -q 'No tests were found' "$work/empty.log"
test ! -f "$work/build/checks/native.pass"
