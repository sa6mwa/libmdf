# HTML Slide Deck Mode

## Goal

libmdf provides a native slide deck renderer built on the existing HTML output
mode and exposes it through `cmdf` as `--deck`. The feature renders ordinary
Markdown as a fullscreen browser presentation, with slides separated by Markdown
section breaks and with libmdf chart fences supported inside slides.

This is a libmdf extension, like chart fences. It does not need Go `mdf` parity.

## User-Facing Behavior

`cmdf --html -o my.html input.md` continues to emit the existing HTML document
mode.

`cmdf --deck -o my.html input.md` emits a complete standalone HTML slide deck.
The output path extension remains `.html`; the `--deck` flag selects deck mode.

`--deck` implies HTML output. Supplying both `--html` and `--deck` is accepted
when they do not conflict; deck mode wins because it is a more specific HTML
mode. Non-HTML terminal output is not produced in deck mode.

Slide boundaries are Markdown thematic break blocks written as `---` on their
own line, with optional surrounding blank lines. Equivalent thematic break
spellings that the parser recognizes are slide separators only when they are
block-level thematic breaks, not when they appear inside fenced code, chart
fences, inline text, front matter, or other non-break contexts.

YAML-style front matter at the very start of the document remains front matter,
not an empty first slide separator, when it starts with `---` and is closed by a
later `---` before ordinary Markdown content. A leading `---` that is not closed
as front matter is treated as a thematic break slide separator.

Front matter may configure deck defaults. `theme:` selects the default deck
theme when no programmatic `theme_name` and no `cmdf --theme` override is
supplied. Explicit API/CLI theme selection wins over front matter. Unknown
front matter keys are reserved for future deck metadata and are ignored rather
than rendered.

Each slide body is rendered as ordinary Markdown using the existing HTML
renderer behavior and theme/font machinery. Charts must render correctly inside
slides.

## CLI

Deck mode uses these `cmdf` flags:

- `--deck`: render a standalone HTML slide deck.
- `--transition`, `-x`: select slide transition behavior. Values are `fade`,
  `cross`, and `hard`. Default is `fade`.
- `--slide-numbers`: show a slide number indicator on slides after the first slide.
- `--deck-center-front-text`: center-align paragraphs on the first slide.

Invalid transition names are hard errors with actionable usage text.
Deck-specific flags are hard errors unless `--deck` is present, because they
otherwise have no observable meaning in ordinary ANSI or HTML output.

Slide numbering is off by default. `--slide-numbers` is opt-in.

The slide number indicator is formatted as `01/NN`, `02/NN`, etc. Width is
based on the total slide count. The first slide never shows an indicator. The
visual style uses the same theme family as lower ATX headings.

Existing HTML-related CLI options continue to apply in deck mode, including
theme selection, boring mode where meaningful, title handling, built-in
JetBrains Mono embedding in libmdf and `cmdf`, external or dumped font
references, and HTML content/font settings.

When both front matter `theme:` and `cmdf --theme` are present, `--theme` wins.

## Public C API

Deck mode is exposed programmatically without requiring CLI-specific behavior.

Public ABI additions:

- `MDF_FORMAT_HTML_DECK` after `MDF_FORMAT_HTML`.
- Deck options in `mdf_options`:
  - `deck_transition`, an enum with `MDF_DECK_TRANSITION_FADE`,
    `MDF_DECK_TRANSITION_CROSS`, and `MDF_DECK_TRANSITION_HARD`.
  - `slide_numbers`, boolean integer.
  - `deck_center_front_text`, boolean integer.
- HTML font options in `mdf_options`, including `html_font_source`, base and
  per-face external URIs, and paired dump destinations and force behavior.
- Built-in font helpers: `mdf_html_jetbrains_mono_font`, the paired
  `mdf_dump_html_jetbrains_mono_font*` functions,
  `mdf_html_font_resolve_dump_paths`, and `mdf_paths_alias`.

The feature intentionally uses a direct public API rather than hiding deck
controls behind CLI-only behavior.

`mdf_create(MDF_FORMAT_HTML_DECK, &opts, &renderer)` renders a deck. Existing
`MDF_FORMAT_HTML` behavior must remain unchanged.

