# Wrapping Pressure

## Long Single Words

| Key | Value |
| --- | --- |
| token | supercalifragilisticexpialidocious |
| id | pseudopseudohypoparathyroidism |
| checksum | antidisestablishmentarianism |

## Long Phrases

| Topic | Description |
| --- | --- |
| sizing | a full row of phrase content that should wrap deterministically when the render width is narrow |
| balance | another full row of phrase content that competes with the first column for available table width |
| stress | a third row with different phrase pressure so column sizing is not based on one body row |

## Multi Column Wrapping

| First | Second | Third |
| --- | --- | --- |
| left column has several words | middle column also has several words | right column has several words |
| short | middle column has the longest phrase in this row and should wrap | short |
| left column is the long one in this row | short | right column also has a competing phrase |

## Row Buffer Contrast

| Small | Small |
| --- | --- |
| x | y |
| this later row is much wider than the first body row | this proves row buffering fixes widths early |
| another later wide row | another later wide value |
