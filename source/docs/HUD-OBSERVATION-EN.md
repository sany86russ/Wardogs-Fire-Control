# SPH-2 sight observation

This document describes HUD-reader development after the published 2.13.0 release. The installed 2.13.0 application was neither replaced nor restarted during qualification; new console-reader results apply to the working source. The workflow retains the sight actually read before a fire input and helps associate it with a later Alt+I correction. The complete installed mini-card → press → counter → Alt+I chain still needs separate verification. This workflow does not measure a hit percentage.

## When observation runs

Observation requires enabled game integration, SPH-2, a confirmed map, accepted gun and target coordinates, an available arc, and a foreground WARDOGS window. Capture and recognition run on a worker with one HUD frame in flight. Observation is disabled in the standalone manual calculator, diagnostic mode, while paused, and for L81. Showing manual coordinate fields does not itself change the operating mode.

The next read accounts for elapsed OCR time and capture age so ordinary updates can complete before the reading expires. A fresh complete result whose next update cannot meet continuous availability starts another independent read after a bounded 150-millisecond idle interval. Only one request runs at a time, without accumulating a queue. With processing taking 600–850 milliseconds, gaps between usable readings remain possible; a continuous border colour is not promised. Partial, refused and stale results retain a slower retry cadence. The one-second expiry remains unchanged and expired readings are cleared.

The observer uses window pixels and text recognition. It does not aim, generate input, or read game-process memory. Window caption and class checks restrict capture but do not authenticate a game process. HUD capture uses [Windows Graphics Capture for a specific HWND](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow) and D3D11 readback. Separate overlays belonging to other applications do not become game-sight pixels. GDI capture of map/chat coordinates and a custom region remains a separate unchanged path.

Each request creates and closes a WGC session and a two-frame pool. Only the device, capture item and readback buffer persist between requests; no capture session remains active while paused. HWND/process/thread identity, visibility, physical dimensions and the complete client crop are checked against observed DWM/window bounds. A closed capture item refuses the read and is recreated on the next request. Geometry is checked again after capture and before the Qt thread applies the result.

[SystemRelativeTime](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime?view=winrt-28000) supplies the compositor frame's QPC rendering time. It must be no earlier than the new request and no later than frame receipt. A cached client-DC image or desktop screenshot cannot replace this source. Unavailable WGC, invalid timing or a changed window produces an explicit refusal.

The request checks a 250-millisecond budget between system operations, frame waits and GPU readback. A cold driver initialization can itself return later: measured initial calls of 491/522 milliseconds were refused, while the initialized device remained cached for the next request. The budget does not promise to interrupt a blocked system call. Accepted HUD age retains the existing one-second limit.

## What is measured

The reader accepts a supported optic layout with actual reticle strokes, matching left and right tick rows, numeric labels, and the inner bearing ruler. The left ladder is RNG in metres; the right ladder is MIL. The reader checks the printed 10-MIL and 15-degree grids and interpolates at detected pixel anchors. RNG remains a displayed range reading; it is not converted into a fallback MIL value.

[Windows OCR](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrengine?view=winrt-26100) returns text and word positions. OCR geometry needs independent validation: nonzero `TextAngle` can place coordinates in a rotated image, as explained by [BoundingRect](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrword.boundingrect?view=winrt-26100). Ambiguous labels, unverified geometry, and excessive uncertainty cause refusal. Screen centre and the calculator's last command cannot substitute for a missing sight.

Ammunition is a separate channel beneath an identified SPH-2 single-shell icon. The machine-gun counter is excluded. Available stabilization and ASL labels remain separate observations. MIL is the HUD reading, not a verified physical barrel angle. Bearing, MIL, and reticle position are not measurements of chassis attitude.

