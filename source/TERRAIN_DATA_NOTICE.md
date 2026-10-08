# Terrain data source notice

The optional `bakurani.wdt`, `ozeti.wdt`, and `zestafona.wdt` terrain packages
are derived from Terrain3D elevation datasets published by **WARDOGS Artillery
Calculator**, an unofficial community project maintained by Apollyon:

- Project: https://wardogs-artillery.com/
- Source repository: https://github.com/apollyon-sys/wardogs-calculator
- Published Terrain3D data: https://assets.wardogs-artillery.com/releases/assets-v1/data/terrain/

This project records the SHA-256 digest of each upstream manifest, verifies each
source chunk against the digest listed in that manifest, retains the 2 metre
horizontal grid, quantizes height to 0.1 metre units, and re-encodes each chunk
with Zstandard. The resulting `.wdt` files contain elevation samples and package
metadata; they do not contain map imagery.

According to the upstream project's
[legal notice](https://github.com/apollyon-sys/wardogs-calculator/blob/main/docs/legal.md),
its MIT License applies to original source code and does not cover WARDOGS game
assets or other third-party material. Those materials remain the property of
their respective rights holders. This project's MIT License likewise does not
relicense the source terrain data or any WARDOGS intellectual property.

War Dogs Distance Calculator is an unofficial fan-made utility and is not
affiliated with, endorsed by, or officially associated with BULKHEAD or the
WARDOGS development team.

The upstream hosted-infrastructure policy additionally says that public CDN
URLs do not grant permission to mirror, package or redistribute hosted assets.
The portable App and redistributable ZIP do **not** bundle these community-derived
`.wdt` files. The application does not download data from the upstream CDN.
Independently authorised, already available local files can be imported into
`%LOCALAPPDATA%/WardogsFireControl/terrain-packs` with the application's local
import action or `terrain_probe --install <local-source-directory>`. Updating the
portable App leaves this independent per-user directory intact.

## Manual download / Ручная загрузка

Rico217 publishes a separate compatible `terrain-packs.zip` asset with
[WarDogs Distance Calculator v1.4.0](https://github.com/Ricoz217/WarDogs_Distance_Calculator/releases/tag/v1.4.0).
[Download that published archive](https://github.com/Ricoz217/WarDogs_Distance_Calculator/releases/download/v1.4.0/terrain-packs.zip)
from the original release, extract it, and select its `terrain-packs` folder
using **Connect local height data…** in the main window's **Game map** section.
Installing the original application is unnecessary. This is a reference to an
independently published asset, not a mirror or a grant of redistribution rights.

Для ручной загрузки используйте отдельный архив `terrain-packs.zip` из
указанного релиза Rico217 v1.4.0. Распакуйте архив и выберите вложенную папку
`terrain-packs` кнопкой **«Подключить локальные данные высот…»** в блоке карты
основного окна.
Программа проверит файлы и установит их в пользовательскую папку. Устанавливать
приложение Rico217 не требуется. Затем выберите и подтвердите соответствующую
карту в основном окне. Это ссылка на самостоятельную публикацию автора;
она не меняет права на данные и не разрешает их повторное распространение.

The published archive has SHA-256
`c511df32f956e9171ac79bf939e955ea7f2851a4c39bdad6f924e46c989ede49`.
The three `.wdt` hashes below match the files in that archive. The application
validates the package files, not the surrounding ZIP. Later upstream versions
may change; the pinned v1.4.0 link identifies the checked compatible dataset.

Every recognized import checks the package SHA-256 and its embedded map identifier;
the exact copied bytes are checked before atomic installation. The application
uses the package matching the user's selected map, with a verified package next
to the EXE taking precedence over the independently imported user copy. It does
not infer the active game map from overlapping X/Y coordinates. Training range
data is not available; selecting a different map is not a valid substitute.

Integrity checks establish that a file matches the recorded dataset, not rights
from the data owner, multiplayer approval or compatibility with every future game
update. These local data files must not be included in Git, a ZIP, a mirror or
another redistribution without permission from the relevant rights holders.

Recognized datasets retain a 2 metre sampling grid and 0.1 metre height units
(maximum re-encoding quantization error 0.05 metre). X uses +50 landscape quads
per game coordinate unit; Y uses -50. The application interpolates heights inside
coverage and rejects points outside it. The dataset's vertical origin can differ
from the game's ASL readout; firing calculations use differences between terrain
samples. The grid represents ground surface, not bridge decks, building roofs or
the vehicle's barrel height.

| Map | Package SHA-256 |
|---|---|
| Bakurani | `9c79af2f69df5023f6e2e944329ae80981432e7115832ae801da067bd79bfdde` |
| Ozeti | `f636225e89ffe19111da58466d4db60e4ccff285c3cfa11d8c30b959f544c400` |
| Zestafona | `e60f95a6e23791164342fe51b465ac04238f338f1f4293aef664b61513ecc048` |
