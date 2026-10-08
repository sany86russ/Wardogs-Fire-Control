# How the game calculations work

This calculator is intended only for WARDOGS. The checks described below establish program consistency and preservation of the original tables. They do not establish impact accuracy in the current game version.

## Coordinates, distance and bearing

- Map coordinates are `x` and `y`; 1 map unit = 100 meters.
- Range is the length of the line segment between the gun and the target on the map. Elevation is not part of this horizontal distance.
- Bearing is measured clockwise from positive `y`: north 0°, east 90°, south 180°, west 270°.
- Bearing is normalized to `[0°, 360°)`: north is returned as 0°, including rounding of an almost-north direction. Target history restores the original point at full manual-input precision, rather than using the rounded list label.
- For coincident points, range and the conventional bearing are zero; calibration does not accept such impacts.
- Manual input preserves the fractional part. OCR keeps two decimal places and corrects lookalike characters `I/l/| → 1`, `O/o → 0`. All recognized pairs are available for ambiguity review; the compatible single-pair parser chooses the last complete pair.
- Decimal-point numbers are read independently of Windows regional settings. Infinity, NaN and overflow of coordinate differences or conversion to meters are rejected.
- A custom OCR pattern is limited to simple labels, separators and two numeric groups. Alternation, backreferences, lookahead/lookbehind, repeated groups, arbitrary wildcards and ambiguous repetition chains are rejected before recognition. This reduces expensive regular-expression backtracking; the engine does not have an exact internal cancellation timer.

## L81 and SPH-2 tables

