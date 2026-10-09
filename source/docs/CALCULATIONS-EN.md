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

### Profile provenance and conflicting sources

On October 8, 2026, all 71 L81 rows and 139 SPH-2 rows in this application were compared again with [Apollyon, commit `87e3cf3579c88499b16dfd1e29ed56c70bc0bc7b`](https://github.com/apollyon-sys/wardogs-calculator/blob/87e3cf3579c88499b16dfd1e29ed56c70bc0bc7b/data/weapons.json): the retained rows matched. The last change to `weapons.json` itself was commit `e5b801ee17bbd2bdf49f8f9ac474173c65573b62` on September 7. This identifies the source revision, not a verified WARDOGS patch. [t0ki](https://wardogs.t0ki.cn/credits.html) also describes the data as community measurements and credits Apollyon; agreement between these projects does not constitute an independent in-game trial.

The L81 source file contains **84 rows**, including **80 m → 950 MIL** and **697 m → 120 MIL**. However, the same file explicitly sets operating limits of **132–684 m** and **150–850 MIL**. The [author's documentation](https://github.com/apollyon-sys/wardogs-calculator/blob/87e3cf3579c88499b16dfd1e29ed56c70bc0bc7b/docs/features.md) distinguishes table coverage from the weapon's permitted operating range. Fire Control retains the 71 rows within those declared limits; the existence of a 950 MIL row does not extend this profile down to 80 m.

For SPH-2, the source configuration declares **780–2629 m / 20–1390 MIL**, although the table includes the endpoint **735 m / 1400 MIL**. This application retains both tables and the sight limit of 1400 MIL previously confirmed by SoNiX. Additional [Firepanda observations from September 22](https://github.com/Firepanda415/MZ-Wardogs/blob/main/docs/sph2-scale.md) show slightly different labels: 20 MIL → 1186 m and 610 MIL → 2630 m. Mechanical stops are estimated from the central reticle's position. The author explicitly distinguishes these sight readings from impact measurements and actual barrel angles. Those observations do not replace the active profile.

[MetaForge](https://metaforge.app/wardogs/map) publishes 80–684 m for L81, 600–2600 m for L52, and an L52 example at 2000 m: 251 MIL / 12.3 s or 1048 MIL / 33 s. The inspected pages provide neither a patch number nor a reproducible calibration method. SPH-2 names the vehicle, while L52 names the gun in that source; equivalence of their sight scales and profiles has not been established. MetaForge's map calculator labels L81 ammunition as `120x800mm`, while its separate [ammunition card](https://metaforge.app/wardogs/database/ammunition/81mm) lists `81mm` for the buildable mortar. These conflicts remain limits of external comparison; MetaForge's closed code and datasets have not been imported into the application.

**Sight MIL and physical launch angle are different inputs.** An SI milliradian is 0.001 radian ([BIPM](https://www.bipm.org/en/measurement-units/si-prefixes)), but the unit definition does not establish that the game's sight value equals the barrel angle. The main calculation uses the selected weapon's table. A `MIL / 1000` convention in a separate mathematical model must be identified explicitly as a model assumption.

## Elevation and platform tilt

The retained elevation correction model uses an assumed SPH-2 maximum range of **2629 m** and the two solutions of one approximate trajectory. Elevation is in meters: a positive difference means the target is above the gun. After correction and platform rotation, the result is converted back to the sight table. An unreachable target, unsupported angle or final value outside the sight table produces an error.

In this model, `R = 2629 m`, `x` is horizontal range, and `z` is target height relative to the gun. The angle follows `tan(θ) = (R ± √(R² − x² − 2Rz)) / x`: minus gives the low arc, plus the high arc. For the low arc, the code uses an equivalent expression that avoids subtracting nearly equal numbers. A negative radicand means the target is unreachable under the model. The arc profile is `z(x) = x·tan(θ) − x² / (2R·cos²(θ))`. This is a drag-free parabola, rather than recovered WARDOGS physics.

The model determines only the ratio `R = v² / g`. A range table alone therefore does not determine speed `v`, acceleration `g`, or flight time. The [NASA Glenn equations](https://www1.grc.nasa.gov/beginners-guide-to-aeronautics/ballistic-flight-equations/) for drag-free motion are `x = v·cos(θ)·t` and `z = v·sin(θ)·t − g·t²/2`. Calculating seconds requires an independently specified `g` or a measured flight-time profile; choosing Earth's gravity is an assumption, not an in-game measurement. Projectile mass alone does not resolve this ambiguity.

The open [Apollyon numerical solver](https://github.com/apollyon-sys/wardogs-calculator/blob/87e3cf3579c88499b16dfd1e29ed56c70bc0bc7b/js/workers/terrain-height-solver.js) also separates its estimated model from internal game constants. Its model families are fitted for elevation corrections and return `certified: false / estimated: true`; they do not provide verified flight times. Their fitted coefficients have not been adopted as WARDOGS speed or gravity.

To interpret an observed impact, the model permits a mathematical continuation down to 0 m / 0 MIL for the low trajectory and 0 m / π·500 MIL for the high trajectory. These values are needed for calibration; they do not extend the supported range of the final game sight setting.

Terrain packages contain a 2 m grid with 0.1 m height resolution; bilinear interpolation is used between four vertices. Connection checks SHA-256, mapId and geometry; reading checks decompressed block sizes and CRC. A negative Y scale preserves coordinate alignment. A point outside coverage is not assigned zero elevation. Selecting a map is mandatory and confirmation is required at launch; changing the map clears coordinates and corrections. The selected map's heights apply to both SPH-2 trajectories and the impact location. For the training ground and an unknown map, the user explicitly selects a mode without heights; L81 keeps its existing table without an elevation model. Local import atomically writes verified bytes to the persistent user data directory.

In the 2.11 workflow, the selected command and ranging appear together with automatic ground assessment; the detailed profile and personal measurements remain in additional tools. This does not change the retained sight table. Dispersion, wind, target movement and above-ground objects are not modeled. The experimental fitted-drag model from upstream 1.4.1 was not imported: limited in-game observations do not justify a promise of greater accuracy.

## Workflow and additional tools in 2.11

The main cycle uses a confirmed map and accepted gun/target positions: **Alt+X sets the gun → middle-click sets the target → fire → Alt+I records the actual impact → refined guidance for the original target**. Analysis and target changes are blocked until the map is confirmed and coordinate review is complete. Impact recording is optional; baseline guidance is available immediately.

The main window, mini card, sight and Alt+I snapshot use **one selected final command**: that arc's bearing and MIL with applicable corrections. Analysis of another arc in additional tools is explicitly labeled as baseline. The model arc and ground check concern the **baseline geometric calculation**, not measured flight after ranging. F4 explicitly changes the game arc; a ground recommendation does not switch it automatically.

Results distinguish **horizontal target metres**, **final MIL** and **approximate equivalent metres from the retained community table**. The latter is the inverse conversion of final MIL, not a measurement of the current game's RNG. Primary range does not change through ranging unless the target point itself changes.

Compact **RANGING** shows the latest accepted miss in metres, the **applied change** in bearing degrees and MIL, observation count and reset. This change is already included in the final command, rather than an extra offset to add again. Manual impact entry is under **Manual input and diagnostics → Impact corrections** in the main window. Target shifting, saved points, time, ground profiles and sources are in **Additional tools**. [Step-by-step guide](USAGE-EN.md).

Hints express left/right as bearing changes in **degrees**, and closer/farther as **MIL** changes. In the retained tables, increasing low-arc MIL moves farther; decreasing high-arc MIL moves farther. These directions refer to the selected arc's scale and do not make game MIL a physical angle.

### Manual target shifting

Left, Right, Closer and Farther buttons move the accepted target by **10/25/50/100 m**. The forward axis points from the gun to the current target. For its unit vector `u = (uₓ, uᵧ)`, the right axis is `(uᵧ, −uₓ)` and the new point is:

`target′ = target + [L·(uᵧ, −uₓ) + A·u] / 100`.

Positive `L` means right, positive `A` means farther; dividing by 100 converts meters to map units. Each click rebuilds the frame from the current target. Coincident gun and target points cannot define this frame and are rejected. The change follows the ordinary manual-input path and invalidates outdated OCR. It moves a point; it neither records an actual impact nor adjusts MIL directly.

### Recent and named positions and targets

Accepted positions and targets are saved automatically in a separate up-to-**64**-record list, <code>recent-fire-missions.json</code>, with exact coordinates, map, weapon and point kind; a user-supplied name is optional. The bounded recent list does not evict records from the named collection, <code>fire-missions.json</code>. Unaccepted OCR review and an empty or invalid pair do not count as successful points. The list shows only the current map and weapon; restoration is explicit and requires a confirmed map. Restoring a gun clears the previous target and calibration; a target follows the ordinary manual-input path. Map, weapon and corrections are not restored automatically on launch.

`%LOCALAPPDATA%\WardogsFireControl\fire-missions.json` is outside the application directory. Limits are **500** records, **120** UTF-16 code units per name and **1 MiB** per file. Names within the same map, weapon and point kind must be unique without regard to case. Writes are atomic after locking, rereading and validation; unknown formats, damage or inaccessible files produce an error instead of replacement with an empty list. This collection is separate from the last 12 targets of the current session.

### Flight time and user measurements

The interval before pressing Alt+I is not a flight-time measurement: the app does not detect the firing moment, and the delay includes player reaction, opening the map and selecting the impact. It does not create a seconds observation automatically.

Without a matching observation or an enabled physical assumption, flight time is **unknown**. Each observation stores range, time, uncertainty, weapon, arc, elevation difference, a Version / profile label and source. In the UI, ammunition and charge are identified through that user label: enter a different label after changing the game version, ammunition or charge. The application does not detect them in the game.

A profile applies only to the same weapon, arc, exact label and elevation difference; height comparison has a numerical tolerance of **10⁻⁶ m**, not a claim about terrain measurement accuracy. At an observed range, its time is used; between two points, time is linearly interpolated. There is no extrapolation beyond measured coverage. A new observation at the same range with the same profile identity replaces the previous one. `±s` is the absolute uncertainty entered by the user; interpolating these bounds is not a statistical confidence interval. A matching measured profile takes priority over model time.

Observations are stored in `%LOCALAPPDATA%\WardogsFireControl\flight-profiles.json`: **256** records in total, time up to **600 s** and file size up to **256 KiB**. Recording requires available nominal MIL and a known elevation difference. If the current map requires terrain but heights are missing or do not cover the points, recording is unavailable. An explicitly selected no-height mode assumes zero elevation difference.

Enable assumed speed and gravity is initially unchecked; later choices are saved in `planning.ini`. When enabled, SPH-2 uses the entered `g` and `v = √(2629·g)`, followed by `t = x / (v·cosθ)`. **9.80665 m/s²** is an Earth-gravity assumption, not measured game gravity. L81 additionally requires a positive user-entered speed: its initial value is missing, and an independent high vacuum arc is used. Neither model treats the displayed MIL as a physical barrel angle or changes the sight table. Model time is explicitly labeled an estimate, including when measured coverage does not include the current range.

### Ground profile and estimated arc

For an accepted SPH-2 target, known ground is checked automatically using the selected **baseline** arc. An intersection, no intersection at sampled points, unknown elevations and incomplete coverage are explicit separate states. The app can suggest another available arc when the assessment supports it; the player decides whether to change. Alt+I corrections change the command rather than produce a measured trajectory, so the check does not establish safe corrected flight.

SPH-2 displays the retained geometric parabola even without assuming `g`; that assumption is still needed for seconds. L81 displays ground without an arc until the user supplies a speed and `g` model. A measured total time alone does not determine the arc shape or timing at intermediate points.

Green is ground height relative to the gun position; orange is the estimated arc. Checks use a step of approximately **2 m** and include the analytic apex, with a limit of **8192** points. Interior points are checked for intersections. Ground contact at the gun and impact endpoints is expected and is not treated as an obstacle, but an arc endpoint below known ground produces a warning; only numerical roundoff is tolerated. Results report the first sampled crossing, minimum clearance and actual step. Missing heights remain gaps and the result is marked incomplete. No intersections at checked points does not guarantee clear flight between them.

UI positions are taken at ground level: actual barrel height is not entered separately. Buildings, roofs, bridges, trees and other above-ground objects are absent from the height package. Warnings describe an approximate arc and sampled ground, rather than detection of an actual in-game obstacle.

### Profile sources

Profiles and sources shows the retained limits and links for comparison. It does not automatically download another project's table or change the selected weapon. L81 850/950 MIL, SPH-2 1390/1400 MIL and SPH-2/L52 naming conflicts are explained above. A game update requires new table checks and measurements, rather than relabeling old seconds as a new version.

## Direct calculation and optional correction

At L81's 132 and 684 m boundaries, only `double` calculation error up to approximately 5·10⁻¹² m is tolerated. This prevents false rejection after subtracting decimal coordinates; real points outside the table remain rejected, and all 71 rows are preserved.

In 2.6.0, bearing and MIL use independent consistency weights. The local correction is normalized by the actual relative weights; low consistency alone does not return the estimate to zero. Spatial attenuation is applied separately. Replay of four retained field observations passed as a regression check on October 8; new in-game trials are still necessary to assess accuracy.

Since 2.5.0, the main SPH-2 path does not reconstruct platform tilt from two shots. It first calculates horizontal range, bearing, the game's table-based aiming for the selected trajectory, and an elevation correction if suitable heights are available. With no impacts, the local correction is zero. The public two-point calibration function remains for compatibility and separate tests; the ordinary UI and Alt+I do not call it.

Alt+I optionally records an actual impact for the current target. Each record captures the selected trajectory, commanded bearing and MIL, target and elevation when capture begins. The application does not observe the shot itself or the actual sight settings.

The snapshot retains the unrounded calculated command. Card values are rounded to 0.1° and a whole MIL, so a manual setting based on them can differ from the snapshot by up to 0.05°/0.5 MIL. When assessing small misses, the application cannot attribute this difference exclusively to the model.

Deviations are measured against the unchanged nominal impact calculation: `Δbearing = wrap(commanded bearing − nominal impact bearing)`; `ΔMIL = commanded MIL − nominal impact MIL for the same trajectory`. A bounded local estimate of these deviations is added to the next aiming solution. For the high trajectory, a short shot requires decreasing MIL; for the low trajectory, increasing MIL. Corrections from repeated shots are not summed: each snapshot already contains the commanded aiming solution.

One impact does not determine the gun's three-dimensional tilt. Local mode leaves the original matrix unchanged, uses only observations of the same trajectory within **50 m** of the target and does not create a global rotation or transfer a global offset to another part of the map. Influence decreases smoothly toward the radius boundary and is zero beyond it. Since 2.5.1, the first geometrically valid impact applies the full measured correction at the same target; conflicting nearby observations weaken the estimate. Independent ±3°/±50 MIL limits were removed: they rejected a confirmed 236.70 m miss at a range of 2210.33 m. Instead, the target-to-impact distance is checked: it must be no greater than **20% of horizontal range and 400 m**, simultaneously. The commanded setting is also checked by the distance between its horizontal aiming vector and the current calculation, using the same limit. The final command must remain within the available scale and must not depart from the original command by more than twice the geometric limit. The double allowance accommodates nonlinear table-based aiming while limiting the accumulated displacement. These are application rules, not recovered game-dispersion parameters. A recording error preserves the entire previous history and aiming solution. Reset clears refinements and immediately restores the original direct calculation.

History is limited to 256 observations. The consistency score from 0 to 1 is not a hit probability. Software regressions check correction signs, a repeated shot with an already corrected command, isolation of trajectories and distant targets, rejection without state changes, and memory bounds.

SPH-2's main distance in metres is the horizontal distance to the accepted target, identical for both arcs. The main window and mini card separately show final MIL and inverse table range, marked as approximate. Elevation and local corrections can make table metres differ from target range. In the supplied game sight, 660/670/680 MIL corresponded to 2612/2609/2605 m, while the retained table gives 2621/2617/2613 m. According to SoNiX's check, rotating the turret by about 90° did not change 670 MIL → 2609 m. These data do not determine a complete new ballistic model; the table is not replaced with arbitrary interpolation of six points. A task around 2200 m illustrates the workflow rather than a measured new shot.

SoNiX's checks take place on the training ground. A suitable training-ground height map is unavailable; local packages for other maps are not substituted. Game dispersion, hull position, actual sight settings and unknown elevations limit achievable first-shot accuracy. A 90–99% hit-rate claim requires a defined target size, conditions and a series of measured shots; no such series is available yet.

## What is checked automatically

Checks cover the bearing circle, distance symmetry and units; original table points, intermediate interpolation, monotonicity and SPH-2's ambiguous maximum; independent numerical comparison of elevation corrections; reconstruction of a synthetic rotation and its reversibility; direction and weight scaling; exact impacts at different heights; outliers and rejection of refinements without state changes. Separate checks cover OCR/manual input, the Russian locale, NaN/Inf/very large numbers, damaged terrain packages and cache boundaries.

Version 2.1 results are retained in `analysis/verification/v2.1/core`. Speed measurements describe local calculation performance; map-package checks establish their compatibility and height reading, not in-game hits.
