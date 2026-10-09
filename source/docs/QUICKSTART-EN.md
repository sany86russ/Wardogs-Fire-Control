# WARDOGS Fire Control 2.11.0 — candidate quick start

A portable assistant for L81 and SPH-2: range, bearing and table aiming from two points. The main cycle is map → Alt+X: gun → middle-click: target → fire → Alt+I: impact → refined SPH-2 guidance. Additional tools provide manual target shifting, saved points, timing observations, ground profiles and sources. Recognition and calculations run locally.

This guide covers the **2.11.0 candidate**: one cycle for map, gun, target and ranging; a shared final command, automatic ground assessment and retained accepted points. The 2.10 OCR and log archives remain. The published stable ZIP remains **2.8.0**; downloading that release does not provide the candidate features.

## Launch and first calculation

Extract the entire portable ZIP into a separate folder and double-click **Запустить.cmd**, **Start.cmd** or **WarDogsDistanceCalculator.exe** there. Windows 10/11 x64; installation and administrator privileges are not required. Keep `models`, `platforms`, `tls`, `Launch.ps1`, `Update.ps1`, the manifest and DLLs together; copying only the EXE is insufficient. Settings and imported terrain remain in the Windows profile when the complete package is replaced.

Russian is the default interface language. Select **EN** in the header beside the version to switch to English. You can switch back to **RU** at any time; the preference is saved. See the [language guide](LOCALIZATION.md).

1. In **Game map · required**, select and explicitly confirm the current map. **Confirm map and enter game** confirms the selected map and enters game mode; once confirmed, the button reads **To game**. Select a map first when none is chosen; missing required elevations block entry. After restarting, the saved selection must be confirmed again.
2. Open the full WARDOGS map with **M**. Right-click the gun position and select **Mark Coordinates**. The pair appears in the chat input; you do not need to send a message.
3. Press **Alt+X**. The application finds the active draft in the upper-left part of the window, independently of any saved custom region. If it reads a complete pair with sufficient confidence, it sets the gun position, hides the main window and shows the mini card.
4. Point at the target on the map and press the **middle mouse button**. The application reads the separate X and Y labels near the cursor and displays the calculation. The button still places a marker in the game; this target does not need new text in chat.
5. For SPH-2, set the selected bearing and **final MIL**, then fire. Optionally point at the actual impact on the map and press **Alt+I** before changing the target or arc. The command is refined for the original target; middle-clicking the impact instead sets a new target.

Use **Tab** to switch chat channels: team, local, vehicle or all. The reading region adjusts automatically to the current channel label width. Channel text comes from the game and follows the game's language.

You do not need to select an OCR region, press Start game or confirm an ordinary successful capture. For the next target, use the middle mouse button. It works after the gun position is accepted, with either the main window or the mini card visible. Returning with **Alt+C**, the tray or another launch opens the calculator and keeps target capture ready. When you move the gun, repeat the map selection and gun coordinate capture. To stop capture entirely, select the standalone calculator in settings, disable the middle mouse button or close the application.

Shortcuts can change after a conflict with another application. The current combinations appear in the hint and settings; use them instead of the defaults shown here.

## When automatic reading needs review

For the gun, the application checks for an active draft, one complete X/Y pair, agreement between two passes, character confidence and consistency with visible characters. Sent chat history does not replace a damaged draft. The middle mouse button reads separate X and Y fields around the clicked point without consulting chat.

The first middle-button screenshot waits for the marker to appear: **250 ms** by default. Automatic acceptance needs two identical trustworthy complete X/Y pairs from **two separate screenshots** with unchanged cursor, window and geometry. A request is limited to **four frames**; processing one screenshot again does not count as a second observation. Conflicts, incomplete labels or lack of two confirmations lead to review or rejection. If you mark a new point during OCR, only the latest capture remains queued; an outdated result will not be applied. The screen is not read continuously.

Weak, clipped or ambiguous text opens a review card. Return with **Alt+C**, select a pair, correct it if necessary and press **Apply verified coordinates**. Once the gun is accepted, the middle mouse button is ready regardless of how the position was entered or confirmed. During a new reading and until an error is resolved, the previous aiming solution is hidden. An unaccepted new gun capture also blocks new targets, so the calculation cannot use the old gun position. An empty capture is not presented as a successful calculation. **Reject** explicitly restores the previous calculation; manual input cancels the pending review.

## If coordinates are not read

