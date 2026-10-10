# WARDOGS Fire Control — release notes

[Русский](RELEASE-NOTES-RU.md) · [Project overview](../../README.en.md) · [Quick start](QUICKSTART-EN.md)

## 2.11.1 — candidate: complete battle diagnostics · October 10, 2026

- Separate accepted gun/target events with exact coordinates, map, weapon and input source. OCR results are distinct from player acceptance; reviews, confirmations and cancellations are explicit.
- Weapon/arc changes and correction resets/rejections are recorded. SPH-2 calculation logs are no longer skipped when an arc is unavailable.
- Main actions flush immediately; other events flush once per second. A recording failure after startup displays a notification; a diagnostic run fails if its log is unavailable.
- Regression checks read events from an open log and verify closed sessions, failures and separation of user/test data. Tables and coordinate acceptance rules are preserved. This section describes the prepared fix; publication and installation are separate actions.

## 2.11.0 — unified fire-control workflow and ranging · October 10, 2026

Changelog date: **October 10, 2026, Moscow time**. Version 2.11 brings the initial calculation, selected aiming command, observed impact and command refinement into one workflow. It includes the additional tools developed in **2.9** and coordinate-reading/logging changes developed in **2.10**, after the published 2.8 release. Historical 2.9/2.10 entries are retained below; their “candidate” wording describes those development stages.

**The L81 and SPH-2 tables have not been recalibrated.** This update changes the workflow, interface, data persistence and input safeguards. It does not promise a first-shot hit, a hit percentage or agreement between calculated range and the current game's RNG. Check the [release page](https://github.com/sany86russ/Wardogs-Fire-Control/releases) for the published ZIP and the status of a particular build.

### What changes for players

| Task | In the published 2.8 release | In 2.11 |
|---|---|---|
| Refine SPH-2 guidance from an actual miss | Ranging was already available, with separate actions and status reading | The latest accepted miss, applied change and reset are beside the result |
| Read target coordinates | Reading relied on regions near the input event | Complete X/Y labels are searched in a bounded region; automatic acceptance needs agreement from two separate screenshots |
| Return to a useful position | Current-session history and manual entry | Separate automatic recent points and named gun positions/targets |
| Examine terrain and flight time | The primary calculation and local heights | Additional tools with sources, personal observations, estimates and explicit limits |
| Find earlier diagnostics | Current and previous logs | A bounded archive of earlier logs, preserving previous data before replacement |

### The quick SPH-2 routine

1. **Select and confirm the current map.** **Confirm map and enter game** explicitly confirms the selection and enters game mode; once confirmed, the button reads **To game**. If no map is selected, select one first. Missing required height data blocks entry. A saved map name must be confirmed in a new session.
2. **Set the gun:** open the game's map and use **M → right-click the gun → Mark Coordinates → Alt+X**. The active draft must contain a complete coordinate pair; the primary workflow does not require sending it to chat.
3. **Middle-click the target on the map.** Once coordinates are accepted, select the desired SPH-2 arc; **F4** switches the selection. The initial calculation is available without mandatory test shots.
4. **Aim using the final bearing and MIL**, then fire in the game. The app does not observe the shot or set game controls for you.
5. **For optional ranging, point at the actual impact on the map and press Alt+I.** An accepted observation refines the selected command for the original target. The target stays in place.
6. **Use the new final bearing and MIL.** You can add another observation after the next shot at the same target. Set the gun again after moving it; reset corrections manually if the hull orientation changes at the same position.

These are the usual default shortcuts. After changing settings or a Windows registration conflict, use the shortcuts shown by the app.

### Ranging beside the result

The compact **RANGING** block in the main window and mini card shows the latest accepted miss in metres, its lateral and longitudinal components, accepted observation count and reset action. **Applied change** shows changes in bearing, MIL and approximate table-equivalent range.

**The change is already included in the final command. Do not add it to the displayed bearing and MIL again.** Reset returns to the baseline calculation. If observations disagree, the app reports limited correction accuracy.

Left/right means a change in **bearing degrees**. Nearer/farther refers to **MIL**, with the direction depending on the arc:

| To change table range | SPH-2 low arc | SPH-2 high arc |
|---|---|---|
| Farther | Increase MIL | Decrease MIL |
| Nearer | Decrease MIL | Increase MIL |

