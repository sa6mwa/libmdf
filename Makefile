PREFIX ?= /usr/local

.PHONY: help deps-debug deps-release deps-cross build build-debug build-release install test test-debug test-all test-hardening asan tsan msan fuzz fuzz-smoke fuzz-long parity parity-quick parity-full parity-ansi parity-ansi-quick parity-html parity-html-quick parity-stream parity-stream-quick parity-ansi-stream parity-ansi-stream-quick parity-html-stream parity-lua lua-env lua-rock lua-test release-lua-artifacts verify-lua-artifacts package package-source package-source-smoke package-checksums package-verify verify-release-privacy verify-release-archives release-matrix finalize-slice prerelease prerelease-hardening release print-release-version format clean clean-dist cross-build test-install-tree example-smoke-local

help:
	@printf '%s\n' 'libmdf lifecycle targets:'
	@printf '%s\n' '  make deps-debug            Report debug dependency acquisition status'
	@printf '%s\n' '  make deps-release          Report release dependency acquisition status'
	@printf '%s\n' '  make deps-cross            Report cross dependency acquisition status'
	@printf '%s\n' '  make build                  Build static cmdf for local install'
	@printf '%s\n' '  make build-debug            Configure and build debug preset'
	@printf '%s\n' '  make build-release          Configure and build host release preset'
	@printf '%s\n' '  make install                Install built cmdf to DESTDIR/PREFIX/bin/cmdf'
	@printf '%s\n' '  make test                   Run debug tests'
	@printf '%s\n' '  make test-all               Run bounded local gate: tests, ASan, fuzz smoke, quick parity'
	@printf '%s\n' '  make test-hardening         Run tests, all sanitizers, fuzz smoke, and full parity'
	@printf '%s\n' '  make parity                 Run exhaustive ANSI, HTML, streaming, and Lua parity gates'
	@printf '%s\n' '  make parity-quick           Run bounded ANSI, HTML, and streaming parity smoke'
	@printf '%s\n' '  make lua-test               Run Lua facade and cmdf.lua parity smoke tests'
	@printf '%s\n' '  make lua-env                Print Lua local development environment'
	@printf '%s\n' '  make lua-rock               Build local Lua rock'
	@printf '%s\n' '  make release-lua-artifacts  Build Lua source archive and source rock'
	@printf '%s\n' '  make verify-lua-artifacts   Verify Lua release artifacts'
	@printf '%s\n' '  make asan                   Run AddressSanitizer + UBSan tests'
	@printf '%s\n' '  make tsan                   Run ThreadSanitizer tests'
	@printf '%s\n' '  make msan                   Run MemorySanitizer tests'
	@printf '%s\n' '  make fuzz-smoke             Run bounded libFuzzer smoke'
	@printf '%s\n' '  make fuzz                   Run standard bounded libFuzzer job'
	@printf '%s\n' '  make fuzz-long              Run longer bounded libFuzzer job'
	@printf '%s\n' '  make package                Build release SDK archives'
	@printf '%s\n' '  make package-source         Build source archive'
	@printf '%s\n' '  make package-source-smoke   Build and test the source archive'
	@printf '%s\n' '  make package-checksums      Generate release checksum manifest'
	@printf '%s\n' '  make package-verify         Verify release SDK archives'
	@printf '%s\n' '  make verify-release-privacy Verify release artifacts for local path leaks'
	@printf '%s\n' '  make verify-release-archives Verify release archive checks'
	@printf '%s\n' '  make release-matrix         Build all configured release targets'
	@printf '%s\n' '  make finalize-slice         Run format and local test gate'
	@printf '%s\n' '  make prerelease             Run deterministic prerelease gate, including full hardening'
	@printf '%s\n' '  make prerelease-hardening   Run prerelease plus release matrix'
	@printf '%s\n' '  make release                Run full local release gate'
	@printf '%s\n' '  make print-release-version  Print packaging version'
	@printf '%s\n' '  make format                 Format project-owned C sources'
	@printf '%s\n' '  make cross-build            Build configured cross targets'
	@printf '%s\n' '  make test-install-tree      Build and verify install tree packages'
	@printf '%s\n' '  make example-smoke-local    Run local example smoke'
	@printf '%s\n' '  make clean                  Remove generated build, dist, and cache state'
	@printf '%s\n' '  make clean-dist             Remove generated dist artifacts'

deps-debug deps-release deps-cross:
	@printf '%s\n' 'libmdf has no c.pkt.systems dependency bundle to acquire.'

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

test-all: test asan fuzz-smoke parity-quick

test-hardening: test asan tsan msan fuzz-smoke parity

asan:
	@scripts/test.sh asan

tsan:
	@scripts/test.sh tsan

msan:
	@scripts/test.sh msan

fuzz:
	@scripts/fuzz.sh standard

fuzz-smoke:
	@scripts/fuzz.sh smoke

fuzz-long:
	@scripts/fuzz.sh long

parity: parity-ansi parity-html parity-stream parity-lua

parity-full: parity

parity-quick: parity-ansi-quick parity-html-quick parity-stream-quick

parity-ansi:
	@scripts/parity.sh ansi

parity-ansi-quick:
	@LIBMDF_PARITY_PROFILE=quick scripts/parity.sh ansi

parity-html:
	@scripts/parity.sh html

parity-html-quick:
	@LIBMDF_PARITY_PROFILE=quick scripts/parity.sh html

parity-stream: parity-ansi-stream

parity-stream-quick: parity-ansi-stream-quick

parity-ansi-stream:
	@scripts/stream_parity.sh ansi

parity-ansi-stream-quick:
	@LIBMDF_STREAM_PARITY_PROFILE=quick scripts/stream_parity.sh ansi

parity-html-stream:
	@printf '%s\n' 'html streaming parity is not a supported contract' >&2
	@exit 2

parity-lua: lua-test

lua-env:
	@printf 'LUA_PATH=%s/lua/?.lua;%s\n' "$$(pwd)" "$${LUA_PATH:-;;}"

lua-rock:
	@scripts/build_lua_rock.sh

lua-test:
	@scripts/lua_test.sh

release-lua-artifacts:
	@scripts/release_lua_artifacts.sh

verify-lua-artifacts:
	@scripts/verify_lua_artifacts.sh

package:
	@scripts/package.sh

package-source:
	@scripts/package_source.sh

package-source-smoke: package-source
	@scripts/test_release_from_source.sh

package-checksums:
	@scripts/package_checksums.sh

package-verify:
	@scripts/package-verify.sh

verify-release-privacy:
	@scripts/verify_release_privacy.sh

verify-release-archives: package-verify

release-matrix:
	@scripts/cross_build.sh
	@scripts/package.sh
	@scripts/release_lua_artifacts.sh
	@scripts/package-verify.sh

finalize-slice: format test

prerelease: format test-hardening package-source-smoke

prerelease-hardening: prerelease release-matrix

release: clean prerelease-hardening

print-release-version:
	@scripts/release_version.sh

format:
	@scripts/format.sh

test-install-tree:
	@scripts/package.sh
	@scripts/package-verify.sh

example-smoke-local: build-debug
	@build/debug/example_basic >/dev/null

clean:
	@scripts/clean.sh

clean-dist:
	@rm -rf dist
