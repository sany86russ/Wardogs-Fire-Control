# WARDOGS Fire Control 2.7.0 — quick start

A portable assistant for L81 and SPH-2: distance, bearing and table-based aiming from two points. The main in-game workflow is enabled by default; recognition and calculations run locally.

## Launch and first calculation

Extract the entire ZIP and double-click **Запустить.cmd** or **WarDogsDistanceCalculator.exe**. In the working project, **Запустить.cmd** opens the new application from `App`; the original executable in the root is kept separately. Windows 10/11 x64; installation and administrator privileges are not required. Keep the `models` and `platforms` folders and the DLLs beside the executable.

Russian is the default interface language. Select **EN** in the header beside the version to switch to English. You can switch back to **RU** at any time; the preference is saved. See the [language guide](LOCALIZATION.md).

1. In **Game map · required**, select the current map. After restarting, confirm the previous selection with **Confirm map**. Aiming and entry into game mode remain unavailable until the map is confirmed.
2. Open the full WARDOGS map with **M**. Right-click the gun position and select **Mark Coordinates**. The pair appears in the chat input; you do not need to send a message.
3. Press **Alt+X**. The application finds the active draft in the upper-left part of the window, independently of any saved custom region. If it reads a complete pair with sufficient confidence, it sets the gun position, hides the main window and shows the mini card.
4. Point at the target on the map and press the **middle mouse button**. The application reads the separate X and Y labels near the cursor and displays the calculation. The button still places a marker in the game; this target does not need new text in chat.

Use **Tab** to switch chat channels: team, local, vehicle or all. The reading region adjusts automatically to the current channel label width. Channel text comes from the game and follows the game's language.

You do not need to select an OCR region, press Start game or confirm an ordinary successful capture. For the next target, use the middle mouse button. It works after the gun position is accepted, with either the main window or the mini card visible. Returning with **Alt+C**, the tray or another launch opens the calculator and keeps target capture ready. When you move the gun, repeat the map selection and gun coordinate capture. To stop capture entirely, select the standalone calculator in settings, disable the middle mouse button or close the application.

Shortcuts can change after a conflict with another application. The current combinations appear in the hint and settings; use them instead of the defaults shown here.

## When automatic reading needs review

For the gun, the application checks for an active draft, one complete X/Y pair, agreement between two passes, character confidence and consistency with visible characters. Sent chat history does not replace a damaged draft. The middle mouse button reads separate X and Y fields around the clicked point without consulting chat.

After a failed target reading, the application can make two further brief attempts while the cursor stays at the same point and the active window stays unchanged. If you mark a new point during OCR, only the latest capture remains queued; an outdated result will not be applied. The screen is not read continuously.

Weak, clipped or ambiguous text opens a review card. Return with **Alt+C**, select a pair, correct it if necessary and press **Apply verified coordinates**. Once the gun is accepted, the middle mouse button is ready regardless of how the position was entered or confirmed. During a new reading and until an error is resolved, the previous aiming solution is hidden. An unaccepted new gun capture also blocks new targets, so the calculation cannot use the old gun position. An empty capture is not presented as a successful calculation. **Reject** explicitly restores the previous calculation; manual input cancels the pending review.

## If coordinates are not read

- **Gun:** select Mark Coordinates again, check that the pair is visible in the active chat input, then repeat Alt+X.
- **Target:** keep the cursor near the labels of the selected point and click the middle mouse button again. Switching windows or moving the cursor stops the previous automatic attempt.
- If the game is minimized, its window is not recognized or the region extends beyond one monitor, open the game on a single monitor and try again.
- For an unusual HUD, use **Manual input and diagnostics** or the advanced recognition settings.

Automatic chat detection is limited to the upper-left part of the client window. It accounts for the window's physical position and size, including negative coordinates on another monitor. Fields near the cursor remain within the game client. The application uses the original executable's geometry and a limited scaled retry; it selects complete labels for the required axes within each field and does not treat a cursor fragment at the edge as a digit. These checks do not guarantee recognition of every HUD and resolution.

## Manual fallback

Diagnostics are stored in `%LOCALAPPDATA%\WardogsFireControl\logs`: `latest.log` contains the current session and `latest.previous.log` contains the previous log. Each file is limited to 4 MiB; earlier history is replaced after several launches. Completed readings and calculations are written immediately. The log records X/Y recognition details and rejection reasons, but no screenshots: the log alone cannot reconstruct an obscured digit or its background.

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