- **Gun:** select Mark Coordinates again, check that the pair is visible in the active chat input, then repeat Alt+X.
- **Target:** keep the cursor near the labels of the selected point and click the middle mouse button again. Switching windows or moving the cursor stops the previous automatic attempt.
- **Target fallback through chat:** **M → right-click target → Mark Coordinates → Alt+T**. A full pair must be visible in the active draft; no message needs to be sent. Enable **Auto-detect for additional captures** under **Settings → Advanced**. If disabled, Alt+T reads your saved custom region.
- If the game is minimized, its window is not recognized or the region extends beyond one monitor, open the game on a single monitor and try again.
- For an unusual HUD, use **Manual input and diagnostics** or the advanced recognition settings.

Automatic chat detection is limited to the upper-left part of the client window. It accounts for the window's physical position and size, including negative coordinates on another monitor. Map fields are searched in a bounded cursor neighborhood within the game client; original rectangles guide the search, while a complete X or Y label identifies the axis. Unlabeled numbers, clipped pairs and competing labels are not accepted automatically. RapidOCR is the main engine; an error does not silently switch to Windows OCR. These checks do not guarantee recognition of every HUD and resolution.

## Manual fallback

Diagnostics are stored in `%LOCALAPPDATA%\WardogsFireControl\logs`: `latest.log` contains the current session, `latest.previous.log` the previous log, and `latest.archive` up to **32** older `session-<number>.log` files. Each is limited to **4 MiB**: up to **128 MiB** of archives and **8 MiB** for current and previous files. A filesystem failure may retain an additional recovery file of up to 4 MiB; unrelated files are outside this limit and are not deleted. The oldest managed archives are replaced when the limit is reached. Completed readings and calculations are written immediately. Logs record X/Y recognition details, separate-frame agreement and rejection reasons, but no screenshots: the log alone cannot reconstruct an obscured digit or its background. Archiving does not recover sessions already lost.

Automatic gun and map reading uses local RapidOCR. The OCR for custom region selection in advanced settings applies to manual capture; a saved Windows OCR choice does not change the quick workflow.

Expand **Manual input and diagnostics**. Enter the gun and target, for example `x98.48, y110.35`, and calculate the target. **Paste** reads a pair from the clipboard only when clicked; the application does not monitor the clipboard in the background. In the game, you can select and copy the draft with Ctrl+A/C, then paste it into the calculator. **Copy calculation** places the result on the clipboard; history keeps the last 12 targets of the current session.

**Alt+R** lets you select a custom line as an additional source. The selected region is tied to the monitor's physical size; select it again after changing resolution. Saving it does not redirect Alt+X to the custom region or disable the middle mouse button. The main workflow does not require this setting.

## Default shortcuts

| Action | Shortcut |
| --- | --- |
| Read gun position from the active draft | Alt+X |
| Read target at the map cursor | Middle mouse button |
| Open the main window while keeping middle-button capture ready | Alt+C |
| Select a custom region | Alt+R |
| Fallback target reading from chat or a custom region | Alt+T |
| Temporarily select a line and read a target | Alt+V |
| Read an impact for SPH-2 corrections | Alt+I |
| Switch the SPH-2 aiming trajectory | F4 |

Windows reserves registered shortcuts. If a combination is occupied, the application chooses an available alternative with extra Ctrl/Shift modifiers, displays it and saves it after the complete set is registered successfully. If alternatives run out or Windows reports another error, an explicit error appears; an incomplete set is not saved. F12 and combinations using Win are not used.

## Settings and portability

**Settings → General** contains the workflow and the delay after placing a map marker. **Advanced** contains the standalone calculator, middle mouse button, shortcuts, OCR and custom region. Selecting **Standalone calculator · manual input only** disables game features and is saved between launches.

The default middle-button delay is **250 ms**. In the candidate, the first frame waits at least **80 ms**, even if the setting is zero; later frames are captured separately after a bounded wait. This waits for the label to appear rather than promising a total OCR duration.

Profile: `%LOCALAPPDATA%\WardogsFireControl\settings.ini`. If it does not exist yet, the previous `%LOCALAPPDATA%\WarDogsDistanceCalculatorCpp\settings.ini` is read without modifying the old file. Profiles without the new-workflow marker are migrated to quick mode: game features, middle mouse capture and automatic detection are enabled; gun capture uses Alt+X and return uses Alt+C, accounting for internal conflicts. After the new profile is saved, any subsequent choice of standalone mode is retained.

Settings are written in Unicode, preserving additional sections and keys. A damaged ANSI version of the standard pattern is repaired automatically. For a rejected custom pattern, use **Restore standard WARDOGS pattern**; complex expressions are not accepted. Windows OCR remains an optional source; automatic reading of the two map fields uses the main bundled model.

## Additional tools

Return to the main window with **Alt+C** and click **Additional tools** below the quick-workflow hint. First confirm the map, accept the gun and target, and finish reviewing uncertain coordinates. Calculation actions are unavailable while a new gun capture or target review is unresolved.

