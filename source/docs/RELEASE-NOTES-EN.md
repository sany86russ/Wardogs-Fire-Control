# WARDOGS Fire Control — release notes

[Русский](RELEASE-NOTES-RU.md) · [Project overview](../../README.en.md) · [Quick start](QUICKSTART-EN.md)

## 2.10.0 — map reading and diagnostic history (candidate)

The published stable release remains 2.8.0. The changes below describe the current source and candidate build under validation.

- Complete labeled X/Y are searched in a bounded cursor neighborhood within the game client. Original rectangles remain hints; an unlabeled number is not accepted by position.
- The first middle-button screenshot waits for the marker to appear, 250 ms by default. Client geometry is pinned at the click; a geometry change during that delay cancels capture. Automatic acceptance needs two matching trustworthy pairs from separate screenshots, with at most four frames per request. Changes to point, window, geometry or epoch invalidate the old request.
- Incomplete and competing labels lead to review or rejection; there is no silent OCR-engine fallback. A target can be read through M → right-click → Mark Coordinates → Alt+T with automatic detection for additional captures enabled.
- Up to 32 managed archives in `latest.archive`, plus `latest.log` and `latest.previous.log`, each up to 4 MiB: up to 128 MiB of archives and 8 MiB for current and previous files. Prior bytes are archived before replacement; a failure may retain an additional recovery file of up to 4 MiB. Errors are reported explicitly; unknown files are outside the managed limit and are not deleted.
- The candidate includes planning prepared in 2.9. The portable package preserves the user profile; launching requires the whole set, including Qt, models, TLS and the updater helper.
- A failed or cancelled Alt+I restores the unchanged previous SPH-2 target's calculation when no coordinate review is pending; cancellation creates no impact or correction.
- Five real X/Y pairs and scale/position transformations are retained for regressions. New in-game firing trials have not been performed. Tables and the physical model are unchanged; first-shot accuracy and any hit percentage are not guaranteed.

## 2.9.0 — planning (included in the 2.10.0 candidate)

- A Planning window with RU/EN panels for profiles and sources, flight and terrain, named points and personal flight-time observations.
- Left/right/drop/add corrections by 10/25/50/100 m in the gun → target frame; full recalculation and removal of stale aiming values outside the range.
- Gun positions and targets saved by map and weapon with exact coordinates. Restoration requires a confirmed map; restoring the gun clears the target and calibration.
- Local timing observations with game/profile version, source and uncertainty, interpolated without extrapolation. A separate assumed gravity/speed model is clearly marked as an estimate.
- Ground and estimated-arc plots, ground-intersection warnings and incomplete-coverage states. Buildings, bridges, trees and actual muzzle height are not checked.
- Retained operating tables and limits. The L81 80/132 m and SPH-2/L52 discrepancies are documented; unverified data does not replace aiming commands.
- New local data survives application updates.

## 2.8.0 — GitHub release updates

- Background stable-release checks at startup, configurable in Settings. Manual checks are available in the header.
- A new-version banner offers Update, What's new and Later. Download and installation require the user's action.
- The complete ZIP is verified against its declared size and mandatory GitHub SHA-256 digest. The manifest, every file and the Windows x64 executable version are then checked.
- The complete portable package is updated, including Qt DLLs and resources. An external helper waits for the app to exit, retains backups and rolls back on replacement/start failure.
- Settings and imported terrain in the Windows profile are preserved. Current session data is cleared on restart.
- Diagnostic launches do not check the network. OCR and calculations stay local and work offline.
- The first upgrade from 2.7.0 is manual; see the [update guide](../../docs/UPDATES-EN.md).

## 2.7.0 — Russian and English interface

- Russian is the default for a new profile. The **RU / EN** selector beside the version changes the interface immediately and saves the preference for the next launch.
- Main controls, settings tabs, guides, tooltips, OCR review, result cards, the sight overlay, tray actions and user messages are available in both languages.
- Switching language preserves coordinates, map confirmation, the selected arc, calculation and pending OCR review.
- English quick-start, calculation, architecture and game-interaction guides accompany the Russian documentation. Translation catalogs are embedded in the EXE.
- Localization checks cover catalogs, placeholders, language changes and UI messages. Prepared screenshots show both interface languages.

Language selection does not change the game's language, shortcuts, recognized text, formulas, weapon ranges or terrain limits. **MIL** remains the game's aiming scale; coordinate input uses a decimal point.

