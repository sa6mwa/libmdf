# libmdf

`libmdf` is a C89/POSIX Markdown renderer for ANSI terminal output and HTML.
It is a C port of the rendering surfaces from [pkt.systems/mdf](https://github.com/sa6mwa/mdf),
with parity checks against the Go implementation.

The project ships:

- `libmdf`, a receiver-style C API.
- `cmdf`, a CLI renderer.
- `cmdf.lua`, a Lua CLI facade packaged with the CLI artifacts.
- Lua bindings for Lua 5.5.

PDF generation is intentionally out of scope.

## Install

Archives are split by purpose.

Source archives are named:

```text
libmdf-<version>.tar.gz
```

They contain the release source tree for rebuilding the C library, CLI, tests,
Lua bindings, and release artifacts.

Library SDK archives are named:

```text
libmdf-<version>-<target>.tar.gz
```

They contain:

```text
include/libmdf.h
include/libmdf/mdf.h
lib/libmdf.a
lib/libmdf.so* or lib/libmdf.dylib*
lib/cmake/libmdf/
lib/pkgconfig/libmdf.pc
share/libmdf/package-metadata.txt
share/doc/libmdf/LICENSE
share/doc/libmdf/README.md
```

CLI archives are named:

```text
cmdf-<version>-<target>.tar.gz
```

They contain:

```text
bin/cmdf
bin/cmdf.lua
share/doc/cmdf/LICENSE
share/doc/cmdf/README.md
share/doc/cmdf/OFL.txt
```

Both CLI archives and library SDK archives include JetBrains Mono and its
`OFL.txt` attribution. The font is embedded by libmdf for self-contained HTML
and deck output.

Lua release artifacts are named:

```text
libmdf-lua-<version>.tar.gz
libmdf-<version>-1.rockspec
libmdf-<version>-1.src.rock
```

They provide the Lua 5.5 facade and native binding package source. The source
rock is built against an extracted SDK archive during verification.

Prebuilt targets:

```text
x86_64-linux-gnu
x86_64-linux-musl
aarch64-linux-gnu
aarch64-linux-musl
armhf-linux-gnu
armhf-linux-musl
arm64-apple-darwin
```

Linux `cmdf` binaries are static. The library SDK archives ship both static
and shared libraries.

## CMake

After extracting an SDK archive, point CMake at the archive prefix:

```cmake
find_package(libmdf CONFIG REQUIRED)

add_executable(app app.c)
target_link_libraries(app PRIVATE libmdf::mdf_static)
```

The package exports `libmdf::mdf_static` and `libmdf::mdf_shared` when both
library variants are present.

When building from source, the main CMake options are:

```text
LIBMDF_BUILD_STATIC=ON|OFF
LIBMDF_BUILD_SHARED=ON|OFF
LIBMDF_BUILD_BINARY=ON|OFF
LIBMDF_BUILD_TESTS=ON|OFF
LIBMDF_BUILD_FUZZ=ON|OFF
LIBMDF_INSTALL_BINARY=ON|OFF
LIBMDF_CMDF_STATIC_RUNTIME=ON|OFF
```

The project presets and `Makefile` targets are the preferred local entry points
for development and release builds.

## pkg-config

The SDK also ships relocatable pkg-config metadata:

```sh
cc app.c $(pkg-config --cflags --libs libmdf)
```

## C API

Include either header:

```c
#include <libmdf.h>
/* or */
#include <libmdf/mdf.h>
```

The public handle is `mdf`. Construct it with `mdf_create`, then use receiver
methods on the handle.

```c
#include <libmdf/mdf.h>

#include <stdio.h>

int main(void)
{
    mdf_options opts;
    mdf *renderer;
    char *out;

    mdf_options_init(&opts);
    opts.boring = 1;

    if (mdf_create(MDF_FORMAT_ANSI, &opts, &renderer) != MDF_OK) {
        return 1;
    }
    if (renderer->render_cstr(renderer, "# hello\n\nworld\n", &out) != MDF_OK) {
        renderer->destroy(renderer);
        return 1;
    }

    fputs(out, stdout);
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    return 0;
}
```

For producer-to-consumer rendering, use `renderer->render` with `mdf_source`
and `mdf_sink` callbacks. The source callback returns bytes read, returns `0`
for EOF, and sets `*err` on read failure. The sink callback returns `0` on
success and nonzero on write failure.

Important options:

- `width`: ANSI wrap width. Default is `80`; `cmdf` uses terminal width for
  ANSI output unless `--width` is set. Effective ANSI content width after
  margins must be at least 3 columns.
- `margin_left`, `margin_right`: ANSI-only margins. Left margin spaces are
  emitted lazily only on non-empty lines. Right margin reduces available wrap
  width.
- `boring`: disable ANSI styling.
- `osc8`: enable OSC8 hyperlink output for ANSI.
- `theme_name`: theme name. `default` is used when unset.
- `html_content_width_ch`: HTML document content width in `ch`. Default is
  `96`.
- `html_font`: optional user-supplied HTML font data. HTML and deck output use
  the built-in JetBrains Mono regular and italic WOFF2 faces when this is unset.
- `deck_transition`: `MDF_DECK_TRANSITION_FADE`,
  `MDF_DECK_TRANSITION_CROSS`, or `MDF_DECK_TRANSITION_HARD`. Default is
  `MDF_DECK_TRANSITION_FADE`.
- `slide_numbers`: show deck slide numbers after the first slide.
- `deck_center_front_text`: center-align paragraph text on the first deck
  slide.
- `table_buffer_mode`: `MDF_TABLE_BUFFER_FULL` or `MDF_TABLE_BUFFER_ROW`.
- `table_wire_mode`: `MDF_TABLE_WIRE_LINE`, `MDF_TABLE_WIRE_ASCII`, or
  `MDF_TABLE_WIRE_SPACE`.
- `write_trace`: callback for ANSI decision-emission tracing.
- `allocator`, `emission_buffer`, `memory`: custom memory and emission-buffer
  control.

The shared library uses SONAME ABI version `2`. Lua facade and `cmdf.lua`
changes do not require a C ABI bump; changes to installed C headers,
`mdf_options`, exported symbols, or shared-library layout determine whether the
ABI version changes.

For HTML, `mdf_set_html_title(renderer, title)` can set an optional document
title after `mdf_create` and before rendering starts. Passing `NULL` clears the
override. When no explicit title is set, libmdf auto-detects the title from an
initial ATX heading before any paragraph and otherwise falls back to `mdf`. The
probe stops as soon as the input proves there is no initial ATX title, then
continues rendering through the same source stream. This title probe is
HTML/deck-only; ANSI rendering never collects heading text for title detection.

Complete HTML documents generated by libmdf include this provenance comment
immediately after the doctype:

```html
<!-- Generated by libmdf (C) 2026 Michel Blomgren https://pkt.systems/c/libmdf -->
```

Available themes can be queried with `mdf_theme_count`, `mdf_theme_name`, and
`mdf_theme_exists`. Theme lookup is case-insensitive. The built-in set includes
`default`, `ayu-light`, `ayu-mirage`, `catppuccin-mocha`, `dracula`,
`everforest`, `github-dark`, `github-light`, `gruvbox`, `kanagawa`, `nord`,
`rose-pine`, `solarized-dark`, `solarized-light`, and `tokyo-night`; use
`cmdf --list-themes` for the exact list in a build.

## Chart Fences

`libmdf` supports chart fences as a libmdf-specific Markdown extension. This is
not CommonMark and is not currently a Go `mdf` parity feature. Use chart fences
when the document needs small inspectable numeric summaries that should survive
plain terminal output, copied text, and printable HTML without external image
assets.

A chart fence uses a `mdf-*chart` info string, optional comma-separated
options, and two-column CSV rows:

````md
```mdf-bar-chart,sort=desc,colored-bars=on
key,value
Build,40
Test,25
Ship,10
```
````

The first nonnumeric row is treated as a header. Values must be nonnegative
numbers. The first column is the display label; the second column is the raw
numeric value. Percentages are calculated from the sum of the values and are
not supplied in the input.

Chart output is theme-aware in both ANSI and HTML. Labels, values, percentages,
bars, and tile segments use colors from the active theme. ANSI chart rows are
emitted through the same decision-emission surface as other ANSI output, and
HTML preserves the active theme colors by consuming the styled ANSI emissions.

Supported chart types:

- `mdf-bar-chart`, `mdf-horizontal-bar-chart`
- `mdf-vertical-bar-chart`
- `mdf-tile-chart`

Use horizontal bar charts for ranked comparisons where labels and exact values
matter. Use vertical bar charts for compact small datasets where shape matters
more than long labels. Use tile charts for part-of-whole data: they render one
100% stacked bar, a second percentage row, and a raw-value legend. Pie charts
are intentionally not supported; tile charts are the recommended ASCII-friendly
replacement.

Supported options:

- `sort`, `sort=desc`, `sort=descending`, `sort=asc`, `sort=ascending`
- `colored-bars=on`, `colored-bars=off`, `colored-bars=yes`,
  `colored-bars=no`
- `disable-percentage=on`, `disable-percentage=off`,
  `disable-percent=true`, `disable_percent=false`

`colored_bars` is accepted as an underscore spelling. Colored bars are enabled
by default and cycle through active theme accents. `colored-bars=off` uses a
single theme accent for chart marks. `disable_percentage` and
`disable_percent` are accepted as underscore spellings. Horizontal bar chart
percentages are enabled by default and can be hidden with
`disable-percentage`; tile chart percentage rows remain part of the tile chart
presentation.

Horizontal charts fit bars, labels, values, and optional percentages to the
configured content width. In ANSI, `width` and margins define the available
chart width. In HTML, `html_content_width_ch` defines the rendered document
width; the chart is centered as a chart box within that document rather than
padded with terminal spaces. `cmdf -w` sets ANSI width and also sets HTML
content width unless `--html-content-width` is provided.

Vertical charts expand small datasets toward the content width, reduce plotted
point resolution when the x axis would overflow, and suppress unreadable dense
x-axis labels. Tile charts render a single 100% stacked bar with each value as a
colored segment, write centered labels on the first bar row, centered
percentages on the second bar row, and follow with a raw-value legend. Chart
labels, values, and percentages use the same theme color as their bar or tile
segment.

## Streaming Contract

ANSI output is real producer-to-consumer streaming. The core invariant is:

```text
one parser/renderer decision buffer -> one emission -> one sink write
```

When `write_trace` is configured for ANSI output, trace events describe the
exact sink writes in the same order. Tables are the exception to ordinary
word-sized streaming: `MDF_TABLE_BUFFER_FULL` buffers the full table before
rendering, while `MDF_TABLE_BUFFER_ROW` emits header plus first row, then later
rows as the parser can decide them.

The Go `mdf` implementation is the behavioral reference for ANSI streaming.
Parity checks compare the C hot path against that reference.

HTML output is streamed, but byte-for-byte streaming parity with the Go CLI is
not a supported contract. HTML tables rely on browser layout rather than ANSI
alignment.

## cmdf

`cmdf` reads Markdown from a file or stdin and writes ANSI or HTML to stdout or
`--output`.

```sh
cmdf README.md
cmdf --html README.md -o README.html
cmdf README.md -o README.html
cmdf -b -w 80 --margin-left 2 --margin-right 2 README.md
```

Flags:

```text
-h, --help
-V, --version
    --html
    --deck
-b, --boring
-o, --output PATH
-t, --theme NAME
-T, --title TITLE
-w, --width WIDTH
    --margin-left N
    --margin-right N
-8, --osc8 auto|on|off
    --list-themes
    --html-content-width N
    --html-disable-embedded-font
    --html-font-uri URI
    --html-font-regular-uri URI
    --html-font-italic-uri URI
    --html-dump-font
    --html-dump-font-force
    --html-dump-font-path DIR
    --html-dump-font-regular-path PATH
    --html-dump-font-italic-path PATH
    --transition fade|cross|hard
    --slide-numbers
    --deck-center-front-text
    --table-buffer full|row
    --table-wire line|ascii|space
    --simulate
    --simulate-chunk N
    --simulate-delay D
    --trace-writes PATH
```

`--simulate` reads input in small chunks as fast as the renderer can consume
them. `--simulate-delay` is an opt-in demo/probe option for making output timing
visible; it accepts Go-style durations such as `20ms`, `1s`, `500us`, and
compound values. `--trace-writes` is ANSI-only and writes NDJSON records
containing sequence number, format, byte length, and base64 data.

For HTML, libmdf and `cmdf` embed JetBrains Mono WOFF2 data by default.
`--html-disable-embedded-font` instead references the version-pinned JetBrains
hosted regular and italic files. `--html-font-uri` is an external URI base and
derives both standard filenames; `--html-font-regular-uri` and
`--html-font-italic-uri` override either output URI independently. Dumping is
an explicit pre-operation: `--html-dump-font` writes the built-in faces through
libmdf before rendering, while `--html-dump-font-path` selects a directory and
the paired path flags override either destination. Existing files are preserved
unless `--html-dump-font-force` is used. C callers can use the same flow via
`mdf_options` or call `mdf_dump_html_jetbrains_mono_font_to_paths` directly.
Output URIs and dump destinations are intentionally separate. Applications can
provide a different embedded family through `mdf_options.html_font` or obtain
the built-in family through `mdf_html_jetbrains_mono_font`. `cmdf` infers HTML output from
`.html` and `.htm` output paths when `--html` is omitted, warning before it
renders. libmdf sets the HTML document title from the first ATX heading before
any paragraph, falls back to `mdf`, and lets `cmdf` override it with `--title`
or `-T`. Generated HTML documents include the libmdf provenance comment shown
in the API section.

`--deck` renders a standalone HTML slide deck and implies HTML output. Slides
are split on block-level thematic breaks, deck transition mode is selected with
`--transition`/`-x`, and `--slide-numbers` enables browser-filled slide number
placeholders after the first slide. The default transition is `fade`; `cross`
crossfades directly between slides and `hard` switches immediately.

Decks use the normal HTML renderer for each slide body, including chart fences,
themes, embedded fonts, links, tables, and HTML escaping rules. Deck links open
in a new tab/window with `target="_blank"` and `rel="noopener noreferrer"` so
following a link does not replace the presentation. Leading front matter is
consumed as deck metadata: `theme:` selects the default deck theme unless
`--theme` overrides it. `--deck-center-front-text` center-aligns paragraph text
on the first slide only.

Browser navigation supports arrow keys, `j`/`k`, Space, Home/End, `g`/`G`, and
`o` for the previous deck position. Touch screens support guarded horizontal
and vertical swipes. The `f` key and invisible bottom-left corner hit target
toggle fullscreen when the browser exposes it, with a viewport-filling fallback
for mobile browsers that do not.

The comprehensive UX fixture lives at
`testdata/deck-corpus/comprehensive.md`. A local preview can be generated with:

```sh
cmdf --deck --slide-numbers -x fade -o deck.html testdata/deck-corpus/comprehensive.md
```

## Lua

Lua bindings currently target Lua 5.5 only.

```lua
local mdf = require("libmdf")

local out = mdf.render("# hello\n\nworld\n", {
  boring = true,
  width = 80,
})
io.write(out)
```

Streaming:

```lua
local mdf = require("libmdf")
local input = "# hello\n\nworld\n"
local pos = 1

mdf.render_stream(function(cap)
  if pos > #input then return nil end
  local chunk = input:sub(pos, pos + math.min(cap, 8) - 1)
  pos = pos + #chunk
  return chunk
end, function(chunk)
  io.write(chunk)
end, { boring = true })
```

Handle API:

```lua
local mdf = require("libmdf")

local h = mdf.new({ boring = true })
io.write(h:render("# hello\n"))
h:close()
```

HTML and deck handles can set or clear the document/deck title before rendering:

```lua
local h = mdf.new({ format = "deck" })
h:set_html_title("Deck Title")
io.write(h:render("# hello\n"))
h:close()
```

Lua option names mirror the C options where practical:

```text
format = "ansi" | "html" | "deck" | "html_deck"
html = true
deck = true
boring = true
osc8 = true | false
width = N
margin_left = N
margin_right = N
theme = NAME
html_content_width_ch = N
html_title = TITLE
deck_transition = "fade" | "cross" | "hard"
slide_numbers = true
deck_center_front_text = true
table_buffer_mode = "full" | "row"
table_wire_mode = "line" | "ascii" | "space"
write_trace = function(format, chunk) ... end
html_font = {
  family = NAME,
  regular_format = "woff2" | "ttf",
  regular_data = bytes,
  italic_format = "woff2" | "ttf",
  italic_data = bytes,
}
disable_embedded_font = true
font_uri = URI_BASE
font_regular_uri = REGULAR_URI
font_italic_uri = ITALIC_URI
dump_font = true
dump_font_force = true
font_path = DIRECTORY
font_regular_path = REGULAR_PATH
font_italic_path = ITALIC_PATH
```

The generated `cmdf.lua` shipped in CLI archives uses the streaming Lua API and
the libmdf-built-in JetBrains Mono faces for HTML and deck parity with `cmdf`.

The Lua facade also exposes `mdf.version`, `mdf.version_major`,
`mdf.version_minor`, `mdf.version_patch`, `mdf.status`, `mdf.status_string`,
and `mdf.token` constants corresponding to the public C values that are useful
from Lua. Handle objects expose `set_html_title`, `render`, `render_stream`,
`write_token`, `finish`, `error`, and `close`.

## Markdown And HTML Safety

The renderer supports the Markdown constructs covered by the parity corpus:
headings, emphasis, links, autolinks, blockquotes, lists, task lists, fenced and
indented code, front matter, thematic breaks, entities, tables, and inline HTML
handling used by `cmdf`.

HTML output escapes text and attributes. Link `href` output is restricted to
safe schemes: `http`, `https`, `mailto`, and relative URLs. Unsafe schemes such
as `javascript:`, `data:`, `vbscript:`, and `file:` are not emitted as active
anchors. Normal HTML links do not force a new browsing context; deck-mode links
open in a new tab/window with opener protection.

## Development

Run `make help` for the authoritative local command list.

Common commands:

```sh
make build
make test
make asan
make parity
make parity-quick
make lua-test
make package
make package-verify
make release-lua-artifacts
make verify-release-privacy
make release
```

Hardening targets:

```sh
make tsan
make msan
make fuzz-smoke
make fuzz
```

`make parity` runs the exhaustive ANSI, HTML, and streaming parity matrix across
all configured chunks, widths, themes, boring modes, table buffers, and table
wire modes. Streaming parity is chunk-bound and runs as fast as the renderer can
consume input; it does not use sleeps, wall-clock gaps, or timing thresholds.

`make parity-quick` is an explicit local smoke for fast iteration. It does not
replace the full parity matrix.

`make golden-test` verifies byte-for-byte libmdf API output snapshots rendered
through a test utility linked directly against the library. The golden matrix
covers three broad Markdown fixtures across ANSI, asymmetric ANSI margins,
HTML, and deck variants. Regenerate those snapshots deliberately with
`make golden-update` only when the rendered library output change is
intentional.

`make lua-test` also writes `build/cmdf.sh`, a local developer wrapper with
absolute paths to the current checkout's generated `cmdf.lua`, LuaRocks tree,
and debug `libmdf` build. It is generated under `build/` intentionally and can
be run from another working directory while testing local Lua CLI UX.

`make benchmark` measures the direct C libmdf API and Lua binding ANSI, HTML,
and deck paths against `testdata/deck-corpus/comprehensive.md`. Pass extra
options with `BENCH_ARGS`, for example `make benchmark BENCH_ARGS="--rounds 30"`.
`make bench-check` compares those medians with
`testdata/benchmarks/libmdf-baseline.json` and fails if any path is more than
5% slower than baseline. Pass options with `BENCH_CHECK_ARGS`; keep enough
rounds to avoid noise when using the 5% allowance.
`make benchmark-cmdf` is available for the secondary CLI UX comparison.

## Release Cycle

`libmdf` follows the pkt.systems C/CMake lifecycle used by
`c.pkt.systems`:

```text
https://github.com/sa6mwa/c.pkt.systems/
```

The local lifecycle skill is the release authority for this repository. The
public release surface is `make release`: it starts from a clean tree, runs the
prerelease checks, sanitizer checks, fuzz smoke, Lua checks, full Go parity
matrix, release matrix builds, package generation, Lua release artifact
generation, checksum generation, package verification, and artifact
privacy/relocatability checks.

Release artifacts are selected from the generated checksum manifest, not from a
`dist/` glob.

Darwin release builds use osxcross by default. Override the toolchain root with
`OSXCROSS_ROOT` and the host prefix with `CPKT_OSXCROSS_HOST`; when unset they
default to `$HOME/.local/cross/osxcross` and `arm64-apple-darwin25`. Package
verification discovers target tools from explicit overrides, CMake cache
entries, compiler siblings, osxcross target-prefixed tools, and `PATH` last.
Darwin configure, build, and package commands prepend the osxcross `bin`
directory to `PATH` and pass an absolute `-fuse-ld` linker path so `cmdf` and
shared libraries do not accidentally link through a host `ld`. Darwin archives
are verified with target-correct `otool` against the final extracted artifacts.

## License

`libmdf`, `cmdf`, and the Lua bindings are MIT licensed. SDK and CLI artifacts
include the JetBrains Mono SIL Open Font License under their respective
`share/doc/*/OFL.txt` paths.