`mdf_set_html_title` applies to deck mode the same way it applies to HTML mode.
The document title uses the explicit title if provided. Otherwise, libmdf uses
its initial ATX heading title detection when rendering HTML or a deck.

Programmatic `opts.theme_name` wins over front matter `theme:`. Front matter
`theme:` only fills the theme when the caller leaves `theme_name` unset.

## Rendering Model

Deck mode materializes one slide at a time. It is not part of the ANSI streaming
hot-path contract, but it must not silently materialize the full Markdown source
in normal operation.

The renderer buffers naturally per slide:

1. Accumulate one slide's Markdown until a slide separator is reached.
2. Render that slide through the HTML renderer into that slide's section.
3. Release per-slide source buffers before moving to the next slide where
   practical.

The C renderer does not need the final slide count before emitting slides. When
`slide_numbers` is enabled, it emits slide-number placeholders and lets the
browser-side deck script compute `NN/TT` from the final section count. This
preserves per-slide Markdown buffering and avoids a full-source prepass for
non-seekable inputs.

For `render_cstr`, the caller-provided string is already materialized by the
caller. The deck renderer still treats it as a source stream internally and does
not add a second full-source Markdown buffer.

Thematic breaks are the authoritative slide boundaries. The deck splitter is
block-aware and honors fenced code, chart fences, front matter, blockquotes,
lists, and other contexts where `---` is not a thematic break.

## HTML Structure

Deck mode emits one complete standalone HTML document.

The deck shell includes:

- semantic slide containers, for example `<section class="mdf-slide">`;
- a root class that identifies deck mode, for example `<main class="mdf-deck">`;
- a current-slide state class or attribute controlled by JavaScript;
- embedded CSS required for fullscreen presentation;
- embedded JavaScript required for navigation, transitions, and autofit;
- a touch-friendly fullscreen control that uses the browser Fullscreen API when
  available and falls back to a viewport-filling deck state when true fullscreen
  is not exposed by the mobile browser.
- a desktop `f` keyboard shortcut that toggles the same fullscreen behavior.

No external runtime dependencies are required. The generated deck works from a
local file URL.

Generated deck and ordinary HTML documents include this provenance comment
immediately after the doctype:

```html
<!-- Generated by libmdf (C) 2026 Michel Blomgren https://pkt.systems/c/libmdf -->
```

Use the existing HTML renderer for Markdown block and inline output wherever
possible. Deck mode should not fork Markdown semantics.

## Layout

Slides are fullscreen viewport pages.

The generated deck must adapt to materially different viewport sizes, including
mobile portrait, mobile landscape, tablet, laptop, and desktop windows. Normal
success means slide content is visible without incoherent cropping or overlapping
controls. Dense slides may scroll internally only after font fitting reaches its
minimum useful size.

The first slide is the front page:

- content is vertically and horizontally centered as a block;
- if the slide contains only one `#` heading, that heading is fully centered;
- subheaders and paragraphs may be included;
- paragraph blocks are centered as blocks but remain normal text-aligned by
  default;
- `--deck-center-front-text` center-aligns first-slide paragraph text.

Slides after the first use standard presentation layout:

- ATX heading sizes are stable across all slides;
- the leading `#` slide title remains fixed at the top;
- the remaining body block is vertically centered by default as one block,
  including subheadings, paragraphs, lists, quotes, code, tables, and charts;
- normal paragraphs, lists, blockquotes, code blocks, tables, charts, and other
  body content are autofitted per slide;
- more body content yields smaller body text;
- less body content yields larger body text;
- autofit is based on available viewport space and must prevent overflow in
  common fullscreen desktop and laptop viewports.

The implementation separates fixed heading scale from body autofit. The
browser-side deck script measures each slide after load and resize events, then
resizes a body-content wrapper until it fits or reaches the fallback scroll
threshold.

Autofit must not hide content by clipping as a normal success path. If a slide
cannot fit within configured min/max font limits, it may use internal scrolling
as a last-resort fallback.

## Transitions

`fade` is the default:

- current slide fades out to black;
- next slide fades in from black.
- fade-out and fade-in use the same duration, tuned slow enough to be
  perceptible without feeling sluggish.