## 2.6.0 — calculations, persistence and diagnostics

- L81 accepts exact 132/684 m boundaries with tolerance only for machine rounding. All 71 table rows and actual limits are preserved.
- North is normalized to 0° in calculations, the direction plot and copied output. The sight rejects NaN/Infinity and safely handles large numeric values.
- History restores original coordinate precision and retains its limit of 12 targets.
- Fixed edge-pixel mixing during OCR image enlargement. Diagnostics distinguish X/Y, recognition passes and rejection reasons without relaxing confidence thresholds.
- A new launch preserves the previous log in `latest.previous.log`. Completed calculations flush to disk; rotation failures preserve existing files.
- Copying checks the Windows clipboard write and reports failure. Multiline footer placement was corrected.
- Bearing and MIL use independent correction-consistency weights; spatial attenuation is applied separately.
- Map selection is required before calculation. The last selection is saved and needs confirmation in a new session. Changing the map clears the gun, target, history and corrections.
- Local height import checks SHA-256 and mapId, then installs atomically into the persistent user directory. Heights apply to both SPH-2 arcs and impacts; training-ground mode uses no heights.
- Build preparation uses Idle priority and one process. Root `Build.ps1` prevents replacing App with an untested `-SkipTests` build.

## 2.5.1 — geometric limits for local correction

- Independent ±3°/±50 MIL limits were replaced with checks of miss geometry, the commanded setting and the available aiming scale.
- An impact must be within both 20% of horizontal range and 400 m of the target. Command consistency is checked; the final command is bounded by twice the geometric allowance.
- The first valid impact fully refines the same target. Conflicting observations reduce consistency; influence smoothly reaches zero at 50 m and is not transferred between arcs.
- Status displays the miss in meters and applied correction. **Alt+I** retains the current target.

## 2.5.0 — direct SPH-2 calculation

- Both arcs are available after setting the gun and target without two mandatory test shots.
- Optional **Alt+I** records an actual miss for the current target. The local correction does not reconstruct global vehicle tilt or modify distant targets.
- Repeated observations measure deviation from the unchanged nominal calculation. An already applied correction is not added again.
- Reset restores the direct solution; moving the gun and changing the map clear dependent observations.
- SPH-2 cards display horizontal range in meters. Approximate table-equivalent range is separate from current in-game RNG.

## 2.4.x — shared trajectory context

- F4, the sight, cards and impact recording use the same effective arc.
- Impact context captures the target, arc and commanded bearing/MIL when reading begins. Changing the target or map, or an unresolved gun review, cancels stale context.
- Recording errors preserve previous observations and aiming. Both SPH-2 tables are retained.
- Fixed Alt+X row detection on bright game backgrounds; complete drafts and confidence thresholds remain required.

## 2.3.x — reliable coordinate reading

- Alt+X always detects the active gun draft automatically; a saved custom region does not replace the primary source.
- Middle-button readiness depends on enabled integration, the mouse setting and an accepted gun. Showing the main window, mini card or returning through Alt+C does not disable it.
- X/Y fields come from one bounded screenshot near the event point. Old chat is not used as the middle-button source.
- Detection accounts for different channel-label widths, small fonts, grid lines, cursor fragments, ping brackets and nearby elements. Clipped digits remain rejected.
- The game caption is normalized for whitespace. Old active aiming is hidden during new reading; ambiguous results require review.
- The queue retains only the latest pending target. Bounded retries require the same point and window context.
- Initial import of an older profile performs a one-time quick-workflow migration. Later explicit standalone-mode choices are retained.

## Checks and limits

Public source includes CTest checks for mathematics, parsers, OCR, state, localization, settings, hotkeys, mouse, capture and damaged packages. New build results are available in GitHub Actions. Private historical logs and original user screenshots are not published.

Test scenes and software regressions do not establish recognition of every live HUD, in-match hit accuracy or anti-cheat approval. The application does not observe the shot itself, actual sight settings or hull tilt. No validated training-ground height package is available; another map's data is not substituted.

Community `.wdt` data is excluded from the public source and portable ZIP. Original, library and model licenses are retained. See [calculations and mathematical limits](CALCULATIONS-EN.md), [game interaction](ANTICHEAT-EN.md) and [terrain provenance](../TERRAIN_DATA_NOTICE.md).
