# Automatic observation collection and SPH-2 refinement

The normal workflow stays **Alt+X after moving the gun → middle mouse for the target → fire → optional Alt+I for the impact**. There is no new calibration procedure, mandatory test shot or shortcut.

## Saved observations

Each accepted Alt+I or manual impact automatically saves exact gun/target/impact coordinates, arc, height deltas, the displayed azimuth/MIL command and session/emplacement/observation identities. Height source and the verified terrain package SHA-256 are saved automatically when available. Application and table versions are separate; an unknown game version remains explicitly `unknown`.

Since 2.17.0, `table_profile=sph2_empirical_low_v1` identifies low-trajectory table aiming without height correction, selected by default. `sph2_retained_v1` identifies the legacy model, including high trajectory and low trajectory with approximate height correction enabled. The log analyzer separates groups by this field; older records without it use the legacy profile. Actual heights are retained in both modes. The identifier describes the calculation method, rather than verified hit accuracy.

`%LOCALAPPDATA%/WardogsFireControl/accuracy-observations.json` lives beside saved points, outside the portable updater payload. It retains the latest 4096 observations, bounded by 8 MiB, replacing the oldest entries as history grows. Duplicate identities do not create extra impacts. Rejected points remain outside this accepted-observation archive; normal logs retain rejection diagnostics.

These are user-marked landings and displayed commands, **not verified actual sight settings or physical shots**. Normal records retain `command_source=displayed_unverified`, `sight_verified=false`, `discharge_observed=false` and `physical_shot_verified=false`. No screenshots, chat or account data are saved or uploaded.

One bounded background writer handles disk I/O. Storage/locking/format errors appear in the log and status line while calculations and Alt+I remain available. Corrupt or unsupported files are never silently replaced with empty history. Clean shutdown flushes accepted queued records.

## Predicting a new target

Local ranging remains primary. With observations from at least three independent targets at the same emplacement and arc, the application also evaluates a provisional shared MIL correction. Repeating one target cannot replace independent targets. Three groups are only a minimum: a line or plane of observations cannot support off-plane targets; a volume at different heights normally requires at least four suitably distributed groups.

Each target group contributes one median absolute correction. Each entire target is then excluded in turn from training. Validation compares the baseline and candidate **integer MIL commands** in retained sight-table metres. Median, RMS and maximum error must improve; no held-out target may worsen. This measures consistency with observations, not physical hit probability.

Transfer is restricted to the jointly supported range/bearing/height region, model-elevation limits and supported sight settings. Outside that region the baseline remains available. Target-local correction takes priority. Main and mini-card labels identify the provisional prediction; it is already included in the displayed command.

Pause, weapon or arc changes withdraw transfer until fresh independent observations exist. Map/gun/reset changes clear the active calibration. Changing the approximate low-trajectory height-correction setting also resets ranging so observations from different baseline models cannot mix. Alt+I works in both modes; no fixed 50 MIL correction is added. The archive never restores an old vehicle pose or automatically transfers corrections between sessions. If the body attitude changes without an observable position change, set the gun again or reset corrections as before.

## HUD stays disabled

No background sight OCR, Windows Graphics Capture, left-button shot subscription or colored alignment frame is reintroduced. Collection uses existing accepted actions. Capture pause still suspends game capture and the reticle while Build helper and Team Piker remain independent.

[Accuracy limits and log analysis](SPH2-VALIDATION-EN.md) · [Normal workflow](USAGE-EN.md)
