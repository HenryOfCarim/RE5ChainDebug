# Changelog

All notable changes to this project. Dates are local build dates.

## 1.1.3 — fast discovery on the packed build

- The packed build's class object (`0x00F6D534`) is now a profile, so the fast path finds its
  chains directly instead of waiting for a structural scan.
- A class object learned by the structural route is remembered (`g_learnedVt`), so rescans
  after a level change stay on the fast path.
- The class-object pass now uses the wide region filter too; the old narrow filter skipped
  the large pool the packed build allocates its objects from.
- Loaded rChain resources are cached: retries repeat only the pointer pass, and the cache is
  dropped when a level change makes it stale.
- All resources are matched in a single pointer pass instead of one pass per resource, and
  the retry interval is 5 s instead of 10–30 s.
- Log: `reusing N cached rChain resource(s)`, `learned uChain class object …`,
  `scan: no uChain points to the cached resources`.

## 1.1.2 — candidates are scored by their joints

- **Fix (why the chain was still not found after 1.1.1):** `InitJoints()` limited its walk to
  `g_chainJoints`, which is still 0 while the first candidate is being scored. Every
  structural candidate therefore counted 0 joints, and since `mpColJointWMat` is null on PC
  the score was 0 and the real chain was rejected. The log shows exactly that:
  `rChain resource #0 … chn resources=2` followed by `cand … joints=0 score=0`.
- `ScoreChain()` now weighs joints and only uses the (usually null) col-matrix array as a
  tie-breaker; a candidate needs at least one valid `JointWork` to be accepted.
- If a candidate scores 0, `AutoLayout()` re-probes the `JointWork**` / col-matrix offsets and
  the candidate is rescored; a rejected candidate is hexdumped once (`rej+000: …`) so the
  next log shows what the object actually is.

## 1.1.1 — the packed build's memory is actually searched

- **Fix (why only the red square was drawn on the Enigma-packed exe):** the structural pass
  only looked at `MEM_PRIVATE` regions of at most 64 MB above 32 MB. A log from the packed
  build (`image 00400000-01D29000`) showed `scan: 226 MB … magic=0` — the loaded `rChain`
  resources were outside that filter, so nothing was ever found.
- The structural pass is now wide: every committed readable region except the game image,
  including `MEM_MAPPED` data and large private pools. The class-object (fast path) scan keeps
  the narrow filter so it stays cheap.
- Diagnostics: one-shot `mem: private/mapped/image` summary, per-scan coverage
  (`… MB in … regions, rpe fails, image skipped`), and `magic … rejected: version …` when a
  header exists but its engine version is not supported.
- Documentation updated for the two passes.

## 1.1.0 — multi-build support (1.10, 1.20, protected exe)

- `uChain` / `uChainEx` are created by the engine at runtime, so their addresses are build
  data. They moved into a profile table (`g_chainVtProfiles`) with the 1.10
  (`0x010B789C` / `0x00F6DEEC`) and 1.20 (`0x010B0E48` / `0x00F6DC54`) pairs; the latter was
  read from the `uChain` constructor at `0x222A30` of the 14 306 816-byte build.
- **Fix:** `ChainStillAlive()` required the exact profile address, so on any build with a
  different address the chain was declared dead every 120 frames and the overlay never drew.
  Liveness now checks the `rChain` magic (and a plausible class pointer) only.
- The scanner logs the image range and, per profile, whether its class objects exist in the
  process (`usable` / `not this build`); an unknown build falls back to the structural
  `rChain` route instead of relying on a profile.
- `docs/ARCHITECTURE.md` documents the profile table and how each address was established.

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
