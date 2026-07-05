---
title: "Container Chart Golden Fixture"
theme: "tokyo-night"
---

# Container Chart Golden Fixture

This fixture concentrates on chart rendering inside Markdown containers. It is
used as an ANSI, HTML, and deck golden input because container prefixes are a
common source of width, margin, and wrapping regressions.

---

# Blockquote Charts

> Before the horizontal chart.
>
> ```mdf-bar-chart
> key,value
> Evidence,53
> Habit,11
> Review,24
> ```

> Before the vertical chart.
>
> ```mdf-vertical-bar-chart
> key,value
> Mon,4
> Tue,7
> Wed,3
> Thu,9
> ```

---

# List Charts

- Horizontal chart in a list:

  ```mdf-horizontal-bar-chart
  key,value
  Build,40
  Test,25
  Ship,10
  ```

- Tile chart in a list:

  ```mdf-tile-chart
  key,value
  Parser,19
  Renderer,31
  CLI,14
  Lua,9
  ```

---

# Adjacent And Lazy Containers

> text immediately before chart
> ```mdf-bar-chart
> A,1
> B,2
> ```

- list text immediately before chart
  ```mdf-vertical-bar-chart
  A,1
  B,2
  ```

The paragraphs around these charts should remain separate and should not absorb
container prefixes or chart rows.

---

# Mixed Table And Links

| Container | Chart | Risk |
| --- | --- | --- |
| blockquote | horizontal | prefix accumulation |
| unordered list | tile | indentation drift |
| lazy quote | bar | boundary detection |

[Container link](https://example.com/container-chart) and bare
https://example.com/container-chart/autolink should both remain safe in HTML and
deck output.
