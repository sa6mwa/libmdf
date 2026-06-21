# Inline Code And Links In Cells

## Inline Code

| Variant | Example | Long Example |
| --- | --- | --- |
| code | `value` | `very-long-inline-code-token-that-should-force-wrapping` |
| code pipe | `x|y` | before `left|right` after enough text to wrap |
| code phrase | `go test ./...` | `command with spaces and flags --table-wire line --table-buffer full` |
| mixed code | before `code` after | before `code span with several words` after enough text to wrap |

## Links

| Variant | Example | Long Example |
| --- | --- | --- |
| inline link | [site](https://example.com) | [long descriptive link label that should wrap](https://example.com/long/path) |
| link label pipe | [left|right](https://example.com) | before [label|with|pipes](https://example.com/long/path) after |
| link destination pipe | [pipe url](https://example.com/a|b) | [long label](https://example.com/long/path/with|pipe) |
| reference-like text | [label] | bracketed text that is not a resolved reference |
| reference-like pipe | [left|right] | bracketed [label|with|pipes] text |
| link in sentence | open [docs](https://example.com/docs) now | long sentence before [documentation link](https://example.com/docs/reference/table-renderer) and after |

## Autolinks

| Variant | Example | Long Example |
| --- | --- | --- |
| url | <https://example.com> | <https://example.com/really/long/path/for/table/rendering> |
| url pipe | <https://example.com/a|b> | text before <https://example.com/long|pipe/path> and after |
| email | <user@example.com> | <long.user.name@example.com> |
| mixed | <https://example.com> and `code` | long text with <https://example.com/path> and `inline code` in one cell |
