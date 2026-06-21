# Go Reference Audit

This tracks the C implementation against `pkt.systems/mdf`. The default rule is:
mirror Go behavior and structure unless a C-specific implementation is justified
by API shape, allocation control, or measurable performance. Any exception must
still pass output and streaming parity.

Status values:

- `mirror`: C should follow the Go implementation directly.
- `exception`: C may differ, with the reason documented here.
- `cleanup`: likely local/legacy behavior that needs removal or replacement.
- `unknown`: not audited yet.

## Subsystem Map

| C surface | Go reference | Status | Notes |
| --- | --- | --- | --- |
| Public handle/options API | `render_options.go`, `stream_sink.go`, `doc.go` | exception | C API is receiver/handle driven and exposes allocator/emission-buffer control. Behavior defaults must still mirror Go. |
| Allocator and emission buffer | `alloc_test.go`, `trace.go`, `stream.go` write path | exception | C owns explicit bounded emission buffers for near-zero allocation. Exception is valid only if one decision maps to one sink write and one trace event. |
| CLI defaults/options | `cmd/mdf/main.go`, `stream_sim.go` | mirror | `cmdf` flags, defaults, OSC8 auto, duration parsing, and simulator chunk timing should follow Go. |
| Frontmatter filter | `frontmatter_filter.go`, `frontmatter_filter_test.go` | mirror | C now has the Go-style opening-prefix probe; delimiter-candidate buffering should stay byte-for-byte behavioral parity. |
| Live parser/block decisions | `stream_live.go`, `live_parser.go`, `stream_minimum_emission_test.go` | mirror | One decision, one emission, no line buffering except required table modes/thematic/frontmatter candidates. |
| Table parser/lookahead | `table_parse.go`, `table.go`, `table_parse_test.go` | mirror | Edge-pipe tables only. No-edge tables stream as text. Full/row table buffering should match final output and configured buffering semantics. |
| ANSI renderer word wrapping | `stream.go`, `wrap_utils.go`, `wordwrap_regression_test.go` | mirror | Broad ANSI output and stream trace parity pass against Go SDK. Keep C-specific branches only when they preserve those traces or cover table buffering. |
| ANSI inline parsing/emphasis/link handling | `live_parser.go`, `stream.go`, `tokens.go` | mirror | Reported heading, list, bold quote, link fallback, inline-code, and autolink drift is covered by stream parity and focused tests. |
| OSC8 links | `stream.go`, `osc8_support.go`, `cmd/mdf/main.go` | mirror | CLI default auto, reset/order grouping, surrounding style reopen, autolinks, and ANSI table final output now match installed `mdf` under an OSC8-capable environment. |
| HTML renderer | `html/stream.go`, `html/render.go`, `html/config.go` | mirror | Output parity matters, including selected built-in themes. HTML streaming trace parity is not a contract. Streaming should still emit as soon as decisions are made. |
| Font embedding/license packaging | `html/embedded_fonts.go`, release packaging | mirror | HTML default is JetBrains Mono. Ship `OFL.txt` beside `LICENSE` in distribution share path. |
| Write trace | `trace.go`, `trace_test.go` | mirror | Trace events must describe renderer emissions, not raw sink writes, and must match sink writes one-to-one in C. |
| Built-in themes | `theme.go`, `internal/palette/palette.go` | mirror | C resolves built-in themes by name per instance and applies semantic styles to ANSI and HTML. Custom palette construction is not exposed in the current C API. |
| Parity judge | Go tests plus `parityjudge` | mirror | Use Go SDK through parityjudge/CGO for fast feedback. Do not rely on `strace` as the parity oracle. |

## Immediate Audit Order

- [x] CLI/simulator: confirm `cmdf` input chunking and delay semantics match `StreamSimulate`.
- [x] Frontmatter/parser gates: copy Go prefix-candidate tests into C equivalents.
- [x] ANSI wrapping: replace C-only wrapping branches with Go word/atom/code segment rules where parity required it.
- [x] OSC8: match Go auto/default detection and emission grouping for normal output and ANSI table final output.
- [x] Tables: compare C table state machine to Go edge-pipe-only behavior and remove no-edge remnants.
- [x] HTML: compare default shell/style/font output against Go HTML package.
- [x] Packaging: verify `OFL.txt` lands in the same share path as `LICENSE`.

## Exception Requirements

An exception is acceptable only when all are true:

- The C implementation has a concrete API, allocation, or performance reason.
- The final output matches Go for the covered mode.
- ANSI streaming emits the same decisions or a documented stricter/minimal stream.
- The behavior has a focused regression test.