These signs explain the hint; use the resulting final command to aim. They do not mean “change the bearing by the same number of metres” and do not determine vehicle tilt.

Corrections remain local and specific to an arc: observation influence fades to zero **50 m** from the recorded target. Existing geometry and command checks remain in force, including an impact limit of **both 20% of horizontal range and 400 m**. An incompatible impact does not replace the previous guidance. Repeated observations do not add the same correction indefinitely.

Manual impact entry is available in the main window under **Manual input and diagnostics → Impact corrections**. It records the same type of observation and also retains the original target.

### One command and complete cards

The main window, mini card, sight and Alt+I context use **one selected final command**. An impact observation refers to the target, arc and displayed settings captured when reading begins. Changing the map/target/arc or leaving coordinate review unresolved prevents a stale observation from being applied to a new task.

SPH-2 cards display the complete set of labeled values in RU and EN:

- **Target range:** horizontal metres between the gun and target coordinates.
- **Bearing:** final aiming direction in degrees.
- **Set:** final MIL for the selected arc, including the active local correction.
- **Table range ≈:** approximate range obtained by converting the final MIL back through the community table.

Labels and numeric values have separate readable elements in both the main window and mini card. Target metres and table-equivalent metres describe different quantities and may differ with a height or local correction. **Table range ≈ is not a verified RNG value from the game's sight.**

The alternative arc in additional-tools analysis is explicitly a **baseline calculation without local ranging corrections**. It does not replace the selected final command.

### Coordinate reading: labels, separate screenshots and uncertain input

The OCR changes developed in 2.10 are included. Complete labeled **X/Y** are searched in a bounded cursor neighborhood within the game client. Original rectangles serve as search hints. An unlabeled number is not accepted as X or Y solely because of its position.

After middle-click, the app waits for the marker to appear: **250 ms** by default. The first screenshot in this workflow has an **80 ms** minimum delay even when the setting is zero. This is a wait for the game's interface, not a promised total recognition time.

Automatic acceptance needs **two matching trustworthy pairs from separate screenshots**. A request is limited to **four frames**; reprocessing one frame does not count as another observation. Changes to the point, window, client geometry or context invalidate an old request. The screen is not scanned continuously.

Clipped digits, labels covered by the cursor/map elements or competing pairs may require review or result in rejection. Hidden or incomplete coordinates are not invented to produce a result. The selected OCR engine is not silently replaced by another.

If reading fails:

- For the gun, repeat **Mark Coordinates → Alt+X** with a complete pair in the active draft.
- For the target, repeat middle-click and leave the cursor near both visible labels. The fallback is **M → right-click the target → Mark Coordinates → Alt+T** with automatic detection for additional captures enabled.
- Correct and accept the values in coordinate review, or use explicit manual coordinate entry.
- For an impact, repeat Alt+I at the actual point before changing the target/arc, or enter the impact manually.

During new reading or unresolved review, old values are not presented as a new solution. A failed or cancelled Alt+I restores the unchanged previous SPH-2 target's calculation when no coordinate review is pending; cancelling creates neither an impact nor a correction.

### Additional tools and saved points

The features developed in 2.9 are included under **Additional tools**. Opening them is optional for the primary routine.

- **Manual target relocation:** left/right/nearer/farther by **10/25/50/100 m** in the gun → target frame. The buttons move the target itself, create a new task and recalculate guidance. This is a separate action from ranging against an actual miss at the original target. Outside the operating range, previous values do not remain an active command.
- **Automatic recent points:** up to **64** accepted gun positions and targets with exact coordinates, map, weapon and record type. Naming is optional; unaccepted OCR does not become a saved position.
- **Named collection:** up to **500** separately saved positions and targets. Automatic history does not evict its records.
- **Explicit restoration:** confirm the correct map, select the matching weapon and apply the chosen point. On startup, the saved map name needs confirmation, and the gun/target must be set explicitly; a saved record does not restore old ranging corrections. Set the target again after restoring a gun position.
- **Personal timing observations, ground profiles and sources:** detailed analysis, data provenance and uncertainty are available.

New local data uses four separate files in the Windows profile:

| File | Contents |
|---|---|
| <code>recent-fire-missions.json</code> | Up to 64 recently accepted points |
| <code>fire-missions.json</code> | Up to 500 named positions and targets |
| <code>flight-profiles.json</code> | Personal flight-time observations and their source information |
| <code>planning.ini</code> | Additional-tools settings and assumed-model parameters |

