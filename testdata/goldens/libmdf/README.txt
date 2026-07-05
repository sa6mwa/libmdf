# libmdf Golden Outputs

These files are byte-for-byte library output invariants generated from
`testdata/goldens/libmdf/MANIFEST.txt`.

They are rendered by `tests/golden_render.c`, which links directly against
libmdf and exercises the public `mdf_create` / `renderer->render` API. These
are the primary invariants for library-side performance work.

Regenerate intentionally with:

```sh
make golden-update
```

Verify with:

```sh
make golden-test
```

The matrix covers three broad Markdown fixtures across ANSI, ANSI margins,
HTML, and deck output. Each fixture carries styled and boring ANSI layout
goldens across 20, 40, 80, and 120 column widths with zero, one-sided,
asymmetric, and heavy symmetric margin profiles. The narrowest golden cases
leave at least 15 content columns; one- and two-column ANSI content is rejected
by the API tests instead of snapshotted. `testdata/deck-corpus/comprehensive.md`
remains the benchmark fixture.
