# WARDOGS Fire Control 2.8.0 — source

[Русский](README.md) · [Project overview and downloads](../README.en.md)

A native C++20 / Qt Widgets artillery assistant **for the game WARDOGS**. It calculates horizontal range, bearing and table-based MIL settings for L81 and SPH-2. It accepts manual coordinates and uses local OCR of visible game fields. Based on the MIT-licensed [Rico217 / Ricoz217 project](https://github.com/Ricoz217/WarDogs_Distance_Calculator), version `v1.4.0`; further development and interface by SoNiX.

Russian is the default. The **RU / EN** header selector changes the interface immediately, saves the preference and preserves coordinates, the map, selected arc and calculation. Translations are embedded in the EXE from `translations/*.json`.

## Using the application

Version 2.8.0 checks stable GitHub releases and updates the entire portable package when you click Update. It includes asynchronous SHA-256 verified downloads, manifest validation, an external helper and rollback on installation failure. See the [update guide](../docs/UPDATES-EN.md).

1. Confirm the current map. Training-ground and unknown-map selections use an explicit mode without heights.
2. Open the map with **M**, select the gun through **right-click → Mark Coordinates**, then press **Alt+X**. You do not need to send the coordinates to chat.
3. Point at a target and press the **middle mouse button**. The application reads separate X/Y labels near the cursor and displays range, direction and MIL.
4. For SPH-2, select the low or high arc. Optional **Alt+I** records an actual impact for a local correction; initial test shots are not required.
5. **Alt+C** opens the main window. Manual input, ambiguous OCR review, history, copying, the mini card and sight overlay are available in the interface.

If shortcuts conflict, the UI shows the combinations actually registered. Weak or incomplete pairs require review. A new unaccepted gun capture blocks aiming from the old position. Moving the gun or changing the map clears dependent corrections.

## Documentation

| Guide | Contents |
| --- | --- |
| [Quick start](docs/QUICKSTART-EN.md) | Launching, coordinates, shortcuts, settings and troubleshooting |
| [Calculations and limits](docs/CALCULATIONS-EN.md) | Coordinates, L81/SPH-2 tables, interpolation, elevation and local corrections |
| [Architecture](docs/ARCHITECTURE-EN.md) | Modules, threads, storage, OCR and checks |
| [Game interaction](docs/ANTICHEAT-EN.md) | System APIs, integration boundaries and anti-cheat |
| [Release notes](docs/RELEASE-NOTES-EN.md) | Interface and calculation changes |
| [Coordinate workflow, Russian](docs/COORDINATE-WORKFLOW-RESEARCH-RU.md) | Visible text sources and stale-result protection |
| [Alternative sources, Russian](docs/COORDINATE-SOURCES-INVESTIGATION-RU.md) | Why the application reads visible pixels |
| [Original documentation](docs/README-UPSTREAM.md) | Preserved description of the original author's project |

## Building

From the repository root, after installing MSVC x64 and Qt 6.8+ and setting `QT_ROOT`:

```powershell
.\Build.ps1
.\Build.ps1 -Package
```

CMake/Ninja build the application; normal builds run CTest. `Build.ps1 -SkipTests` prepares a build without tests, but combining it with a `-Package` update is rejected. Detailed prerequisites, public builds and ready-to-use ZIP downloads are described in the [root README](../README.en.md).

## Data and limits

Settings: `%LOCALAPPDATA%\WardogsFireControl\settings.ini`. The previous profile is imported initially without writing to it. Acceptable local height packages are stored separately in `%LOCALAPPDATA%\WardogsFireControl\terrain-packs` and survive updates. The user selects the map; the application does not detect it automatically.

Community `.wdt` files are excluded from the public source repository and ZIP. They are not covered by the application's MIT license; import only already available files you have rights to use. For a map using heights, a missing or damaged package blocks confirmation. No-height mode requires explicitly selecting the training ground or unknown map. See the separate [manual download of a third-party published archive and import instructions](TERRAIN_DATA_NOTICE.md). L81 has no validated elevation model.

Software tests do not establish in-match hit accuracy, publisher approval or overlay visibility in exclusive fullscreen. Recognition and calculations run locally. The application does not observe the shot itself or the actual sight settings; the player performs aiming and firing.

[MIT license](LICENSE) · [Third-party components](THIRD_PARTY_NOTICES.md) · [Terrain provenance](TERRAIN_DATA_NOTICE.md)

## Support the author

Ways to support SoNiX and cryptocurrency wallet addresses are listed in the [main project overview](../README.en.md#support).

## Trouble connecting to WARDOGS?

See another SoNiX project: [StabiLink Desktop](https://github.com/sany86russ/StabiLink-Desktop). It may help with access to network services; compatibility depends on your network and settings.
