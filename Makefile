PREFIX ?= /usr/local
CMAKE_BUILD_PARALLEL_LEVEL ?= 2
export CMAKE_BUILD_PARALLEL_LEVEL
.NOTPARALLEL:

.PHONY: help tools-native tools-cross build build-debug build-release install test test-debug check-native check-clean test-all test-hardening valgrind fuzz fuzz-smoke fuzz-long parity parity-quick parity-full parity-ansi parity-ansi-quick parity-html parity-html-quick parity-stream parity-stream-quick parity-ansi-stream parity-ansi-stream-quick parity-lua lua-test lua-rock lua-env benchmark benchmark-cmdf bench-check golden-update golden-test cmdf-golden-update cmdf-golden-test format format-check shell-check package package-source package-source-smoke package-checksums package-verify verify-release-privacy verify-release-archives release-lua-artifacts verify-lua-artifacts cross-build release-matrix prerelease prerelease-hardening release lifecycle-version-contract print-release-version finalize-slice test-install-tree example-smoke-local clean clean-dist

help:
	@printf '%s\n' \
	  'libmdf native lifecycle (all operations are serialized):' \
	  '  make tools-native            Explicitly prepare pinned GNU/AFL++ tooling' \
	  '  make tools-cross             Explicitly prepare the declared target tools' \
	  '  make build                   Build the static native musl CLI' \
	  '  make build-debug             Build native tests, Go parity tools and Lua facade' \
	  '  make build-release           Build native Release libraries and static CLI' \
	  '  make install                 Install static cmdf to DESTDIR/PREFIX/bin' \
	  '  make test                    Force the native CTest suite' \
	  '  make check-native            Reuse unchanged native suite success' \
	  '  make check-clean             Invalidate local suite success records' \
	  '  make test-all                Native checks, memory, fuzz and quick parity' \
	  '  make test-hardening          Native checks, memory, fuzz and full parity' \
	  '  make valgrind                Check the existing native Debug producer' \
	  '  make fuzz-smoke              Check AFL failure gates and native fuzz smoke' \
	  '  make fuzz / fuzz-long        Run fresh bounded fuzz jobs' \
	  '  make parity / parity-quick   Full / bounded Go parity CTest suites' \
	  '  make lua-test / lua-rock     Verify the installed-SDK facade / build a local rock' \
	  '  make benchmark               Measure C/Lua API ANSI and HTML medians' \
	  '  make benchmark-cmdf          Measure C/Lua CLI ANSI, HTML and deck medians' \
	  '  make bench-check             Check the declared benchmark baseline' \
	  '  make golden-test             Verify library output snapshots' \
	  '  make format / format-check   Apply / check C formatting' \
	  '  make shell-check             Validate lifecycle shell syntax' \
	  '  make package                 Build/reuse selected binary archive producers' \
	  '  make package-verify          Verify the binary rehearsal through CTest' \
	  '  make package-source-smoke    Reconstruct and test the committed source archive' \
	  '  make prerelease              Incremental binary/Lua preparation; source smoke is explicit' \
	  '  make release                 Require a clean exact tag, clean once and verify final bytes' \
	  '  make clean                   Remove owned build/dist/extraction state; preserve shared caches'

tools-native:
	@scripts/cpkt-toolchains.sh ensure x86_64-linux-gnu
	@scripts/cpkt-aflpp.sh ensure

tools-cross:
	@scripts/prepare_targets.sh

build:
	@scripts/build_cmdf_static.sh

build-debug:
	@scripts/build.sh debug

build-release:
	@scripts/build.sh x86_64-linux-gnu-release

install:
	@scripts/install_cmdf.sh

test test-debug:
	@scripts/test.sh debug

check-native:
	@scripts/build.sh debug --target check-native

check-clean:
	@scripts/invalidate_checks.sh

test-all: check-native valgrind fuzz-smoke parity-quick

test-hardening: check-native valgrind fuzz-smoke parity

valgrind:
	@scripts/build.sh debug --target check-memory

fuzz-smoke:
	@scripts/fuzz.sh smoke

fuzz:
	@scripts/fuzz.sh standard

fuzz-long:
	@scripts/fuzz.sh long

parity parity-full: parity-ansi parity-html parity-stream parity-lua
parity-quick: parity-ansi-quick parity-html-quick parity-stream-quick

parity-ansi:
	@scripts/build.sh debug --target check-parity-ansi

parity-html:
	@scripts/build.sh debug --target check-parity-html

parity-stream parity-ansi-stream:
	@scripts/build.sh debug --target check-parity-stream

parity-ansi-quick:
	@scripts/build.sh debug --target check-quick-parity-ansi

parity-html-quick:
	@scripts/build.sh debug --target check-quick-parity-html

parity-stream-quick parity-ansi-stream-quick:
	@scripts/build.sh debug --target check-quick-parity-stream

parity-lua: check-native

lua-test:
	@scripts/lua_test.sh

lua-rock:
	@scripts/build_lua_rock.sh

lua-env:
	@printf 'LUA=%s/build/debug/lua-runtime/bin/lua\nLUA_PATH=%s/build/debug/lua-tree/share/lua/5.5/?.lua;;\nLUA_CPATH=%s/build/debug/lua-tree/lib/lua/5.5/?.so;;\n' "$(CURDIR)" "$(CURDIR)" "$(CURDIR)"

benchmark:
	@scripts/benchmark.sh api $(BENCH_ARGS)

benchmark-cmdf:
	@scripts/benchmark.sh cli $(BENCH_ARGS)

bench-check:
	@scripts/check_benchmark.sh $(BENCH_CHECK_ARGS)

golden-update:
	@scripts/update_libmdf_goldens.sh

golden-test: build-debug
	@ctest --test-dir build/debug -R '^libmdf_goldens$$' --output-on-failure --stop-on-failure --no-tests=error -j1

cmdf-golden-update:
	@scripts/update_cmdf_goldens.sh

cmdf-golden-test: build-debug
	@ctest --test-dir build/debug -R '^cmdf_goldens$$' --output-on-failure --stop-on-failure --no-tests=error -j1

format:
	@scripts/format.sh

format-check:
	@scripts/format.sh --check

shell-check:
	@scripts/check_shell.sh

package:
	@scripts/package.sh

package-source:
	@scripts/package_source.sh

package-source-smoke: package-source
	@scripts/test_release_from_source.sh

package-checksums:
	@scripts/package_checksums.sh complete

package-verify:
	@scripts/verify_packages.sh binary

verify-release-archives:
	@scripts/verify_packages.sh complete

verify-release-privacy:
	@scripts/verify_release_privacy.sh

release-lua-artifacts:
	@scripts/release_lua_artifacts.sh

verify-lua-artifacts:
	@scripts/verify_packages.sh lua

cross-build:
	@scripts/cross_build.sh

release-matrix: package release-lua-artifacts package-verify verify-lua-artifacts

prerelease prerelease-hardening:
	@scripts/release.sh prepare

release:
	@scripts/release.sh final

lifecycle-version-contract:
	@scripts/lifecycle_version_contract.sh

print-release-version:
	@scripts/release_version.sh

finalize-slice: format-check test-all

test-install-tree: package package-verify

example-smoke-local: build-debug
	@build/debug/example_basic >/dev/null

clean:
	@scripts/clean.sh

clean-dist:
	@scripts/clean_dist.sh
