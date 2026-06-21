# Escapes And Entities In Cells

## Escaped Pipes

| A | B | C |
| --- | --- | --- |
| value \| with pipe | 2 | right |
| short | value \| again | right |
| longer value \| with escaped pipe | longer middle value | longer right value |

## Backslashes

| Path | Description |
| --- | --- |
| path\\to\\file | plain escaped backslashes |
| C:\\tmp\\notes | windows-like path |
| longer\\path\\segment | longer right value that should wrap in narrow output |

## Entities

| Entity | Example | Long Example |
| --- | --- | --- |
| ampersand | AT&amp;T | text before AT&amp;T and text after |
| non-breaking | 10&nbsp;000 | values like 10&nbsp;000 and 20&nbsp;000 should remain coherent |
| angle | &lt;tag&gt; | escaped angle entity in a longer phrase that should wrap |
| mixed-case literal | AT&AMP;T | mixed-case named entities such as &Lt;tag&Gt; remain literal |
