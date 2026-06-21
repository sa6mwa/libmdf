# Parity Bug Tracker

This tracks reported drift between installed `mdf` and `cmdf`/libmdf. An item is
done only when the reproduction is fixed and covered by a repeatable check.

## Open

No open parity bugs are currently tracked.

## Done

- [x] PARITY-001: `cmdf` default OSC8 behavior is not `auto`
  - Fixed: `cmdf` initializes OSC8 from terminal auto-detection and accepts
    `--osc8 auto|on|off`.
  - Evidence: direct installed-`mdf` vs `build/debug/cmdf` output and trace
    match for a normal link under `TERM=xterm-256color VTE_VERSION=6000`.

- [x] PARITY-002: parityjudge masks CLI OSC8 drift
  - Fixed: SDK/lib parity remains the main fast oracle, and direct CLI smoke was
    added to the verification process for default OSC8 behavior.
  - Evidence: direct installed-CLI final output parity passes across
    `testdata/` at width 80/chunk 1 with OSC8 auto.

- [x] PARITY-003: `cmdf --simulate-delay` accepts/rejects the wrong syntax
  - Fixed: `cmdf` accepts Go duration units such as `50ms` and rejects unitless
    fractional values such as `0.05`.
  - Evidence: `tests/test_cmdf.c` covers valid duration syntax, implicit
    simulation, and invalid unitless fractional delay.

- [x] PARITY-004: headers are reported as fake-streamed / emitted as one chunk
  - Fixed: ATX heading text emits as word-sized decisions instead of waiting for
    the line ending.
  - Evidence: `tests/test_api.c` asserts sink writes equal trace emissions and
    that `# The Outcome-Based Agile Framework` emits before newline; broad
    `scripts/stream_parity.sh ansi` passes.

- [x] PARITY-005: list items in `OBAF.md` are reported as fake-streamed
  - Fixed: list/emphasis paths no longer emit reported bold phrases as one fake
    stream chunk.
  - Evidence: `tests/test_cmdf.c` covers the reported
    `"Governance exists to support autonomy"` phrase as word emissions; broad
    `scripts/stream_parity.sh ansi` passes.

- [x] PARITY-006: OSC8 write grouping/reset order differs
  - Fixed: OSC8 links now open before resets where Go does, close before style
    resets, and reopen surrounding inline style through the pending-style path.
  - Evidence: direct installed-`mdf` vs `build/debug/cmdf` output and trace
    match for `testdata/mdtest/README.md`, `testdata/mdtest/TEST.md`, and
    `testdata/parity/autolinks.md` under OSC8 auto.

- [x] PARITY-008: previous parity claims were based on insufficient evidence
  - Fixed: verification now distinguishes SDK/lib parity, direct installed-CLI
    output parity, and table trace exceptions.
  - Evidence: this tracker records the exact oracle for each parity claim.

- [x] PARITY-009: old frame-mode assumptions remain in tooling/docs/tests
  - Fixed: no frame simulator or frame parity path remains.
  - Evidence: repository search for `--frames`, `simulate_frames`, frame parity,
    and `cmdf -F` is clean except this historical tracker entry.

- [x] PARITY-010: parity checks do not model flush/visible streaming behavior
  - Fixed: libmdf has a native write-trace callback and tests assert trace
    events match real sink writes one-to-one.
  - Evidence: `tests/test_api.c` compares sink write chunks and trace chunks
    from the same render.

- [x] PARITY-011: broad ANSI stream parity failed under v0.8.0 emission trace
  - Fixed: current C trace output matches Go SDK trace output across the broad
    ANSI stream parity suite.
  - Evidence: `scripts/stream_parity.sh ansi` passes.

- [x] PARITY-012: table trace chunking is not exact under OSC8
  - Decision: tables are the documented buffering exception. Exact per-chunk
    table trace parity is not a contract; final output parity and configured
    table buffering semantics are the contract.
  - Evidence: direct installed-`mdf` vs `build/debug/cmdf` final output parity
    passes across `testdata/` at width 80/chunk 1 with OSC8 auto; table trace
    exactness remains intentionally outside `stream_parity.sh`.

- [x] PARITY-007: theme/palette/default style surface audit
  - Fixed: libmdf/cmdf now resolve a per-instance built-in `theme_name` from
    the same built-in names as Go and apply selected themes to ANSI and HTML
    semantic styles.
  - Scope decision: C exposes built-in theme selection by name. A custom
    palette-construction API is not part of the current public C surface.
  - Evidence: `tests/test_api.c` covers selected ANSI/HTML theme output;
    `tests/test_public_api.c` covers invalid theme rejection; direct
    installed-`mdf` vs `build/debug/cmdf` selected-theme ANSI and HTML fixtures
    match.