### 1. Manual target shifting

In **Flight and terrain**, select a **10, 25, 50 or 100 m** step and click **Left / Right / Closer / Farther**. Directions are relative to gun-to-target: right does not mean east on the map. A click immediately moves the target and recalculates aiming; repeated clicks apply to the new point. This is a spotter command that changes the target, rather than an Alt+I impact record. Outdated OCR results are invalidated.

### 2. Named positions and targets

Accepted positions and targets are automatically retained in a separate recent list of up to **64** records. Names are optional. In **Positions and targets**, name a point and save it in the named collection, rename it, delete it or explicitly apply a selected record. Restoration requires a confirmed matching map and weapon; restoring the gun clears the previous target and corrections. Map and gun are not restored automatically on launch.

The separate named collection holds up to **500** points, with names up to **120** UTF-16 code units. These persistent records are separate from the last 12 targets of the current session. Names within the same map, weapon and point kind must not duplicate each other, even with different capitalization.

### 3. Measured flight times

Alt+I does not measure flight time: the delay includes player reaction and map interaction. Seconds require a separate personal measurement; the assumed model is disabled by default.

In **Time measurements**, enter **Version / profile**, **Source**, time from firing to impact and **Measurement uncertainty ±s**, then click **Record time for current target**. An example label is `0.1.2 / 155 HE / normal charge`; it must describe your own conditions. Enter a different label after changing the patch, ammunition or charge. The application does not detect them or start a shot stopwatch.

Recording requires available nominal MIL and a known elevation difference. Up to **256** observations with time up to **600 s** can be stored. Recording the same range and profile again replaces the previous observation. You can delete a selected observation. Uncertainty must not exceed time.

**Flight and terrain** shows a user observation or interpolation between observations of the same weapon, arc, elevation difference and label. The profile supplies no time outside measured coverage. With no observations and the model assumption disabled, the result is **Flight time unknown**. L81 uses the high arc; for SPH-2, additional tools distinguish the selected final command from baseline analysis of another arc. F4 explicitly changes game guidance.

### 4. Flight and terrain profile

After an SPH-2 target is accepted, ground is assessed automatically on the selected baseline arc. Intersection, no detected crossing at sampled points, unknown elevations and incomplete coverage are separate states. Suggesting another arc does not switch it; the plot does not check actual corrected flight after Alt+I.

In **Flight and terrain**, green is ground and orange is the estimated arc. SPH-2 uses the retained geometric model; L81 displays available ground without an arc unless an additional model is supplied. Missing heights remain gaps. A crossing warning gives the approximate range of the first sampled model/ground intersection. Even a complete graph without crossings does not guarantee clear flight: checks sample points about every 2 m.

For estimated time, explicitly select **Enable assumed speed and gravity**. It is initially unchecked; your later choice is saved. **Assumed g, m/s²** initially reads **9.80665**, an Earth-gravity assumption rather than measured WARDOGS physics. SPH-2 derives speed as `v = √(2629·g)`; L81 additionally requires **L81 speed, m/s**, initially missing. Matching user measurements take priority; other seconds are explicitly labeled model estimates. Neither speed nor `g` changes the sight table's MIL.

Install a height package for the selected map to check ground. On the training ground or an unknown map, explicitly selecting no-height mode assumes equal endpoint elevations and does not check ground. Buildings, roofs, bridges, trees and actual barrel height are absent from the graph. Selected final MIL includes local Alt+I refinement; the model plot remains baseline analysis. Measuring time does not validate the arc shape.

### 5. Profiles and sources

**Profiles and sources** shows retained weapon limits, table provenance and links. L81 remains within **132–684 m / 850–150 MIL**: another table's rows up to 950 MIL do not extend its working range. SPH-2 preserves both tables, including the previously confirmed 1400 MIL. MetaForge's L52 is not established as the same profile as SPH-2; its seconds are not substituted into your calculation. See [calculations](CALCULATIONS-EN.md) for the full equations, source versions and conflicts.

Planning data is outside the portable application and survives updates:

| File in `%LOCALAPPDATA%\WardogsFireControl` | Contents |
| --- | --- |
| <code>recent-fire-missions.json</code> | Up to 64 recently accepted positions and targets |
| `fire-missions.json` | Named gun positions and targets |
| `flight-profiles.json` | Measured flight times |
| `planning.ini` | Assumed-model parameters and the latest profile fields |

To move data to another PC, copy these files with the application closed; connect the appropriate terrain package separately. For the JSON stores, damaged or unknown formats produce an error instead of automatic replacement with empty data.

## SPH-2: aim immediately, refine optionally

