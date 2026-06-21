# Regression: Table Spacing Contexts

Tables followed by paragraphs or headings must use the same separator behavior
as the surrounding markdown blocks.

## Table Before Paragraph

| Name | Count |
| --- | ---: |
| apples | 12 |
| bananas | 123 |

Paragraph directly after the table.

## Table Before Heading

| Section | Value |
| --- | --- |
| before heading | ok |
| more data | ok |

## Heading After Table

Expected content after the heading.

## List Table Before Heading

- Inventory:

  | Name | Count | Notes |
  | --- | ---: | --- |
  | apples | 12 | short |
  | bananas | 123 | medium length note |
  | cherries | 1234 | long note with enough words to wrap in narrow output |

## Heading After List Table

Content after a list-contained table.