Profile: `%LOCALAPPDATA%\WardogsFireControl\settings.ini`. If it does not exist yet, the previous `%LOCALAPPDATA%\WarDogsDistanceCalculatorCpp\settings.ini` is read without modifying the old file. Profiles without the new-workflow marker are migrated to quick mode: game features, middle mouse capture and automatic detection are enabled; gun capture uses Alt+X and return uses Alt+C, accounting for internal conflicts. After the new profile is saved, any subsequent choice of standalone mode is retained.

Settings are written in Unicode, preserving additional sections and keys. A damaged ANSI version of the standard pattern is repaired automatically. For a rejected custom pattern, use **Restore standard WARDOGS pattern**; complex expressions are not accepted. Windows OCR remains an optional source; automatic reading of the two map fields uses the main bundled model.

## SPH-2: aim immediately, refine optionally

Select **SPH-2 · Artillery**. Mandatory ranging shots are no longer required: after you set the gun and target, the application immediately shows both available trajectories. You do not need to fire in another direction or collect a pair of shots with different bearings.

1. Confirm the current map. **M → right-click gun → Mark Coordinates → Alt+X** sets the gun position.
2. **Middle-click the target** on the map to obtain a calculation. **F4** changes the selected trajectory; the check mark identifies the row used by the sight and impact recording.
3. Set that row's bearing and MIL in the game. The main distance in meters is the distance to the target, not a sight-setting command.
4. If a normal shot misses and you want to account for it, open the map, **point at the actual impact and press Alt+I**. Do not middle-click the impact: that would replace the target. The impact refines the existing target calculation without requiring another shot.

**Alt+I is optional.** One miss refines only that trajectory and the immediate target area. Its correction does not carry over to another trajectory or a distant target. Repeated observations estimate deviation from the unchanged original calculation instead of accumulating corrections indefinitely. The first valid impact is applied fully at that target. Validation rejects an impact exceeding 20% of range or 400 m, an incompatible setting or an unavailable corrected aiming solution; the previous calculation is preserved. The card shows miss distance and the bearing/MIL changes actually applied.

Bearing and MIL are estimated separately. Inconsistent impacts limit the estimate's consistency, but do not return an already established correction to zero. Its influence fades smoothly away from the recorded target.

Record an impact **before changing the target or trajectory**. The application saves the displayed bearing/MIL when Alt+I starts, but does not observe the shot or the actual game sight settings. Changing the gun, target, F4 or map cancels an outdated capture. After an OCR error, point at the same location and repeat Alt+I; first confirm or reject uncertain coordinates.

Expand **Impact corrections** to enter an actual impact manually or reset refinements. Reset restores the direct calculation; no new ranging shot is needed to continue. After moving the gun, enter its coordinates again. If the vehicle's orientation changes at the same position, reset corrections manually: the application does not observe the hull's orientation.

Distance to target is the same for both trajectories. The approximate equivalent table range has a separate tooltip label. Table meters may differ from RNG in the current game; set the **MIL** of the selected trajectory. MIL is the game's aiming scale, not miles.

In the required map selection, Bakurani, Ozeti and Zestafona each use their own verified terrain package. For the training ground or an unknown map, explicitly select a mode without heights: the calculation assumes equal elevations. Do not substitute another map for the training ground. Local data is stored in `%LOCALAPPDATA%/WardogsFireControl/terrain-packs` and survives App updates; the connect button accepts previously obtained `.wdt` files. Changing the map clears previous coordinates and corrections. Terrain changes MIL for both SPH-2 trajectories. L81 keeps its existing table without an elevation correction. Heights describe the map surface; bridges, roofs and actual barrel height are not determined separately. Third-party map data is not included in the distributed ZIP; provenance and limitations are described in `TERRAIN_DATA_NOTICE.md`.

Vehicle tilt, elevation differences, game dispersion and weapon changes can affect impacts. Observation consistency is not a hit probability. Software checks do not establish 90–99% accuracy in the game.

## Validation limits

OCR is checked against control and supplied screenshots; native capture is checked with a dedicated Windows test window. These results are not a test of the live marker during a match. Borderless mode is available for windows above the game; visibility in exclusive fullscreen and impact accuracy require separate verification.

The application reads visible pixels and does not modify the game installation. At SoNiX's request, available game logs and public alternatives were examined separately: no ready-made stream of selected X/Y coordinates was found. The application does not watch game files; details are in the [source investigation, in Russian](COORDINATE-SOURCES-INVESTIGATION-RU.md). BULKHEAD approval for OCR/overlays has not been confirmed; invisibility to anti-cheat and freedom from sanctions are not guaranteed. See [technical limits and official rules](ANTICHEAT-EN.md), and [coordinate workflow sources, in Russian](COORDINATE-WORKFLOW-RESEARCH-RU.md).
