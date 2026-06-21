# Regression: Streaming Table Ambiguity

Plain paragraphs should stream before newline, but possible no-edge table rows
need enough lookahead to avoid being consumed as ordinary text.

## Plain Paragraph

hello world streams before newline in live parsing

## No Edge Header Table

A | B
--- | ---
1 | 2
alpha | beta

## No Edge Headerless Table

left | right
one | two
three | four
