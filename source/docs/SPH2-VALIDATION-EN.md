# SPH-2 validation and the 10 m criterion

[Русский](SPH2-VALIDATION-RU.md) · [Ranging model](continuous-calibration.md)

Since 2.12, impact learning uses the command shown to the player: integer MIL and azimuth rounded to 0.1°. Cards, the ghost sight, clipboard and active planning command agree. Internal calculations retain full precision; the approximate sight-table range corresponds to commanded MIL. Displayed settings are not automatically treated as observed in-game settings.

The first valid impact applies the full local correction. From three effective nearby observations, a robust series centre suppresses isolated outliers independently for azimuth and MIL. Individual target corrections and MIL fade to zero over 50 m and remain on their observed arc.

The main SPH-2 workflow also learns **a new target's bearing before its first Alt+I**. It needs at least three independent groups of previously ranged targets from the same firing position, arc and current session; repeated shots at one target do not create three groups. Each group contributes one median offset. With agreeing signs, the estimate shrinks when past targets disagree. Validation excludes every target group in turn and refits using only the remaining groups. Transfer is enabled only if median and RMS errors improve and the maximum validation error does not increase. The forecast remains provisional: an individual target can become worse.

The new target must stay within the observed ranges, modelled physical elevations and short bearing sector; only the displayed command's 0.1° resolution is allowed at the sector edge. A measured local correction takes priority, with a smooth transition to the shared bearing; **MIL is not transferred between distant targets**. No imported or hardcoded measured offset is applied, and no new user settings are required.

Predictions do not carry between maps, positions, arcs or sessions and are disabled after pause or reset. Transfer then needs fresh independent targets. Changing the map or firing position clears history. Turning the hull in place still requires a manual reset because the application does not observe its pose. The forecast is already included in displayed bearing and must not be added again. It establishes neither a 99% hit rate nor improvement of every shot.

## Why the tables are preserved

The main calculator, terrain analysis and initial-calibration target checks use the same SPH-2 coordinate solution. At exact table endpoints it accommodates only internal floating-point roundoff: for example, X `126.29 - 100` can produce `2629.0000000000005` metres. Analysis no longer rejects that target while the main window has a valid command. Supported ranges are unchanged; raw distance inputs outside the table remain invalid.

A new [WardogTools](https://wardogtools.gg/artillery/) check on 7 November 2026 found all 59 low and 80 high rows identical to ours. Its height correction is a first-order approximation, with no evidence of greater accuracy. New [ArtyDogs sight readings](https://github.com/BrickTree/ArtyDogs/blob/55fea41499956c1b764e64a597d42c7c9caf356d/data/game_table_seed.csv) differ from some table entries but do not measure impacts. A [field comparison of physical models](https://github.com/Kiyogitpy/wardogs-data-docs/blob/db8172c4306af86e1373b29ab2f4df10430fe08f/docs/evidence/round21-artillery-range-check.md) still reports material error and provides no universal replacement. These numbers have not been imported into aiming without current-patch validation.

The October 10, 2026 source comparison found no transcription errors: all 210 retained L81/SPH-2 rows matched the published [Apollyon table family](https://github.com/apollyon-sys/wardogs-calculator/blob/87e3cf3579c88499b16dfd1e29ed56c70bc0bc7b/data/weapons.json). Copies of one table are not independent measurements. [MZ](https://github.com/Firepanda415/MZ-Wardogs/blob/5a60171796a24eb650e2202a9773a9890a17c48c/docs/sph2-scale.md) checks HUD scale readings; small differences do not establish impact ranges. Apollyon's public impact series provide useful field references but do not cover every map, arc, range and hull position.

Unsupported constant shifts, mixed arcs and replacement physics were not adopted. The SPH-2 height calculation remains an approximation without independently confirmed game constants, muzzle height or current hull pose. The working L81 profile is preserved.

## Analyse saved impacts

Run from the source root without launching the game or application:

```powershell
py -3.11 source/tools/analyze_shot_accuracy.py --archive-directory "$env:LOCALAPPDATA/WardogsFireControl/logs" --json --output shot-accuracy.json
```

The default closed hit radius is **10 m**. Only explicitly supplied files/directories are read; logs and the screen are untouched. Supplying `logs` includes `latest.log`, `latest.previous.log` and `session-*.log` in its `latest.archive`; repeated file paths are read once. Supply `logs/latest.archive` separately to analyze only older records, or provide explicit log paths. Without `--json`, the output is Markdown.

The report includes accepted observation counts, recorded hit rate, mean/median/p95 miss, right/longitudinal bias and RMS spread around the centre. Groups separate sessions, maps, firing positions, arcs and correction modes. Diagnostic records, duplicates and conflicting identities are excluded; corrupt records are reported. Requests and rejections are counted separately, not as physical shots. Legacy records carry weaker provenance.

New logs include session, command and observation identifiers, full calculated and displayed settings, coordinates, heights and the next command. `command_source=displayed_unverified`, `physical_shot_verified=0` and `sight_verified=0` describe what the application actually observes.

## Evidence needed for a 99% claim

The denominator consists of **accepted recorded impacts**, not every fired round. Field acceptance requires a complete independent series, actual verified sight settings and every shot, including misses. The scope must state validated maps, arcs, ranges and conditions, separating initial shots from those after ranging.

The numerical test uses an exact one-sided 95% Clopper–Pearson lower bound. **299 independent hits out of 299** exceed the 99% lower-bound threshold only with a complete verified field protocol. Log flags cannot establish that protocol; `field_99_percent_claim_accepted` remains `false` even for asserted verification flags.

The UI's correction-estimate spread describes observed consistency, not future hit probability. Regression tests verify rounding feedback, robust centring, arc isolation and transactional rejection; game measurements remain a separate acceptance step.