A MIL/RNG word can be isolated through matching bright-component geometry at two thresholds. Recognition receives the original RGB pixels of the complete observed word with bounded surrounding margins; digits are not repaired. Weak reads remain unknown and increment `admission_rejections`. CTC retains the 0.90 mean and 0.70 minimum score thresholds; qualified complete conflicts still reach the strict parser. Acceptance continues to require at least two real labels with independently measured ticks bracketing the reticle, matching MIL/RNG rows and the existing uncertainty limits. Neighbour values, the calculator command and ballistic tables never determine expected text.

The primary Windows OCR engine retains `en-US`. Only the stabilization box additionally uses an installed Russian recognizer when available. Exact `Stabilized`/`Unstabilized` and `Стабилизировано`/`Не стабилизировано` captions are supported, without repairing OCR letters. Matching states from both language passes are merged; opposite states remain a conflict. An absent language pack or unreadable caption never implies enabled stabilization: the value remains unknown and the border neutral.

## Fire input and Alt+I

1. Two distinct fresh frames must agree on the sight and context. Each retains a lower source-time bound (`source_started_at_ms`, request start) and an upper bound (`timestamp_ms`, availability of the acquired frame before OCR). The two acquisition intervals must prove separation; recognizing one image again cannot create another frame.
2. A physical left-button press freezes an available sight captured **before the press**: its upper time bound must be strictly earlier than the press. Equal millisecond timestamps do not prove ordering and are refused. Age is checked from the lower bound, so late receipt cannot make old pixels appear newer. The click alone cannot create an observed discharge. Later aiming changes cannot overwrite the frozen values.
3. A verified counter decrement of exactly one needs a second independent frame with the same count. This is an observed association between input and HUD. The counter may change during reload; its relationship to the physical firing instant is unverified. When reload hides the reticle, a separate counter reading can corroborate an already frozen sight but cannot provide a new one.
4. Alt+I requires one uniquely eligible outstanding observation with the same map, base, arc, HUD generation, target, and target height. Multiple observations, a new unresolved press, or a changed target prohibit substituting the latest shot.
5. Capture retains observation identity; recording validates it again. A failed correction releases the reservation. The observation is consumed only after an accepted correction, and repeated completion cannot consume it twice.

The input adapter uses the original `MSLLHOOKSTRUCT.time` message timestamp, expanded against the 64-bit tick clock within a one-second dispatch bound. Future, unavailable or excessively delayed input timing quarantines the correlation; callback time is not substituted. Coalesced input retains any unavailable timestamp.

## Time limits

These are application policies, not measured flight or reload times.

| Check | Default limit |
| --- | --- |
| Latest HUD age from the lower source-time bound | 1 second |
| Checked WGC request budget | 250 milliseconds; a cold system call can finish later and be refused |
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

Inner-compass labels require the complete observed number and alignment with the corresponding real major tick within 3 pixels at a 720-pixel client height. Geometry comes from an individually verified unrotated OCR word or the distinct `pixel_glyph_union` evidence type: a union of actual source glyphs with the observed character count retained. Recognition-region geometry is never presented as word position. The top navigation ruler cannot replace the inner compass.

When the full Windows pass loses or truncates a digit, a bounded dark-neutral component search locates the complete source word. Two padding views must confirm its text and geometry. The CTC path uses a mask of actually observed glyphs in both views; character count, scores and agreement are checked separately. A qualified complete conflict is retained for parser refusal even when other words form a convenient grid. More than twelve candidates or clipped glyphs never become invented digits.

A word wholly outside the observed tick interval plus its fixed margin increments `unanchored_tokens` and cannot supply a ruler point. This geometric exclusion is independent of numeric value. Ambiguity between existing ticks still refuses the reading. Two admitted labels must bracket the pointer, retaining the 15-degree grid and uncertainty limit; extrapolation is prohibited. A separate timer expires visual indication independently of the next OCR operation; it reads no pixels.

Every printed compass major label must be a multiple of 15 degrees: equal differences alone cannot admit a consistently shifted OCR grid. OCR first reads the union of independently measured bracketing rows on both side ladders. A near-tick localisation difference can require three rows instead of two. Additional rows receive one recognition attempt; all obtained qualified conflicts remain visible to the parser.