`cross`:

- current and next slides crossfade directly.

`hard`:

- current slide is hidden and next slide is shown immediately.

Transition behavior is implemented in CSS/JS in the deck shell. Motion is short
and presentation-safe. `prefers-reduced-motion` uses hard transitions or
near-zero durations.

## Navigation

Keyboard controls:

- Right arrow, Down arrow, Space, PageDown, `j`: next slide.
- Left arrow, Up arrow, PageUp, `k`: previous slide.
- Home and `g`: first slide.
- End and `G`: last slide.
- `o`: return to previous slide position.

Navigation must update the browser URL hash or history state so reload/share of
the current slide is possible. `o` returns to the previous deck position tracked
by deck navigation, not browser-origin history outside the deck.

Touch navigation:

- Swipe left: next slide.
- Swipe right: previous slide.
- Swipe up: next slide.
- Swipe down: previous slide.

Swipe handling uses movement thresholds and directional guards so taps and
diagonal gestures do not advance the deck. Vertical swipe navigation must not
hijack scrolling inside dense slides; when the current slide content is
scrollable, only treat vertical swipes as deck navigation at the relevant scroll
edge.

## Accessibility And Browser Behavior

Only the current slide is reachable by normal tab navigation. Inactive slides
are hidden from assistive technology with `aria-hidden`.

Slides should preserve selectable text and usable links. Deck links open in a
new tab/window with `target="_blank"` and `rel="noopener noreferrer"` so link
activation does not replace the presentation or accidentally advance the deck.

The deck works without network access. With JavaScript disabled, the HTML still
shows the first slide's rendered content and does not display raw implementation
artifacts.

## Tests And Verification

Unit and integration tests prove observable behavior.

Covered behavior:

- `cmdf --deck -o out.html input.md` produces a standalone HTML document.
- `--deck` implies HTML output and does not require `--html`.
- `--transition fade`, `--transition cross`, `--transition hard`, and `-x` are
  accepted and reflected in output state.
- invalid transition values fail with an actionable error.
- deck-specific flags such as `--transition` and `--slide-numbers` fail with an
  actionable error when `--deck` is omitted.
- slide separators split slides only at block-level thematic breaks.
- leading front matter is not emitted as slide content and a top-level `theme:`
  key selects the deck theme only when no explicit API/CLI theme override is
  present.
- first-slide front-page centering classes are emitted only for slide 1.
- slide number output is omitted by default, omitted on slide 1, and present on
  later slides when `--slide-numbers` is set.
- charts inside slides render through the HTML chart path.
- existing non-deck HTML output remains byte-for-byte stable where fixtures
  already assert it, or behaviorally stable where byte-for-byte output is too
  brittle.

Programmatic API tests:

- `mdf_create(MDF_FORMAT_HTML_DECK, ...)` renders a deck from `render_cstr`.
- deck transition, slide-number, and front-text options affect output.
- deck body centering is the default.
- ordinary `MDF_FORMAT_HTML` ignores deck options and remains unchanged.

Browser-level smoke verification is kept outside the ordinary CTest suite. The
checked-in tests assert emitted CSS/JS hooks that implement navigation,
transitions, fullscreen behavior, focus management, and autofit.

Run the standard project checks before completion:

```sh
make test
```

For full local verification, run:

```sh
make test-hardening
```

That target covers debug tests, ASan/UBSan, TSan, MSan, fuzz smoke, and full
ANSI/HTML/stream/Lua parity.

## Architecture Notes

Regular HTML and deck HTML share escaping, theme variables, embedded font
support, chart rendering, and title behavior.

Avoid creating a second Markdown renderer. Deck mode is a composition of:

- slide boundary handling;
- existing HTML rendering for each slide;
- deck-specific document shell;
- deck-specific CSS and JavaScript.

Regular HTML mode remains separate except for shared helpers.

The deck renderer does not introduce direct ANSI hot-path writes or affect ANSI
streaming semantics.

## Non-Goals

- PDF export.
- Speaker notes.
- Presenter view.
- Remote control.
- Incremental bullet reveals.
- External JavaScript or CSS dependencies.
- Theme authoring changes beyond mapping existing theme data into deck CSS.
