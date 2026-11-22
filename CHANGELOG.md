# Changelog

All notable changes to this project. Dates are local build dates.

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
