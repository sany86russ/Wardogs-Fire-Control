# SPH-2 validation and the 10 m criterion

[Русский](SPH2-VALIDATION-RU.md) · [Ranging model](continuous-calibration.md)

Version 2.12 learns from the command shown to the player: integer MIL and azimuth rounded to 0.1°. Cards, the ghost sight, clipboard and active planning command agree. Internal calculations retain full precision; the approximate sight-table range corresponds to commanded MIL. Displayed settings are not automatically treated as observed in-game settings.

The first valid impact applies the full local correction. From three effective nearby observations, a robust series centre suppresses isolated outliers independently for azimuth and MIL. No additional settings or mandatory shot sequence are introduced. Corrections fade to zero over 50 m and remain on their observed arc. Changing the map or firing position clears history. Turning the hull in place still requires a manual reset because the application does not observe its pose.

## Why the tables are preserved

The October 10, 2026 source comparison found no transcription errors: all 210 retained L81/SPH-2 rows matched the published [Apollyon table family](https://github.com/apollyon-sys/wardogs-calculator/blob/87e3cf3579c88499b16dfd1e29ed56c70bc0bc7b/data/weapons.json). Copies of one table are not independent measurements. [MZ](https://github.com/Firepanda415/MZ-Wardogs/blob/5a60171796a24eb650e2202a9773a9890a17c48c/docs/sph2-scale.md) checks HUD scale readings; small differences do not establish impact ranges. Apollyon's public impact series provide useful field references but do not cover every map, arc, range and hull position.

Unsupported constant shifts, mixed arcs and replacement physics were not adopted. The SPH-2 height calculation remains an approximation without independently confirmed game constants, muzzle height or current hull pose. The working L81 profile is preserved.

## Analyse saved impacts

Run from the source root without launching the game or application:

```powershell
py -3.11 source/tools/analyze_shot_accuracy.py --archive-directory "$env:LOCALAPPDATA/WardogsFireControl/logs" --json --output shot-accuracy.json
```

The default closed hit radius is **10 m**. Only explicitly supplied files/directories are read; logs and the screen are untouched. Supply `logs/latest.archive` separately for older records, or provide explicit log paths together. Without `--json`, the output is Markdown.

The report includes accepted observation counts, recorded hit rate, mean/median/p95 miss, right/longitudinal bias and RMS spread around the centre. Groups separate sessions, maps, firing positions, arcs and correction modes. Diagnostic records, duplicates and conflicting identities are excluded; corrupt records are reported. Requests and rejections are counted separately, not as physical shots. Legacy records carry weaker provenance.

New logs include session, command and observation identifiers, full calculated and displayed settings, coordinates, heights and the next command. `command_source=displayed_unverified`, `physical_shot_verified=0` and `sight_verified=0` describe what the application actually observes.

## Evidence needed for a 99% claim

The denominator consists of **accepted recorded impacts**, not every fired round. Field acceptance requires a complete independent series, actual verified sight settings and every shot, including misses. The scope must state validated maps, arcs, ranges and conditions, separating initial shots from those after ranging.

The numerical test uses an exact one-sided 95% Clopper–Pearson lower bound. **299 independent hits out of 299** exceed the 99% lower-bound threshold only with a complete verified field protocol. Log flags cannot establish that protocol; `field_99_percent_claim_accepted` remains `false` even for asserted verification flags.

The UI's correction-estimate spread describes observed consistency, not future hit probability. Regression tests verify rounding feedback, robust centring, arc isolation and transactional rejection; game measurements remain a separate acceptance step.
