# Edge Pipe Syntax

## Both Edge Pipes

| A | B |
| --- | --- |
| 1 | 2 |
| alpha | beta |
| longer left value | longer right value |

## No Edge Pipes

A | B
--- | ---
1 | 2
alpha | beta
longer left value | longer right value

## Leading Edge Pipe Only

| A | B
| --- | ---
| 1 | 2
| alpha | beta
| longer left value | longer right value

## Trailing Edge Pipe Only

A | B |
--- | --- |
1 | 2 |
alpha | beta |
longer left value | longer right value |

## One Column With Edge Pipes

| A |
| --- |
| 1 |
| alpha |
| longer single column value |

## Literal Angle Text With Pipe

| Left | Middle | Right |
| --- | --- | --- |
| a <x|y> | z | right |
| <https://example.com/a|b> | autolink keeps its pipe | right |
| <span data-x="a|b">value</span> | html attribute keeps its pipe | right |
