# Container Context Tables

## Block Quote Short Table

> | Name | Count |
> | --- | ---: |
> | apples | 12 |
> | bananas | 123 |
> | cherries | 1234 |

## Block Quote Wrapping Inline Content

> | Variant | Example | Long Example |
> | --- | --- | --- |
> | emphasis | *italic text* | *italic phrase with enough words to wrap inside a quoted table cell* |
> | strong | **bold text** | **bold phrase with enough words to wrap inside a quoted table cell** |
> | link | [docs](https://example.com/docs) | [long descriptive documentation link that should wrap](https://example.com/docs/reference/table-renderer) |
> | code | `go test` | `very-long-inline-code-token-that-should-force-wrapping` |

## Block Quote Headerless Table

> | headerless name | headerless value |
> | alpha | beta |
> | gamma | delta |

## Unordered List Table

- Inventory:

  | Name | Count | Notes |
  | --- | ---: | --- |
  | apples | 12 | short |
  | bananas | 123 | medium length note |
  | cherries | 1234 | long note with enough words to wrap in narrow output |

## Unordered List Wrapping Inline Content

- Rich cells:

  | Variant | Example | Long Example |
  | --- | --- | --- |
  | mixed | plain *italic* **bold** `code` | plain text before *italic phrase* then **bold phrase** and `inline code` after |
  | link | [site](https://example.com) | [full link label that should wrap across multiple rendered table rows](https://example.com/really/long/path/for/table/rendering) |
  | url | https://example.com | https://example.com/really/long/path/for/table/rendering |
  | entity | AT&amp;T | escaped &lt;tag&gt; and 10&nbsp;000 in one list table cell |

## Task List Table

- [ ] Task item with a table:

      | Task Column | Value | Notes |
      | --- | --- | --- |
      | unchecked | [task docs](https://example.com/tasks) | task-list tables keep the full checkbox continuation indent |
      | wrapped | **bold task value** | long task-list table content with enough words to wrap in narrow output |

## Ordered List Table

1. Metrics:

   | Metric | Value |
   | --- | ---: |
   | requests | 1000 |
   | failures | 2 |
   | long metric name with wrapping | 123456 |

## Nested List Table

- Outer item
  - Nested item:

    | Nested | Value |
    | --- | --- |
    | short | value |
    | link | [nested docs](https://example.com/nested/docs) |
    | long | nested list table text with enough words to wrap |

## Block Quote Containing List Table

> - Quoted list item:
>
>   | Quoted List | Value |
>   | --- | --- |
>   | short | value |
>   | emphasis | *italic* and **bold** |
>   | link | [quoted list docs](https://example.com/quoted/list/docs) |

## List Followed By Standalone Table

- item before standalone table
- another item before standalone table

| Standalone | Value |
| --- | --- |
| must not inherit list indentation | ok |
| long standalone value | long standalone value with enough words to wrap |

## Block Quote Followed By Standalone Table

> quoted paragraph before standalone table

| Outside Quote | Value |
| --- | --- |
| must not inherit quote prefix | ok |
| long outside quote value | long outside quote value with enough words to wrap |
