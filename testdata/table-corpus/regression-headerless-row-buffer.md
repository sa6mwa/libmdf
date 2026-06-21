# Regression: Headerless Row Buffering

Headerless tables must render the same divider structure in full and
row-buffered mode.

## Headerless Edge Pipe Table

| A | B |
| 1 | 2 |
| 3 | 4 |

## Headerless Late Extra Cell

Row-buffered rendering must not grow the frame after the first two rows, but
the later extra cell must remain visible in the final column.

| A | B |
| 1 | 2 |
| 3 | 4 | 5 |

## Headerless No Edge Pipe Table

A | B | C
1 | 2 | 3
alpha | beta | gamma
