# Real map coordinate regression fixtures

The source is the last screenshot attached to the 2026-10-07 report of version
2.3.0 failing to calculate a middle-button ping. It shows `x97.79` and `y111.36`
beside the map crosshair. These fixtures contain only the two coordinate fields;
the player-name popup, chat, desktop and calculator are excluded.

Source image dimensions: 1472 × 1149. Reference crosshair point inferred from
the screenshot: (475, 365). Actual mouse-event metadata is not available in a
static screenshot. Applying the original cursor offsets at that reference
point gives the following rectangles, extracted without resampling:

| Fixture | Source rectangle, right/bottom exclusive | Expected text |
| --- | --- | --- |
| map_live_axis_x.png | (495, 319)–(635, 377) | x97.79 |
| map_live_axis_y.png | (463, 245)–(615, 313) | y111.36 |

Source attachment: `codex-clipboard-693d6266-40c5-4eb0-a226-34691546ac25.png`.
Its SHA-256 is
`c69c587550f0873ef83be4d6c6d260c60cc875a4e764fc5b21a18dde009043c8`.
Independent RGB pixel comparison verifies that both fixture crops are exact
copies of their declared source rectangles.

The X field intentionally retains the clipped map-cursor bracket at its left
edge and the top of the PING caption at its bottom edge. The Y field retains
three repeated `1` digits. The production recognizer must read the complete
labeled axes while rejecting swapped axes, a competing numeric row and clipped
coordinate characters. Bare decimals remain usable only as manual evidence.

Run `wardogs_map_ocr_tests` with model, X fixture and Y fixture paths. Tests also
resample the coordinate fields to check antialiasing at different text sizes.
Original, 75%, 150% and 200% fields retain full automatic evidence. Small-font
map rows use the already detected row's full baseline instead of a split upper
glyph loop. One bounded replication of the original pixels makes tiny glyph
fragments measurable without lowering component, confidence or agreement
guards. This also works when smaller text appears inside a native-sized capture
rectangle. Separate regressions ensure that the retry never restores a clipped
axis label or fractional digit from assumptions.
