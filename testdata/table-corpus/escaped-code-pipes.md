| `Head\|er` | Action |
| --- | --- |
| `/output [compact\|full]` | Show or change tool previews. |
| ``a`\|b`` | Two-backtick code delimiter. |
| ```c``\|d``` | Three-backtick code delimiter. |
| `one\|two\|three` | Repeated escaped pipes. |
| `one\\|two` | Preserve the other backslash. |
| `three\\\|four` | Preserve two other backslashes. |
| `path\to\file` | Preserve ordinary code backslashes. |
| `x\*y` | Preserve other code escapes. |
| **strong\|text** | Escaped pipe in emphasis. |
| plain\|text | Escaped pipe in ordinary text. |
| \`literal\|text | Escaped code delimiter. |
| `unclosed\|text | Preserve unmatched code syntax. |

Outside the table: `compact\|full` retains its literal backslash.
