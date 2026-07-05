---
title: "Mixed Layout Golden Fixture"
theme: "everforest"
---

# Mixed Layout Golden Fixture

This fixture is intentionally compact but broad. It exercises headings,
paragraph wrapping, inline *emphasis*, **strong text**, `inline code`, links,
autolinks like https://example.com/mixed, tables, charts, quotes, lists, code,
and deck slide boundaries.

---

# Inline And Containers

> Quoted prose should preserve quote styling while wrapping under configured
> widths and margins.
>
> - Quoted list item with **strong text**.
> - Quoted list item with [a link](https://example.com/quoted-list).

- Top-level list item with enough text to wrap in narrow ANSI output.
- Nested material:
  - Nested item with `inline code`.
  - Nested item with *emphasis* and **strong text**.

1. Ordered item alpha.
2. Ordered item beta with a descriptive link to
   [the project](https://pkt.systems/c/libmdf).

---

# Table And Code

| Area | Status | Notes |
| --- | :---: | --- |
| Parser | green | Keeps token order stable. |
| ANSI | green | Margins are absolute and style escapes must stay inside them. |
| HTML | green | Escapes text and emits semantic wrappers. |
| Deck | watch | Slide wrappers must not perturb body HTML. |

```c
static int boundary(const char *s)
{
    return s != NULL && s[0] == '-' && s[1] == '-' && s[2] == '-';
}
```

The literal thematic break marker in the code fence must not split a deck
slide.

---

# Horizontal Chart

```mdf-bar-chart,sort=descending
Stage,value
Design,12
Implementation,29
Verification,34
Release,8
```

---

# Vertical Chart

```mdf-vertical-bar-chart
Quarter,value
Q1,7
Q2,13
Q3,9
Q4,18
```

---

# Tile Chart

```mdf-tile-chart,sort=descending
Surface,value
CLI,16
Lua,11
HTML,21
Deck,18
Tests,24
```
