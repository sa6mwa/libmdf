# Chart Corpus

This directory contains source fixtures for the `mdf-*chart` fenced-block
extension. Chart fences are a libmdf-specific Markdown extension, not
CommonMark and not currently a Go `mdf` parity feature.

Chart fences use a `mdf-*chart` info string, optional comma-separated options,
and two-column CSV rows:

````md
```mdf-bar-chart,sort=desc,colored-bars=on
label,value
Build,40
Test,25
Ship,10
```
````

The first nonnumeric row is treated as a header. Values must be nonnegative
numbers. Percentages are calculated automatically from the total. Labels,
values, percentages, bars, and tile segments are themed in both ANSI and HTML.

Recommended usage:

- Use `mdf-bar-chart` or `mdf-horizontal-bar-chart` for ranked comparisons
  where labels and exact values matter.
- Use `mdf-vertical-bar-chart` for compact small datasets where shape matters
  more than long labels.
- Use `mdf-tile-chart` for part-of-whole data. It is the intended replacement
  for pie charts in text output.

Supported options are `sort`, `sort=desc`, `sort=descending`, `sort=asc`,
`sort=ascending`, and `colored-bars=on|off|yes|no`. `colored_bars` is accepted
as an underscore spelling. Colored bars are enabled by default.

ANSI charts fit to the configured terminal/content width after margins. HTML
charts use the HTML document content width and are centered as chart boxes
without terminal-space padding.

Render previews with:

```sh
scripts/render_chart_corpus.sh
```

The script writes inspectable ANSI and HTML output under
`build/chart-corpus-preview/`.
