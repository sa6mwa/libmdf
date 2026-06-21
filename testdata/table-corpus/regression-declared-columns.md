# Regression: Declared Column Count

Tables with a delimiter row have a declared column count. Body rows with
missing cells are padded and body rows with extra cells are truncated.

## Short And Extra Body Rows

| A | B | C |
| --- | --- | --- |
| one | two |
| three | four | five |
| six | seven | eight | EXTRA |

## Wrapping Extra Cell Must Not Grow Layout

| Name | Notes |
| --- | --- |
| apples | normal note |
| bananas | note before extra column | EXTRA-LONG-CELL-THAT-MUST-NOT-AFFECT-WIDTH |
| cherries | long note with enough words to wrap in narrow output |
