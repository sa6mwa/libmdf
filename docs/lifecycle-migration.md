# Native lifecycle cutover

The current `pkt-systems-cmake-lifecycle` skill is the governing contract. The
migration preserves the C library, CLI, Lua facade, seven shipment targets, public
ABI and existing rendering tests. Publication remains an engineer operation.

| Removed behavior | Current owner | Observable proof |
| --- | --- | --- |
| Configure-time tool acquisition and duplicated resolver variants | Exact vendored helper collection; explicit preparation, read-only discovery | Resolver fixtures, real native/fuzz/cross producers |
| Shell interpretation of profiles and deletion/reconfiguration of active caches | Native CMake presets; fixtures configure isolated directories | `configure_presets`, option/install matrix |
| Hash-driven Go builds that include C implementation files | CMake producers link shared libmdf; a local public header participates in cgo cache identity | Go option/suite tests and full output/emission parity |
| Python test and benchmark controllers | Compiled terminal-policy and benchmark tests; Lua assertions; CTest; small Bash/CMake entrypoints | Real pipe/file/PTY behavior, seven Lua workflows, benchmark failure and threshold tests |
| Separate equivalent Debug/Valgrind and repeated Lua producers | Shared native Debug producer and installed-SDK Lua graph | Memory checks, Lua runtime maps and module/CLI parity |
| Active-worktree temporary release tags | Read-only final preflight; tag variations only in disposable fixtures | Version/source and release sequencing tests |
| Packaging reconfiguration, forced relinks and partial final manifests | CMake install components and archive dependencies; explicit binary/full inventories | Determinism, warm reuse, selected ownership, missing-artifact failures |
| A separate source-smoke Release producer and native-only packaging override | All targets build from the checkout through their normal Release presets | Canonical native package/reuse fixture, source archive content/mode checks and final sequencing fixture |
| Parent-PID cancellation wrappers and broad best-effort inspection | Foreground helpers; caller owns whole-job cancellation; target-correct checks fail when unavailable | Failure-stop fixtures and final extracted byte checks |

Successful suites use small CMake outputs with declared prerequisites. These are
suite-scoped local results, not a second scheduler or per-case evidence database.
An unchanged producer preserves them; relevant binaries, data, scripts and
configuration changes invalidate them. Force `make test` or `make check-clean`
when the ordinary timestamp/stable-input assumptions do not apply.

A checkout using the previous tool cache may need one explicit
`cmake --fresh --preset <preset>` when changing compiler collection paths. Normal
entrypoints do not erase caches or worktrees. `make clean` removes owned generated
state, including hidden entries and local tools, while declared global verified
archives and tool collections remain untouched. A checkout never borrows another
checkout's generated state to bypass that cleanup.

Implementation, testing and review stay on one development branch. For an
authorized release, preserve it and squash the verified change onto the resolved
local default branch before amendment/signing. Signing, tagging and the final gate
use that default-branch commit; no intermediate release branch is created.

Final release preflight requires HEAD attached to that branch and verifies the
final amended commit's signature and its exact lightweight tag before full
`make clean` or any producer. Artifact/staging redirection
cannot bypass the owned cleanup paths. Remote release refs remain unchanged until
the complete local final gate passes; preparation does not authorize publication.
Cache overrides use this checkout's owned `build/` or `.cache/` paths or the
declared global cpkt cache. Only that global cache and installed workstation
prerequisites survive full cleanup. Documentation/skill-only updates need their
normal verification and authorized commit/signing/push policy, without a release.