Short HUD lines use the same Small model and two crop passes with reduced unused tensor padding. Source resampling and the default map/chat path remain unchanged.

## Diagnostics and replay

`hud.read_summary` records a named parser refusal, capture/OCR/geometry stages, frame dimensions, backend/QPC, processing time, conservative age bound, per-field attempt/token counts and interval counts of accepted/refused/stale results and errors. The first record is immediate; state changes are limited to one record per five seconds, and unchanged states recur every thirty seconds. Admission, unanchored-token and exhausted-budget counters appear in both the summary and console-probe JSON. An accepted HUD read does not verify a physical shot.

Normal runtime writes no screenshots or raw OCR text to files. Qualification frames are saved only by a separately invoked capture probe; personal images and receipts remain in a private dataset outside the public checkout. “Sight HUD unread” means the actual-sight comparison is unavailable; command calculation works independently.

The private replay contract can be checked separately:

```powershell
py -3.11 source/tests/test_hud_replay.py --self-test
py -3.11 source/tests/test_hud_replay.py --probe source/build/release/sph_hud_probe.exe --model source/assets/models/PP-OCRv6_rec_small.onnx --manifest "C:/private-hud/manifest.json" --report "C:/private-hud/replay-report.json"
```

`C:/private-hud` is an example private dataset location. The manifest pins model/image SHA256 hashes, scene context, manually verified readings and tolerance bounds. Qualification requires at least two distinct positive images with different nonoverlapping sight intervals, plus a negative scene. It checks actual native-probe outputs, probe hash, stage/refusal/pose consistency and unchanged input hashes after reading. A passing `--self-test`, absent positive cases or a set containing only refusals does not qualify native recognition. CTest takes a private manifest through `WARDOGS_HUD_REPLAY_MANIFEST`; default public CI receives no personal game images.

## Evidence limits

Historical 2.13.0 baseline: offline replay of sixteen retained original game images, using eighteen console probes, produced no complete accepted AZ+MIL sight. That receipt remains the recorded starting point; later successes do not rewrite it.

Development after 2.13 separately found that the client DC returned the same old image after aim changed. The new WGC backend acquired twelve fresh 2560×1440 frames in two series, taking 46–78 milliseconds after explicit cold-initialization refusal. At the `native-07` stage, the new console reader accepted two distinct sight poses at a 1440-pixel height, with recognition taking approximately half a second. This is bounded proof of fresh capture and offline AZ+MIL, not a final count of every replay case or proof of the installed mini-card and complete fire/impact chain. Final qualification belongs to a specific hash manifest and native-replay receipt. UI colour examples remain explicitly synthetic.

The log separates `sight_verified`, `discharge_observed`, and `physical_shot_verified`. For this HUD/counter association the last flag remains `0`: the application does not authenticate a physical shot, the identity of a selected impact, or a hit probability. Earlier `displayed_unverified` records do not prove that the player entered the displayed command or establish “99% within 10 metres.”

The measured [MZ scale](https://github.com/Firepanda415/MZ-Wardogs/blob/5a60171796a24eb650e2202a9773a9890a17c48c/docs/sph2-scale.md) documents 73 screenshots and 264 pixels per 10 MIL at 3840×2160. This is HUD observation; the author explicitly limits conclusions about barrel angle and landings. Reader checks use individual retained public-video frames covering a limited set of layouts. Some frames have no visible game build number. These frames, synthetic tests, and a controlled test window do not establish current game physics or compatibility across all resolutions, FOV settings, and versions.

The technical contract is in [shot_observation.hpp](../include/wardogs/shot_observation.hpp); fresh capture is in [game_client_capture.cpp](../src/game_client_capture.cpp), and pixel/label interpretation is in [sph_hud.cpp](../src/sph_hud.cpp) and [sph_hud_ocr.cpp](../src/sph_hud_ocr.cpp). Field verification needs linked actual pre-press sight → observed counter transition → selected impact under an unchanged context, retaining uncertainty and both capture-time bounds.
