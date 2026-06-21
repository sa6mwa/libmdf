PREFIX ?= /usr/local

.PHONY: help deps-debug deps-release deps-cross build build-debug build-release install test test-debug test-all asan tsan msan fuzz fuzz-smoke fuzz-long parity parity-quick parity-full parity-ansi parity-ansi-quick parity-html parity-html-quick parity-tokens parity-stream parity-stream-quick parity-ansi-stream parity-ansi-stream-quick parity-html-stream parity-lua lua-env lua-rock lua-test release-lua-artifacts verify-lua-artifacts package package-source package-source-smoke package-checksums package-verify verify-release-privacy verify-release-archives release-matrix finalize-slice prerelease prerelease-hardening release print-release-version format clean clean-dist cross-build test-install-tree example-smoke-local

help:
	@printf '%s\n' 'libmdf lifecycle targets:'
	@printf '%s\n' '  make build                  Build static cmdf for local install'
	@printf '%s\n' '  make build-debug            Configure and build debug preset'
	@printf '%s\n' '  make install                Install built cmdf to DESTDIR/PREFIX/bin/cmdf'
	@printf '%s\n' '  make test                   Run debug tests'
	@printf '%s\n' '  make parity                 Run exhaustive ANSI, HTML, streaming, and Lua parity gates'
	@printf '%s\n' '  make parity-quick           Run bounded ANSI, HTML, and streaming parity smoke'
	@printf '%s\n' '  make lua-test               Run Lua facade and cmdf.lua parity smoke tests'
	@printf '%s\n' '  make release-lua-artifacts  Build Lua source archive and source rock'
	@printf '%s\n' '  make asan                   Run AddressSanitizer + UBSan tests'
	@printf '%s\n' '  make tsan                   Run ThreadSanitizer tests'
	@printf '%s\n' '  make msan                   Run MemorySanitizer tests'
	@printf '%s\n' '  make fuzz-smoke             Run bounded libFuzzer smoke'
	@printf '%s\n' '  make fuzz                   Run standard bounded libFuzzer job'
	@printf '%s\n' '  make package                Build release SDK archives'
	@printf '%s\n' '  make package-verify         Verify release SDK archives'
	@printf '%s\n' '  make verify-release-privacy Verify release artifacts for local path leaks'
	@printf '%s\n' '  make release-matrix         Build all configured release targets'
	@printf '%s\n' '  make release                Run full local release gate'
	@printf '%s\n' '  make clean                  Remove generated build, dist, and cache state'

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

test-all: test asan tsan msan fuzz-smoke parity

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

parity-quick: parity-ansi-quick parity-html-quick parity-stream-quick parity-lua

parity-ansi:
	@scripts/parity.sh ansi

parity-ansi-quick:
	@LIBMDF_PARITY_PROFILE=quick scripts/parity.sh ansi

parity-html:
	@scripts/parity.sh html

parity-html-quick:
	@LIBMDF_PARITY_PROFILE=quick scripts/parity.sh html

parity-tokens:
	@scripts/parity.sh tokens

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
	@printf '%s\n' 'source archive produced'

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

prerelease: finalize-slice asan tsan msan fuzz-smoke parity package-source-smoke

prerelease-hardening: prerelease release-matrix

release: clean prerelease-hardening

print-release-version:
	@scripts/release_version.sh

format:
	@cmake -E true

test-install-tree:
	@scripts/package.sh
	@scripts/package-verify.sh

example-smoke-local: build-debug
	@build/debug/example_basic >/dev/null

clean:
	@scripts/clean.sh

clean-dist:
	@rm -rf dist
