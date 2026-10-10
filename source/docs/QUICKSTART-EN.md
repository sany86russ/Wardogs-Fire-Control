# WARDOGS Fire Control — quick start

The calculator reads your gun and target coordinates, then displays aiming values for **L81** or **SPH-2**. The next target takes one middle-click on the map. OCR and calculations run on your PC.

## First solution

Extract the **complete** portable package and open **Start.cmd**. Administrator privileges are not needed. Choose **RU / EN** in the header.

| Step | Action | Result |
| --- | --- | --- |
| **1. Map** | Select and confirm the current map in the calculator. If elevations are required, import the pack for that exact map. | Coordinate capture becomes available. |
| **2. Gun** | In game: **M → right-click the gun → Mark Coordinates**. Once X/Y appear in the chat input, press **Alt+X**. You do not need to send a message. | Gun position and mini card. |
| **3. Target** | Point at the target on the map and press the **middle mouse button**. Keep the cursor near the label while reading is in progress. | A solution for that point. |
| **4. Aim** | For SPH-2, set the **bearing and MIL** of the selected row. **F4** changes the arc; a check mark identifies the selected one. For L81, use the displayed range, bearing and MIL. | Values to set on the game sight. |
| **5. Next target** | **Middle-click** the next point. | New aiming values without entering the gun again. |

**Alt+C** returns to the calculator and keeps middle-click capture ready. If you move the gun, capture its coordinates again. Changing the map clears previous coordinates and corrections; after restarting, confirm the map again.

## Reading the result

- **To target** is the horizontal map distance.
- **Bearing** is the aiming direction.
- **Set / MIL** is the command on the game's sight scale.
- **Table ≈** is a secondary range estimate, which can differ from the in-game RNG. For SPH-2, set the selected arc's MIL.

If aim is unavailable or coordinates need review, previous values are hidden. Do not use an old command for a new point.

## If coordinates are not read

| Situation | Action |
| --- | --- |
| Gun reading failed | Repeat **Mark Coordinates**, make sure a complete X/Y pair is visible in the active chat input, then press **Alt+X**. |
| Target reading failed | Return to the chosen point's label and middle-click again. Keep the cursor and game window unchanged during reading. |
| Review requested | **Alt+C** → choose or correct the pair → apply reviewed coordinates. **Reject** restores the previous solution. |
| Manual input needed | Expand **Manual input** in the main window and enter the gun and target. Example: `x98.48, y110.35`. **Paste** reads the clipboard only when you click it. |
| Backup target from chat needed | **M → right-click the target → Mark Coordinates → Alt+T**. In **Settings → Recognition**, enable automatic search for additional captures. |

Normal play does not require selecting a custom OCR region. **Alt+R** and Windows OCR apply to additional custom-region reading. The quick workflow uses the bundled RapidOCR. [Coordinate workflow details](COORDINATE-WORKFLOW-RESEARCH-RU.md).

## SPH-2: optional refinement

If a normal shot misses, point at the **actual impact** on the map and press **Alt+I**, before changing the target or arc. Do not middle-click the impact: that would replace the target.

Ranging displays the miss and the already applied bearing/MIL change. **Do not add the correction again.** One accepted observation immediately refines the current target; more observations help assess consistency. Corrections apply to that arc and the local target area. Reset returns to the direct solution; ranging shots are optional.

The application does not observe firing, actual sight settings or the vehicle's attitude. If the chassis changes attitude without moving to new coordinates, reset corrections manually. [Full workflow and ranging limits](USAGE-EN.md).

## Settings and additional tools

| Section | Purpose |
| --- | --- |
| **General** | Operating mode, middle-click and update checks. Manual input only disables game capture and global shortcuts. |
| **Shortcuts** | Your key combinations. Conflicts are resolved with an available combination; the interface displays the current shortcuts. |
| **Sight** | Size and alignment of the transparent reticle. |
| **Recognition** | Marker-reading delay, custom-region OCR and additional captures. The custom coordinate pattern expands separately. |

The **Positions and flight** button opens **target shifts**, **recent and named points**, **terrain** and **your own flight-time observations**. Normal firing does not require these tools. Calculation details and assumed-model settings expand separately. **History and direction** expands current-session targets beside the main solution. **Alt+I does not measure flight time**; the model is disabled by default and always labelled as an assumption.

Drag the mini card to a second monitor and drag its edges to resize it. Position, monitor and separate L81/SPH-2 sizes are saved automatically. The header buttons open controls, lock the card and return to the main window. **Right-click** also opens controls for opacity, the reticle and **Always on top**, which can be disabled on a second monitor.

A locked card passes clicks to the game. The default unlock shortcut is **Ctrl+Alt+Q**; its menu shows the current shortcut. Map, height status and target remain below the values. Hover over a caption for model details; capture warnings remain visible.

## Updates and saved data

An update replaces the complete application package. Settings, imported maps, history and your measurements stay separately in `%LOCALAPPDATA%\WardogsFireControl` and are preserved.

To transfer to another PC, copy the profile while the application is closed. Main files: `settings.ini`, `planning.ini`, `recent-fire-missions.json`, `fire-missions.json`, `flight-profiles.json` and the `terrain-packs` folder. Logs are in `logs`; review their contents before posting them publicly. Corrupt JSON stores display an error and are not automatically replaced with empty data.

## Limits to keep in mind

Bakurani, Ozeti and Zestafona require their own elevation packs. Training and unknown maps use an explicitly selected mode without elevations. Terrain describes the ground, not roofs, bridges, trees or barrel height. Third-party map data is not included in the distributed ZIP. [Map provenance and limits](../TERRAIN_DATA_NOTICE.md).

Community tables and the geometric model may differ from the current game physics. L81 retains the **132–684 m** supported range; SPH-2 has two arcs whose availability depends on range and elevations. OCR speed, software tests and consistent observations alone do not establish a 99% hit rate in matches. Use borderless mode for overlays; exclusive fullscreen needs separate validation.

[Detailed calculations](CALCULATIONS-EN.md) · [Coordinate sources](COORDINATE-SOURCES-INVESTIGATION-RU.md) · [Interaction rules and limits](ANTICHEAT-EN.md)
