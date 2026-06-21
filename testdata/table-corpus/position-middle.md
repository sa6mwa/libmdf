# Table Between Paragraphs

Paragraph before the table establishes normal text context.

| Step | Owner | Result |
| --- | --- | --- |
| collect | parser | table starts after paragraph |
| render | ansi | table ends before next paragraph |
| verify | tests | surrounding text survives |

Paragraph after the table establishes the following block context.
