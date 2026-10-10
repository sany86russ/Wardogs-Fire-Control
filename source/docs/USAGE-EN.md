# WARDOGS Fire Control — workflow

[Русский](USAGE-RU.md) · [App overview](../../README.en.md) · [Mathematics](CALCULATIONS-EN.md)

This guide describes the current workflow: map, gun, target and optional ranging. Download the complete package from the release page. L81 and SPH-2 tables are unchanged. First-shot accuracy and agreement between table metres and the game's RNG are not promised.

## Before calculating

1. Select and confirm the current map. **Confirm map and enter game** explicitly confirms it and enters game mode; afterwards, the button reads **To game**. Select a map first when none is chosen; missing required elevations block entry. After restarting, the saved name requires confirmation.
2. Select L81 or SPH-2.
3. SPH-2 on Bakurani, Ozeti or Zestafona requires verified local elevations for that map. For the training ground or an unknown map, explicitly select no-height mode. Another map cannot replace missing terrain.
4. Use borderless windowed mode for overlays. The app does not obtain the map or coordinates from game memory.

Changing maps clears current coordinates and corrections. A restored record does not confirm the map for you or restore earlier ranging corrections.

## Main SPH-2 cycle

~~~text
Confirm map
    ↓
M → right-click gun → Mark Coordinates → Alt+X
    ↓
Middle-click target on map
    ↓
Selected bearing and final MIL → fire
    ↓
Point at actual impact → Alt+I
    ↓
Refined bearing and MIL for the original target
~~~

The gun is read from the active chat draft; you do not need to send the message. The target and impact are read from separate X/Y labels beside the cursor. This workflow does not require a custom region.

**Middle-click sets the target. Alt+I records an impact.** Middle-clicking the miss location makes it a new target rather than refining the original one. Record the impact before changing the target, arc, map or gun.

Without impact records, SPH-2 provides a direct solution immediately. Alt+I is optional; two required trial shots or firing in another direction are not needed. L81 uses its retained table without an elevation model or SPH-2 ranging.

The app does not observe the shot or actual in-game sight settings. Alt+I captures the displayed selected command when capture begins. An accepted impact refines that command for the original target. If you set different values in the game, the record does not describe the same calculation.

## Which values to set in the game

The main window and mini card distinguish three values:

| Value | Meaning |
|---|---|
| **Target range, m** | Horizontal distance between the accepted gun and target points |
| **Final MIL** | Selected arc's command with applicable elevation and local corrections |
| **Approximate table range, m** | Inverse conversion of the current MIL through the retained community table |

Aim using the selected command's **bearing and final MIL**. Target metres do not become a sight-setting command. Table metres do not promise the game's HUD RNG value.

The main window, mini card, sight and command snapshot for Alt+I refer to one selected arc. Analysis of the other available arc in additional tools is labeled as baseline. **F4** changes the selection; an impact record from the previous arc must not be applied to the new one.

## Ranging

The compact **RANGING** block shows:

- The latest accepted miss in metres.
- **Applied changes** in bearing degrees and MIL, already included in the final command.
- Accepted observation count.
- **Reset corrections**.

Left/right refers to bearing changes; closer/farther refers to changes in MIL. For table-based movement farther:

| SPH-2 arc | Farther | Closer |
|---|---|---|
| Low | Increase MIL | Decrease MIL |
| High | Decrease MIL | Increase MIL |

Alt+I considers the actual miss direction and magnitude. These signs explain the hint; they do not replace the resulting final command.

Corrections are local: only same-arc observations near the target contribute, with influence fading to zero at 50 m. A miss must be within 20% of horizontal range and 400 m simultaneously; command compatibility and the final sight's availability are also checked. Rejecting a record preserves previous observations and guidance.

Reset immediately restores the baseline calculation. Set the gun again after moving it. Reset corrections manually if the vehicle's body changes orientation at the same point: the app does not observe its tilt.

Manual actual-impact entry is under **Impact corrections** in the main window. Gun and target coordinates are under **Manual input**. Recording an impact has the same effect as Alt+I and does not move the target. Expand **Correction details** for the full report and reset action.

## Automatic ground assessment

After accepting an SPH-2 target, the app assesses ground along the selected arc's baseline model trajectory. Read states literally:

