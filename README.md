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

The CLI archives embed JetBrains Mono for HTML output and therefore include the
JetBrains Mono `OFL.txt`. The library SDK archives do not embed the font and do
not ship `OFL.txt`.

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
  ANSI output unless `--width` is set.
- `margin_left`, `margin_right`: ANSI-only margins. Left margin spaces are
  emitted lazily only on non-empty lines. Right margin reduces available wrap
  width.
- `boring`: disable ANSI styling.
- `osc8`: enable OSC8 hyperlink output for ANSI.
- `theme_name`: theme name. `default` is used when unset.
- `html_content_width_ch`: HTML document content width in `ch`. Default is
  `96`.
- `html_font`: optional user-supplied HTML font data. The library does not
  embed a default font.
- `table_buffer_mode`: `MDF_TABLE_BUFFER_FULL` or `MDF_TABLE_BUFFER_ROW`.
- `table_wire_mode`: `MDF_TABLE_WIRE_LINE`, `MDF_TABLE_WIRE_ASCII`, or
  `MDF_TABLE_WIRE_SPACE`.
- `write_trace`: callback for ANSI decision-emission tracing.
- `allocator`, `emission_buffer`, `memory`: custom memory and emission-buffer
  control.

Available themes can be queried with `mdf_theme_count`, `mdf_theme_name`, and
`mdf_theme_exists`. Theme lookup is case-insensitive. The built-in set includes
`default`, `ayu-light`, `ayu-mirage`, `catppuccin-mocha`, `dracula`,
`everforest`, `github-dark`, `github-light`, `gruvbox`, `kanagawa`, `nord`,
`rose-pine`, `solarized-dark`, `solarized-light`, and `tokyo-night`; use
`cmdf --list-themes` for the exact list in a build.

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
cmdf -b -w 80 --margin-left 2 --margin-right 2 README.md
```

Flags:

```text
-h, --help
-V, --version
    --html
-b, --boring
-o, --output PATH
-t, --theme NAME
-w, --width WIDTH
    --margin-left N
    --margin-right N
-8, --osc8 auto|on|off
    --list-themes
    --html-content-width N
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

For HTML, `cmdf` embeds JetBrains Mono woff2 data by default. The library API
does not hardcode this font; applications can provide their own font bytes or
callbacks through `mdf_options.html_font`.

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

Lua option names mirror the C options where practical:

```text
format = "ansi" | "html"
html = true
boring = true
osc8 = true | false
width = N
margin_left = N
margin_right = N
theme = NAME
html_content_width_ch = N
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
```

The generated `cmdf.lua` shipped in CLI archives uses the streaming Lua API and
embeds JetBrains Mono for HTML parity with `cmdf`.

## Markdown And HTML Safety

The renderer supports the Markdown constructs covered by the parity corpus:
headings, emphasis, links, autolinks, blockquotes, lists, task lists, fenced and
indented code, frontmatter, thematic breaks, entities, tables, and inline HTML
handling used by `cmdf`.

HTML output escapes text and attributes. Link `href` output is restricted to
safe schemes: `http`, `https`, `mailto`, and relative URLs. Unsafe schemes such
as `javascript:`, `data:`, `vbscript:`, and `file:` are not emitted as active
anchors.

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

## Release Cycle

`libmdf` follows the pkt.systems C/CMake lifecycle used by
`c.pkt.systems`:

```text
https://github.com/sa6mwa/c.pkt.systems/
```

The local lifecycle skill is the release authority for this repository. The
public release surface is `make release`: it starts from a clean tree, runs the
prerelease checks, sanitizer checks, fuzz smoke, Lua checks, full Go parity
matrix, package generation, checksum generation, package verification, and
artifact privacy/relocatability checks.

Release artifacts are selected from the generated checksum manifest, not from a
`dist/` glob.

## License

`libmdf`, `cmdf`, and the Lua bindings are MIT licensed. CLI artifacts that
embed JetBrains Mono also include the JetBrains Mono SIL Open Font License in
`share/doc/cmdf/OFL.txt`.
