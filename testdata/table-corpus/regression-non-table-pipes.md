# Regression: Non Table Pipe Paragraphs

Pipe-containing paragraphs that are not tables must go through normal paragraph
and inline parsing when the following line proves they are not a table.

## Pipe Paragraph Inline Markdown

A | *em* [site](https://example.com)
not a table

## Pipe Paragraph Before Table

literal | **strong** `code` text
still paragraph text

| Real | Table |
| --- | --- |
| value | ok |
| another value | still ok |
