# Lifecycle Migration

libmdf already provides the pkt.systems C/CMake lifecycle surfaces. This ledger
records the final convergence changes made for the current lifecycle authority.

| Previous behavior | Lifecycle surface | Preserved behavior | Verification |
| --- | --- | --- | --- |
| `prerelease` rebuilt and smoke-tested the source archive. | Release sequencing | Normal prerelease still builds, tests, hardens, packages, verifies binary SDKs, and verifies Lua artifacts. Source reconstruction now occurs only after the binary matrix in clean `make release`. | `release_lifecycle` |
| Version-contract testing used an isolated temporary repository. | Exact lightweight-tag versioning | `make lifecycle-version-contract` now creates and removes the reserved lightweight tag on the active candidate worktree, while exact real tags remain authoritative. | `release_lifecycle`, `version_source_archive` |
| Valgrind shared the debug preset. | CMake preset surface | Valgrind has its own native Bootlin preset while retaining the existing debug test configuration. | `configure_profiles` |
| CMake required 3.20. | Tooling baseline | The project now requires the lifecycle minimum CMake 3.24. | normal configure and CMake consumer checks |
| The shared object leaked renderer internals. | ABI/export boundary | Linux and Darwin linker allowlists expose only installed public C functions. | `mdf_shared_exports` (Linux); Darwin release package verification |
