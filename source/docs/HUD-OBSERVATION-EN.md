# SPH-2 sight observation

This document describes the experimental workflow included in 2.13.0. It retains the sight actually read before a fire input and helps associate it with a later Alt+I correction. The owner must verify compatibility with the current game build in the field. This workflow does not measure a hit percentage.

## When observation runs

Observation requires enabled game integration, SPH-2, a confirmed map, accepted gun and target coordinates, an available arc, and a foreground WARDOGS window. Capture and recognition run on a worker with one HUD frame in flight. Observation is disabled in the standalone manual calculator, diagnostic mode, while paused, and for L81. Showing manual coordinate fields does not itself change the operating mode.

The next read accounts for elapsed OCR time and capture age so ordinary updates can complete before the reading expires. Partial reads retain a slower retry cadence. Recognition that is too slow is not presented as fresh or continuously retried just to preserve a coloured border.

The observer uses screen pixels and text recognition. It does not aim, generate input, or read game-process memory. Window caption and class checks restrict capture but do not authenticate a game process. Screen capture uses standard [Microsoft GDI APIs](https://learn.microsoft.com/en-us/windows/win32/gdi/capturing-an-image).

## What is measured

The reader accepts a supported optic layout with actual reticle strokes, matching left and right tick rows, numeric labels, and the inner bearing ruler. The left ladder is RNG in metres; the right ladder is MIL. The reader checks the printed 10-MIL and 15-degree grids and interpolates at detected pixel anchors. RNG remains a displayed range reading; it is not converted into a fallback MIL value.

[Windows OCR](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrengine?view=winrt-26100) returns text and word positions. OCR geometry needs independent validation: nonzero `TextAngle` can place coordinates in a rotated image, as explained by [BoundingRect](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrword.boundingrect?view=winrt-26100). Ambiguous labels, unverified geometry, and excessive uncertainty cause refusal. Screen centre and the calculator's last command cannot substitute for a missing sight.

Ammunition is a separate channel beneath an identified SPH-2 single-shell icon. The machine-gun counter is excluded. Available stabilization and ASL labels remain separate observations. MIL is the HUD reading, not a verified physical barrel angle. Bearing, MIL, and reticle position are not measurements of chassis attitude.

## Fire input and Alt+I

1. Two distinct fresh frames must agree on the sight and context. Time is recorded before capture, rather than after recognition.
2. A physical left-button press freezes an available sight captured **before the press**. The click alone cannot create an observed discharge. Later aiming changes cannot overwrite the frozen values.
3. A verified counter decrement of exactly one needs a second independent frame with the same count. This is an observed association between input and HUD. The counter may change during reload; its relationship to the physical firing instant is unverified. When reload hides the reticle, a separate counter reading can corroborate an already frozen sight but cannot provide a new one.
4. Alt+I requires one uniquely eligible outstanding observation with the same map, base, arc, HUD generation, target, and target height. Multiple observations, a new unresolved press, or a changed target prohibit substituting the latest shot.
5. Capture retains observation identity; recording validates it again. A failed correction releases the reservation. The observation is consumed only after an accepted correction, and repeated completion cannot consume it twice.

## Time limits

These are application policies, not measured flight or reload times.

| Check | Default limit |
| --- | --- |
| Age of latest HUD capture | 1 second |
| Separation of agreeing frames | 40 milliseconds to 2.5 seconds |
| Agreement of readings | 0.05 degrees and 0.5 MIL |
| Input confirmation window | 15 seconds |
| Outstanding impact association | 120 seconds |
| Queue capacity | 8 observations |

Absolute bearing and MIL reading uncertainties are retained without reduction. Missing uncertainty remains unknown. Frame agreement does not establish coordinate accuracy or a landing within 10 metres.

## Refusal and reset

Pause, disabled integration, a map/base/arc change, a ranging reset, or switching to L81 cancels the old session. Foreground loss, region selection, busy coordinate recognition, or a missing HUD forbids new sight snapshots while retaining already frozen observations. An unreadable ammunition value is unknown evidence. Conflicting counts, replenishment, or multiple possible presses create ambiguity. One previous HUD frame cannot reliably distinguish a rapid map click from a firing intention; a click alone never confirms firing.

Until the first complete sight is accepted, the legacy correction remains available with `command_source=displayed_unverified`. Partially read ammunition or stabilization does not activate strict shot association or block that path. The first complete sight starts a fresh counter sequence. An accepted correction using actual HUD readings starts a separate history. Assumed commands and HUD measurements are not mixed. After transition to HUD evidence, a missing sight cannot be replaced by an assumed command until an explicit context reset. An unknown layout or incompatible HUD version does not receive verified-reading status.

## Mini-card border

The border compares a fresh observed sight with the calculator's current rounded command, including accepted ranging corrections. It does not predict a hit or control the gun. No additional user setting is required.

| Colour | Condition |
| --- | --- |
| Green | Two agreeing frames and known enabled stabilization; the actual reading interval intersects the current command's rounding cell (0.1 degree and one MIL) |
| Orange | A reliably read smaller difference, or known disabled stabilization |
| Red | Even after allowing for OCR uncertainty, difference exceeds 1 degree or 10 MIL |
| Neutral | No complete sight, unknown stabilization or uncertainty, a frame older than one second, changed context, paused application, or lost game focus |

A label repeats the colour state. Its tooltip shows bearing and MIL adjustments toward the command; positive values mean an increase from the current reading. Border updates do not change the card's dimensions or position. Changing the target, map, or arc clears an old green state.

Green means the command is compatible with the sight reading within the measured uncertainty shown in the tooltip. Uncertainty is not subtracted or reduced; a frame with more than one degree or one MIL of uncertainty receives no colour assessment. The rounding allowance is half the displayed step: 0.05 degree and 0.5 MIL. This does not establish readiness for a precise hit. Pixel-localisation uncertainty on the real ruler exceeds 0.2 degree even when central values match perfectly; a worst-error requirement of 0.2 degree would make green unreachable.

Inner-compass labels require verified individual OCR-word coordinates and alignment with the corresponding major tick within 3 pixels at a 720-pixel client height. Recognition-region geometry is never presented as word position. The top navigation ruler cannot replace the inner compass. Labels with a different phase relative to the ticks cause refusal.

When Windows OCR finds no words in the full strip, a bounded dark-neutral component search locates candidates in the original pixels. Each candidate is read with two padding variants; text and word coordinates must agree. More than 12 candidates, clipped glyphs or inconsistent readings cause refusal. This path does not infer numbers from ticks or relax ruler validation. A separate timer expires visual indication independently of the next OCR operation; it reads no pixels.

## Evidence limits

The final offline replay of 16 retained original game images (18 console probes) produced no complete accepted AZ+MIL sight. Some fields can be read, but other labels are occluded, use a different layout or lack a verified tick association. Colour examples come from explicitly synthetic UI tests. They do not establish the complete reader path on the current game; that stage remains unfinished.

The log separates `sight_verified`, `discharge_observed`, and `physical_shot_verified`. For this HUD/counter association the last flag remains `0`: the application does not authenticate a physical shot, the identity of a selected impact, or a hit probability. Earlier `displayed_unverified` records do not prove that the player entered the displayed command or establish “99% within 10 metres.”

The measured [MZ scale](https://github.com/Firepanda415/MZ-Wardogs/blob/5a60171796a24eb650e2202a9773a9890a17c48c/docs/sph2-scale.md) documents 73 screenshots and 264 pixels per 10 MIL at 3840×2160. This is HUD observation; the author explicitly limits conclusions about barrel angle and landings. Reader checks use individual retained public-video frames covering a limited set of layouts. Some frames have no visible game build number. These frames, synthetic tests, and a controlled test window do not establish current game physics or compatibility across all resolutions, FOV settings, and versions.

The technical contract is in [shot_observation.hpp](../include/wardogs/shot_observation.hpp); pixel and label interpretation is in [sph_hud.cpp](../src/sph_hud.cpp). Field verification needs linked actual pre-press sight → observed counter transition → selected impact under an unchanged context, retaining uncertainty and capture timestamps.