Stored coordinate precision is not limited to the rounded card label. A damaged or unsupported JSON store is reported as an error and is not automatically replaced with empty data.

### Ground and flight time: interpreting estimates

After an SPH-2 target is accepted, the app automatically assesses ground along the **selected arc's baseline model**. Results distinguish an estimated intersection, no intersections found at sampled points, unknown heights and incomplete coverage. Missing data is not treated as a clear path.

If another available arc looks preferable under this assessment, the app suggests checking it. **It does not switch arcs automatically.** The player makes the decision.

Assessment uses matching local height data; another map's data is not substituted. Training-ground/unknown-map use has an explicitly selected no-heights mode that assumes equal gun and target heights.

**Buildings, roofs, bridges, trees and actual muzzle height are not modeled.** Ranging changes the aiming command but does not measure the projectile's actual trajectory. The baseline-arc plot is not the post-Alt+I flight path, and finding no intersection does not guarantee a clear shot.

Personal timing observations record the game version/profile, source and uncertainty. Time is interpolated only within their range, without extrapolation. The separate model with assumed speed and gravity remains **disabled by default** and is labeled as an estimate.

Alt+I records the coordinates of an observed impact. The delay between a key press and reading **is not flight time**, and the app does not derive flight time from it. Measuring flight time also does not verify the shape of the arc.

### Diagnostics and log preservation

In addition to current <code>latest.log</code> and previous <code>latest.previous.log</code>, up to **32 managed archives** are retained in <code>latest.archive</code>. Each file is limited to **4 MiB**: up to **128 MiB** of archives plus up to **8 MiB** of current and previous logs.

Previous data is archived before replacement. Write/rotation errors are reported explicitly; a failure may retain an additional recovery file of up to 4 MiB. Unrelated files are outside the managed limit and are not deleted. This is a bounded archive rather than permanent history.

OCR and calculations remain local. Logs do not contain screenshots, but may contain game coordinates and local paths. Review an attachment before posting it publicly in Issues.

### Upgrading to 2.11

- **From the published 2.8.0 release:** once the stable 2.11 release is available, use **Check for updates → What's new / Update**. A background startup check can also run when enabled. Download and installation require a user action.
- **From 2.7.0 or earlier, for a first installation or a development build:** download the **complete <code>WardogsFireControl-v2.11.0-win-x64.zip</code>** from Assets and extract the whole package into a separate folder. A single EXE does not replace Qt, OCR models, TLS, resources, the manifest and updater helper.
- Finish the current calculation before updating and avoid running multiple copies from one folder. Current coordinates, session history and ranging corrections are cleared after restart.
- Settings, imported local heights, logs and the four additional-tools files listed above stay in the Windows profile. Saved points must be applied explicitly; they do not automatically restore a firing session.
- Built-in updating verifies the ZIP's size and mandatory SHA-256 from GitHub metadata, the manifest, file sizes/hashes and the Windows x64 EXE version. The helper updates the complete package after the app closes and provides rollback for replacement/start failures. This does not verify a game hit or separately validate a publisher signature.

For manual recovery, extract the appropriate complete ZIP into a new folder. Do not copy old DLLs over the new package. See the [update guide](../../docs/UPDATES-EN.md) and [quick start](QUICKSTART-EN.md).

### What stays unchanged

Operating tables, weapon ranges and mathematical boundaries are retained. Unverified data does not replace L81/SPH-2 tables, and source discrepancies are not hidden. The app does not observe hull tilt, the actual in-game sight setting or the shot itself.

Software regressions and interface screenshots exercise bounded scenarios; they do not establish recognition of every live HUD, firing quality in the current match or anti-cheat approval. Account for coordinate visibility, the map, vehicle position and observed impacts.

[Full workflow](USAGE-EN.md) · [Calculations and limitations](CALCULATIONS-EN.md) · [Game interaction](ANTICHEAT-EN.md) · [Terrain provenance](../TERRAIN_DATA_NOTICE.md)

## 2.10.0 — map reading and diagnostic history (candidate)

Changes in this historical section were introduced in the 2.10.0 candidate and retained in the current 2.11.0 candidate. This section does not claim a published stable 2.10.0 release.

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