The main window, mini card, sight and Alt+I share one selected final command. Target metres, final MIL and approximate community-table metres are distinct; the latter are not promised to match the current game RNG. The change shown in ranging is already applied. Left/right refers to bearing degrees; farther means more MIL for the low arc and less MIL for the high arc. See the [2.11 workflow](USAGE-EN.md).

Select **SPH-2 · Artillery**. Mandatory ranging shots are no longer required: after you set the gun and target, the application immediately shows both available trajectories. You do not need to fire in another direction or collect a pair of shots with different bearings.

1. Confirm the current map. **M → right-click gun → Mark Coordinates → Alt+X** sets the gun position.
2. **Middle-click the target** on the map to obtain a calculation. **F4** changes the selected trajectory; the check mark identifies the row used by the sight and impact recording.
3. Set that row's bearing and MIL in the game. The main distance in meters is the distance to the target, not a sight-setting command.
4. If a normal shot misses and you want to account for it, open the map, **point at the actual impact and press Alt+I**. Do not middle-click the impact: that would replace the target. The impact refines the existing target calculation without requiring another shot.

**Alt+I is optional.** One miss refines only that trajectory and the immediate target area. Its correction does not carry over to another trajectory or a distant target. Repeated observations estimate deviation from the unchanged original calculation instead of accumulating corrections indefinitely. The first valid impact is applied fully at that target. Validation rejects an impact exceeding 20% of range or 400 m, an incompatible setting or an unavailable corrected aiming solution; the previous calculation is preserved. The card shows miss distance and the bearing/MIL changes actually applied.

Bearing and MIL are estimated separately. Inconsistent impacts limit the estimate's consistency, but do not return an already established correction to zero. Its influence fades smoothly away from the recorded target.

Record an impact **before changing the target or trajectory**. The application saves the displayed bearing/MIL when Alt+I starts, but does not observe the shot or the actual game sight settings. Changing the gun, target, F4 or map cancels an outdated capture. After an OCR error, point at the same location and repeat Alt+I; first confirm or reject uncertain coordinates. A failed or cancelled Alt+I restores the previous SPH-2 target's guidance only when inputs are unchanged and no coordinate review is pending; this creates no new impact or correction.

Expand **Impact corrections** to enter an actual impact manually or reset refinements. Reset restores the direct calculation; no new ranging shot is needed to continue. After moving the gun, enter its coordinates again. If the vehicle's orientation changes at the same position, reset corrections manually: the application does not observe the hull's orientation.

Distance to target is the same for both trajectories. The approximate equivalent table range has a separate tooltip label. Table meters may differ from RNG in the current game; set the **MIL** of the selected trajectory. MIL is the game's aiming scale, not miles.

In the required map selection, Bakurani, Ozeti and Zestafona each use their own verified terrain package. For the training ground or an unknown map, explicitly select a mode without heights: the calculation assumes equal elevations. Do not substitute another map for the training ground. Local data is stored in `%LOCALAPPDATA%/WardogsFireControl/terrain-packs` and survives App updates; the connect button accepts previously obtained `.wdt` files. Changing the map clears previous coordinates and corrections. Terrain changes MIL for both SPH-2 trajectories. L81 keeps its existing table without an elevation correction. Heights describe the map surface; bridges, roofs and actual barrel height are not determined separately. Third-party map data is not included in the distributed ZIP; provenance and limitations are described in `TERRAIN_DATA_NOTICE.md`.

Vehicle tilt, elevation differences, game dispersion and weapon changes can affect impacts. Observation consistency is not a hit probability. Software checks do not establish 90–99% accuracy in the game.

## Validation limits

OCR is checked against control and supplied screenshots; native capture is checked with a dedicated Windows test window. These results are not a test of the live marker during a match. Borderless mode is available for windows above the game; visibility in exclusive fullscreen and impact accuracy require separate verification.

Five real X/Y pairs and scale/position transformations are retained for the 2.10 candidate. New in-game firing trials have not been performed. OCR changes do not change the tables or physical model; first-shot accuracy and any hit percentage are not guaranteed.

The application reads visible pixels and does not modify the game installation. At SoNiX's request, available game logs and public alternatives were examined separately: no ready-made stream of selected X/Y coordinates was found. The application does not watch game files; details are in the [source investigation, in Russian](COORDINATE-SOURCES-INVESTIGATION-RU.md). BULKHEAD approval for OCR/overlays has not been confirmed; invisibility to anti-cheat and freedom from sanctions are not guaranteed. See [technical limits and official rules](ANTICHEAT-EN.md), and [coordinate workflow sources, in Russian](COORDINATE-WORKFLOW-RESEARCH-RU.md).