| State | Meaning |
|---|---|
| Estimated ground intersection | The model arc met known ground at a checked location |
| No intersection at sampled points | No crossing was found in the available samples; clear flight is not guaranteed |
| Heights unknown | Ground cannot be assessed without suitable data |
| Incomplete coverage | Part of the path lacks data; this is not a positive clearance result |

The other arc may be suggested based on ground assessment, but the app does not change the arc automatically. The player decides whether to switch.

The check concerns the **baseline model arc**, not measured flight after Alt+I. Aiming corrections do not determine the actual trajectory. Buildings, trees, bridges, roofs and barrel height are excluded; no detected crossing does not mean a safe shot. See [model boundaries](CALCULATIONS-EN.md).

## Positions and flight

**Positions and flight** is optional for the main cycle. It provides manual actions and detailed analysis. Expand **Calculation details** and **Model settings** for the full result and assumed model.

### Manual target shifts

Left/right/closer/farther commands in 10/25/50/100 m steps **change the accepted target coordinates** relative to the gun-to-target direction. Each click creates a new targeting task.

This does not record an impact or refine a miss. Use shifting when the spotter specifies a **new target point**. To account for an actual miss at the original target, use Alt+I or manual impact entry.

### Saved positions and targets

Accepted positions and targets are saved automatically with exact coordinates, map, weapon and point kind: up to **64** recent records in <code>recent-fire-missions.json</code>. Naming is optional; save a named point separately in the up-to-**500**-record collection <code>fire-missions.json</code>. Neither is a required step before calculating.

To restore a point:

1. Confirm the appropriate map and select the matching weapon.
2. Open **Positions and flight → Positions and targets**.
3. Explicitly select and apply the desired record.
4. Set a target again after restoring the gun; previous ranging corrections are not restored.

Ambiguous OCR, unaccepted review and empty coordinates do not become successful positions. Coordinates retain their original precision; a rounded list label is not calculation input.

Saved points survive app updates but **do not restore the map or gun automatically on launch**. The last 12 targets in the current session are a separate list under **History and direction** in the main window.

### Flight time

Time is unknown by default. Alt+I does not measure it: the delay includes player reaction, opening the map and positioning the cursor.

A personal measurement separately includes range, time, uncertainty, weapon, arc, elevation difference, a version/profile label and source. Matching observations apply at their ranges and between them; there is no extrapolation outside their coverage.

The assumed speed/gravity model is enabled separately and is **disabled by default**. Its seconds are labeled as estimates. Earth's 9.80665 m/s² gravity is an assumption, not a recovered WARDOGS constant. Enabling the model does not change the table or final MIL.

### Ground profile and sources

The detailed profile shows known ground and the model arc; missing elevations remain gaps. Source information explains table provenance and conflicts. It does not automatically download another project's ballistics or change the weapon.

## If reading fails

During a new capture or unresolved review, previous guidance is hidden. Do not treat old values as a new solution.

- For the gun, repeat Mark Coordinates, check the full pair in the active draft and press Alt+X.
- For a target, middle-click again while keeping the cursor beside both labels.
- For an impact, repeat Alt+I on the same actual point before changing the target or arc.
- Resolve ambiguous coordinates by editing and applying them in review, or explicitly reject the capture.
- Manual input and additional captures are under **Manual input**. Set the fallback reading source in **Settings → Recognition**: with automatic search enabled, Alt+T reads an active target draft; with a custom-region choice, it uses that region.

Actual controls can differ from these examples after a Windows hotkey conflict. Use the combinations shown by the app.

## Data and limits

Settings, accepted points, personal measurements and logs reside in <code>%LOCALAPPDATA%\WardogsFireControl</code>. OCR and calculations run locally; diagnostics can contain game coordinates and local paths. Review logs before attaching them publicly.

Check a build's GitHub Actions results for its test status. Software checks do not establish recognition of every live HUD or firing quality in the current match. Workflow changes do not extend weapon ranges or promise a hit percentage.

[Game-interaction rules](ANTICHEAT-EN.md) · [Detailed calculations](CALCULATIONS-EN.md) · [Launch quick start](QUICKSTART-EN.md)
