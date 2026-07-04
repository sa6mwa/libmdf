# Deck Corpus

This directory contains source fixtures for libmdf HTML slide deck mode.

`comprehensive.md` is both an example presentation and a UX test deck. It is
intended to exercise front matter, slide splitting, first-slide layout, dense
autofit cases, headings, inline Markdown, lists, blockquotes, code, tables,
chart fences, links, entities, and thematic-break edge cases.

Use this fixture for manual browser review and generated HTML smoke tests.
Generated deck HTML should include the libmdf provenance comment after the
doctype, open slide links in a new tab/window, and keep `deck.html` as a local
work artifact.

Expected local preview command:

```sh
cmdf --deck --slide-numbers -x fade -o deck.html testdata/deck-corpus/comprehensive.md
```

`deck.html` is a generated work artifact and must not be committed.
