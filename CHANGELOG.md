# Changelog

All notable changes to this project. Dates are local build dates.

## 1.0.1 — CCL stability and degenerate joints

- CCL primitives no longer disappear while the character moves: the model-bone proximity
  gate was measured against a centroid refreshed every 120 frames (and was only 300 cm), so
  every primitive was skipped once the character left that window. The centroid is now
  refreshed every frame and the gate is 2000 cm.
- Joints that the engine leaves at the world origin (missing bone for their `ref_joint_no`)
  are excluded from the calibration cloud; previously one such joint inflated the 3D bounding
  box to thousands of centimetres and the calibration picked a wrong view matrix, which made
  the overlay drift (observed with Chris Safari). Such joints are drawn white and logged.
- New diagnostics: `diag: … refs resolved K/N modelJoints=…`,
  `ccl: N of M primitives unresolved`, `joint ref=N sits at the world origin`.

## 1.0.0 — first stable build

Stability work after the first release:

- log critical section is initialised in `DllMain`; the previous lazy initialisation raced
  with the scanner thread started at level unload and corrupted the heap;
- every chain field is read through `ReadProcessMemory` (the last direct readers were the
  `ccl:` diagnostic line, `ChainStillAlive`, `JointAt` and the model-joint search);
- vectored exception handler logs fatal exceptions with module + offset and a return-address
  trace, and records swallowed first-chance violations inside the ASI;
- the experimental `DrawIndexedPrimitive` capture stays in the source but is **not**
  installed: it was the only change that reproduced a crash on level unload;
- projection guard no longer re-switches conventions after the calibration locked.

Verified: six consecutive level → menu transitions without a crash.

## 0.10.0 — stable projection (branch `feature/stable-projection`)

- `IDirect3DDevice9::DrawIndexedPrimitive` is hooked and the shader constants are snapshotted
  at draw time, one snapshot per frame;
- the snapshot is accepted only when its implied camera continues the previous frame
  (< 60 cm jump), which rejects shadow/reflection/post passes;
- projection uses the captured matrices; the constant-block calibration remains as a
  fallback when no draw is captured;
- calibration with a captured frame only chooses the handedness (screen-centre prior).

## 0.9.0

First public snapshot. The tool already carries the whole investigation behind it, so the
list below is grouped by area rather than by version.

### Overlay
- chain joints drawn from `JointWork` world matrices, graph links from `node_joint_no`
  (resolved through `ref_joint_no`), CCL spheres/capsules/aabb/obb with perspective-correct
  screen radii;
- drawing moved to `Present` (RE5 calls `EndScene` several times per frame);
- F7 toggles the overlay, F8 toggles diagnostics.

### Chain discovery
- `uChain`/`uChainEx` vtable scan over private heap only, validated by `rChain` magic;
- selection by distance to the camera, re-evaluated while playing;
- joint count from the contiguous `JointWork` block (handles modded `.chn` files with a
  second empty chain and `0xCD` filler);
- scanner restarts automatically after a level change.

### Collision
- CCL primitives resolved through the **model** bones (`uModel + 0x318`, stride `0x90`,
  matrix `+0x50`, function id byte `+0x07`) because `mpColJointWMat` is null on PC;
- numeric `PEN` report of joints inside primitives, printed to the log.

### Projection
- view/projection captured from vertex shader constants `c4..c6` and `c7/c8`;
- one-shot calibration using two third-person priors (centre and projected size);
- per-frame sanity guard against collapsed (line/point) projections with hysteresis;
- documented fallback to `c4 / rows / sign -1`;
- `sCamera` path implemented, verified against a process dump and removed.

### Diagnostics
- F8: joint colouring by deviation, link colouring by stretch ratio against a rest-length
  table, `ref` id labels for the worst links;
- log lines `LINK …` and `DEV …`, faster `PEN` cadence;
- explicit `draw: N joints but no geometry on screen` warning when the simulation produces
  invalid values.

### Stability
- all level-owned data read through `ReadProcessMemory`;
- drawing gated on a per-frame liveness check, state reset after 120 bad frames;
- logging never blocks the render thread;
- `IDirect3DDevice9::Reset` drops every cache.

### Documentation
- `docs/CHN_FORMAT.md`: verified `.chn` layout, every tuning parameter and what it does,
  recipes for common symptoms;
- `docs/ARCHITECTURE.md`: hooking, discovery, projection and threading details.