L81 uses 71 original points from the [apollyon-sys/wardogs-calculator project's weapons.json](https://github.com/apollyon-sys/wardogs-calculator/blob/main/data/weapons.json), saved on October 3, 2026. Supported range: **132–684 m**, **850–150 MIL**. Linear interpolation is used between rows; calculations beyond the table are rejected.

**MIL is an aiming scale, not miles.** Meter values and MIL values are not compared with each other. The table row **208 m → 780 MIL** is also valid: 780 sets the sight, while 208 specifies range. For example, a gun at `98.54 / 110.33` and a target at `101.72 / 109.13` give 339.888 m. Between 339 m → 650 MIL and 348 m → 640 MIL, the result is 649.013 MIL. The screen displays **340 m / 649 MIL**. Aiming is calculated from the full range before display rounding. All 71 rows were checked again against the public source on October 7, 2026; this is a community project's table, not an official BULKHEAD dataset.

SPH-2 uses observations from [wardogs.t0ki.cn/js/data.js](https://wardogs.t0ki.cn/js/data.js), saved in the recovered source project on September 11, 2026:

| Trajectory | Rows | Horizontal table range | Sight setting |
| --- | ---: | --- | --- |
| Low (`low`) | 59 | 1181–2629 m | 20–600 MIL |
| High (`high`) | 80 | 735–2629 m | 610–1400 MIL |

All 139 rows were checked again on October 7, 2026 against the then-current t0ki, Rico and Apollyon sources; they matched. The maximum of 1400 MIL was also confirmed in the game by SoNiX. These are game sight tables. MIL cannot be treated directly as a physical launch angle: platform calculations first convert the value through the original table to an equivalent range, then to an angle in the adopted model. A separate direction helper using MIL follows the standard convention `1000 MIL = 1 radian`; the main SPH-2 path uses the table.

In the high table, both 610 and 620 MIL have a range of 2629 m, while 630 MIL has 2628 m. The inverse conversion is therefore ambiguous: exactly 2629 m selects the original first row, 610 MIL. Between these rows, reverse conversion can differ by up to 1 m; this is a property of the preserved observations.

## Elevation and platform tilt

The retained elevation correction model uses an assumed SPH-2 maximum range of **2629 m** and the two solutions of one approximate trajectory. Elevation is in meters: a positive difference means the target is above the gun. After correction and platform rotation, the result is converted back to the sight table. An unreachable target, unsupported angle or final value outside the sight table produces an error.

To interpret an observed impact, the model permits a mathematical continuation down to 0 m / 0 MIL for the low trajectory and 0 m / π·500 MIL for the high trajectory. These values are needed for calibration; they do not extend the supported range of the final game sight setting.

Terrain packages contain a 2 m grid with 0.1 m height resolution; bilinear interpolation is used between four vertices. Connection checks SHA-256, mapId and geometry; reading checks decompressed block sizes and CRC. A negative Y scale preserves coordinate alignment. A point outside coverage is not assigned zero elevation. Selecting a map is mandatory and confirmation is required at launch; changing the map clears coordinates and corrections. The selected map's heights apply to both SPH-2 trajectories and the impact location. For the training ground and an unknown map, the user explicitly selects a mode without heights; L81 keeps its existing table without an elevation model. Local import atomically writes verified bytes to the persistent user data directory.

Flight time, dispersion, wind, target movement and obstacles along the trajectory are not modeled. The elevation model is an approximation, not recovered internal game constants. The experimental fitted-drag model from upstream 1.4.1 was not imported: limited in-game observations do not justify a promise of greater accuracy.

## Direct calculation and optional correction

At L81's 132 and 684 m boundaries, only `double` calculation error up to approximately 5·10⁻¹² m is tolerated. This prevents false rejection after subtracting decimal coordinates; real points outside the table remain rejected, and all 71 rows are preserved.

In 2.6.0, bearing and MIL use independent consistency weights. The local correction is normalized by the actual relative weights; low consistency alone does not return the estimate to zero. Spatial attenuation is applied separately. Replay of four retained field observations passed as a regression check on October 8; new in-game trials are still necessary to assess accuracy.

Since 2.5.0, the main SPH-2 path does not reconstruct platform tilt from two shots. It first calculates horizontal range, bearing, the game's table-based aiming for the selected trajectory, and an elevation correction if suitable heights are available. With no impacts, the local correction is zero. The public two-point calibration function remains for compatibility and separate tests; the ordinary UI and Alt+I do not call it.

Alt+I optionally records an actual impact for the current target. Each record captures the selected trajectory, commanded bearing and MIL, target and elevation when capture begins. The application does not observe the shot itself or the actual sight settings.

The snapshot retains the unrounded calculated command. Card values are rounded to 0.1° and a whole MIL, so a manual setting based on them can differ from the snapshot by up to 0.05°/0.5 MIL. When assessing small misses, the application cannot attribute this difference exclusively to the model.

Deviations are measured against the unchanged nominal impact calculation: `Δbearing = wrap(commanded bearing − nominal impact bearing)`; `ΔMIL = commanded MIL − nominal impact MIL for the same trajectory`. A bounded local estimate of these deviations is added to the next aiming solution. For the high trajectory, a short shot requires decreasing MIL; for the low trajectory, increasing MIL. Corrections from repeated shots are not summed: each snapshot already contains the commanded aiming solution.

One impact does not determine the gun's three-dimensional tilt. Local mode leaves the original matrix unchanged, uses only observations of the same trajectory within **50 m** of the target and does not create a global rotation or transfer a global offset to another part of the map. Influence decreases smoothly toward the radius boundary and is zero beyond it. Since 2.5.1, the first geometrically valid impact applies the full measured correction at the same target; conflicting nearby observations weaken the estimate. Independent ±3°/±50 MIL limits were removed: they rejected a confirmed 236.70 m miss at a range of 2210.33 m. Instead, the target-to-impact distance is checked: it must be no greater than **20% of horizontal range and 400 m**, simultaneously. The commanded setting is also checked by the distance between its horizontal aiming vector and the current calculation, using the same limit. The final command must remain within the available scale and must not depart from the original command by more than twice the geometric limit. The double allowance accommodates nonlinear table-based aiming while limiting the accumulated displacement. These are application rules, not recovered game-dispersion parameters. A recording error preserves the entire previous history and aiming solution. Reset clears refinements and immediately restores the original direct calculation.

History is limited to 256 observations. The consistency score from 0 to 1 is not a hit probability. Software regressions check correction signs, a repeated shot with an already corrected command, isolation of trajectories and distant targets, rejection without state changes, and memory bounds.

SPH-2's main distance in meters is the horizontal distance to the accepted target, identical for both trajectories. The equivalent table range is marked as approximate. In the supplied game sight, 660/670/680 MIL corresponded to 2612/2609/2605 m, while the retained table gives 2621/2617/2613 m. According to SoNiX's check, rotating the turret by about 90° did not change 670 MIL → 2609 m. These data do not determine a complete new ballistic model; the table is not replaced with arbitrary interpolation of six points.

SoNiX's checks take place on the training ground. A suitable training-ground height map is unavailable; local packages for other maps are not substituted. Game dispersion, hull position, actual sight settings and unknown elevations limit achievable first-shot accuracy. A 90–99% hit-rate claim requires a defined target size, conditions and a series of measured shots; no such series is available yet.

## What is checked automatically

Checks cover the bearing circle, distance symmetry and units; original table points, intermediate interpolation, monotonicity and SPH-2's ambiguous maximum; independent numerical comparison of elevation corrections; reconstruction of a synthetic rotation and its reversibility; direction and weight scaling; exact impacts at different heights; outliers and rejection of refinements without state changes. Separate checks cover OCR/manual input, the Russian locale, NaN/Inf/very large numbers, damaged terrain packages and cache boundaries.

Version 2.1 results are retained in `analysis/verification/v2.1/core`. Speed measurements describe local calculation performance; map-package checks establish their compatibility and height reading, not in-game hits.
