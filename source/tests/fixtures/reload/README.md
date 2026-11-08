# Reload arrow-shape fixtures

- `user-sph2-arrow-crop.png` is the unmodified image supplied by the project owner on November 8, 2026. It contains up/right/down/down arrows, with the first two green. The one-frame tests recolor only independently annotated arrow bodies to white in memory.
- `live-sph2-neutral-arrows.png` is a 335 x 420 crop from the running game. It contains left/right/up/down white arrows. The surrounding timer and barrel remain as distractors: tests replace the surrounding pixels with white, black, sky and random colors without changing the expected directions.
- `reload-panel-down-up-up-left.png` is a 211 x 58 strip from the later user attachment. It contains down/up/up/left white arrows. Its gray rectangle is incidental; production recognition does not require a rectangle or circle.
- `live-panel-progress-0.png` is a 220 x 63 live GDI crop containing left/down/right/right white arrows.

These crops exclude player names, application overlays and desktop content. Production reads exactly four arrow shapes in one evenly spaced row from a single image. Shape similarity and directional separation remain mandatory; a rectangular frame, circular timer and weapon silhouette are not prerequisites. The helper schedules the four inputs separately from image recognition.

Tests cover the real white arrow rows at scales 0.75–3, different surrounding colors, manual arrow-only crops, full-client search, independent outlined synthetic arrows on light/dark/colored interiors, ambiguous rows, non-arrow icons and clipped or malformed images. They do not establish completion timing or success of an in-game reload.
