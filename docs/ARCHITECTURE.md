# Architecture

Single-file ASI (`src/dllmain.cpp`, ~1700 lines, C++ with a plain Win32/D3D9 surface).
Everything runs inside the game process; nothing is written to disk except the log.

## 1. Bootstrapping

The packed executable of this game breaks IAT-based hooks, so the entry point is the
exported `Direct3DCreate9` inside `d3d9.dll`:

1. load `d3d9.dll` with `LoadLibrary`, resolve `Direct3DCreate9`,
2. install a 5-byte `jmp` to our hook, keeping the original bytes,
3. on every call restore the original bytes, call through, patch again.

This is safe with multiple devices and survives re-creation. From the returned
`IDirect3D9` we hook `CreateDevice` (vtable index 16), and from the device:

| method | vtable | purpose |
|---|---|---|
| `Present` | 17 | draw once per frame |
| `Reset` | 16 | drop every cached pointer |
| `SetVertexShaderConstantF` | 94 | capture `c0..c23` |
| `EndScene` | 42 | pass-through (drawing there produced 3–5 draws per frame) |

`Present` is the only place that touches drawing state, and it is skipped when
`TestCooperativeLevel()` is not `D3D_OK`.

## 2. Finding the chain

A background thread scans committed, readable memory with `ReadProcessMemory`, so a region
that disappears mid-scan is an error, not a crash. Only the game image is excluded. There are
two passes:

- **narrow** (fast path, class-object scan): `MEM_PRIVATE`, `<= 64 MB`, `base >= 32 MB`;
- **wide** (structural `rChain` scan): every readable region that is not the game image,
  including `MEM_MAPPED` data and large private pools.

The wide pass exists because the packed build keeps its level data in regions the narrow
filter skipped (a 64 MB cap or `MEM_PRIVATE`-only filter made the structural scan report
`magic=0` while the game was running). The log reports what was covered:
`scan: N MB in K regions (rpe fails …, image skipped … MB), magic=… versioned=… rejected=…`
plus a one-shot `mem: private/mapped/image …` summary.

The engine builds the `uChain` / `uChainEx` class objects at runtime, so their addresses are
a property of the **build**, not of the executable on disk. The tool keeps one pair per known
build (`g_chainVtProfiles`):

| build | `uChain` | `uChainEx` | how it was established |
|---|---|---|---|
| 1.10 (2018, 15 046 608 B) | `0x010B789C` | `0x00F6DEEC` | carried over from v2 (the retail runtime image) |
| 1.20 (2017, 14 306 816 B) | `0x010B0E48` | `0x00F6DC54` | disassembled `uChain` ctor at `0x222A30` (the 1.10 ctor is at `0x211CD0`) |
| retail Enigma-packed (8 195 416 B) | `0x00F6D534` | — | read from a chain found by the structural route on that build |

When the structural route selects a chain, its class-object value is remembered
(`g_learnedVt`), so the fast path works for that build on every later scan (level change)
without a table entry.

A candidate is a heap dword equal to either value of any profile. It is validated:

```
uChain + 0x140 -> resource with magic "CHN\0" at +0x60 and version 0x80618 at +0x64
```

Because the class-object addresses are build data, a profile is only a shortcut: when none of
them exists in the process (the read returns 0 and the scanner logs `not this build`), the
thread falls back to the **structural route** — find the `CHN\0` / `0x80618` header in the
heap, then every heap pointer to it, and treat `pointer − 0x140` as the `uChain` base. That
path needs no build-specific address and also covers the packed/protected executable, whose
code is decrypted before the game reaches the level.

Candidates are scored by distance from the camera to the strand centroid (the player chain
is the closest one); the choice is re-evaluated every 120 frames. The scan thread exits once
a chain is selected and is restarted after the level unloads (detected by 120 consecutive
frames where the chain fails its liveness check). Liveness is checked by the `rChain` magic,
not by the class-object address, so a build without a matching profile is not mistaken for an
unloaded level.

Joint count is *not* taken from `ChainInfo` (modded `.chn` files often declare an empty
second chain and leave `0xCD` filler bytes). Instead the first `JointWork` block is walked
with stride `0x1D0` until three consecutive invalid records appear (max 128).

`JointWork` layout used here:

```
+0x010  mWorldMat   4x4, translation in row 3
+0x1C4  mRefJointNo u32 (low 16 bits used)
```

## 3. Model joints and CCL primitives

`mpColJointWMat` (`+0x250`) is null at runtime on PC, so chain-side matrices cannot be used.
Primitives are attached to **model** bones instead:

```
uChain + 0x210 -> uModel
uModel + 0x318 -> joint array, stride 0x90
joint record: +0x07 function id (byte), +0x50 world matrix
```

The array is discovered structurally: for every field of the model that points to readable
memory, try a few strides and matrix offsets and count how many consecutive records have a
translation near the strand centroid (150 cm). One readability check per candidate keeps the
search cheap; the whole discovery runs on the background thread, never inside `Present`.

Each `ColInfo` (0x40 bytes at `rChainCol + 0x70`) is transformed by the matrix of its
`ref` bone; capsules use the `pair` bone for the second endpoint.

## 4. Camera and projection

### Draw-time capture (primary source)

`IDirect3DDevice9::DrawIndexedPrimitive` (vtable 82) is hooked. At the moment of a draw, the
shader constants that were uploaded last belong to **that** draw, so a snapshot taken there
is the real matrix set of the pass being recorded - no guessing from the end of the frame.

- one snapshot per frame (the first qualifying draw after the previous `Present`);
- the candidate is accepted only if the camera it implies **continues** the previous frame's
  camera (jump < 60 cm). Shadow, reflection and post passes teleport the camera, so they are
  rejected;
- the accepted snapshot (`g_use`) is used for projection during the following `Present`;
- calibration then only has to pick the handedness, which is done once with the
  screen-centre prior.

### Fallback: constant-block calibration

If `DrawIndexedPrimitive` is never captured (different driver, overlay loaded late, etc.) the
tool falls back to the end-of-frame constant blocks:

The engine uploads the view matrix several times per frame (main pass, shadow, reflection,
post). The tool captures the **last** upload of each constant register per frame, then:

1. **one-shot calibration**: try every `reg x {rows, columns} x {handedness}` and rank by two
   priors that hold for a third-person camera:
   - the strand centroid must project near the screen centre,
   - the projected size must match `diagonal * fx * (W/2) / cameraDistance`;
2. **sanity guard** every 30 frames: reject a projection that collapses the cloud into a line
   or a point, and switch convention only if the alternative is clearly better (1.4x);
3. **fallback** to `c4 / rows / sign -1` if nothing matches.

Screen radius of primitives is derived from perspective
(`radius * fx * (W/2) / depth`), which is invariant to camera rotation.

An `sCamera`-based path (view `+0x100`, projection `+0x140`) was implemented and then
removed: a process dump showed no object with a *translated* view matrix at those offsets.

## 5. Safety and threading

- All level-owned data is read via `ReadProcessMemory` (`SRead`/`SRd32`); freed pages return
  errors instead of raising access violations.
- Drawing is skipped entirely unless `ChainStillAlive()` passes **this** frame.
- Logging uses `TryEnterCriticalSection` and drops the message when contended, so it can
  never stall a frame.
- The scanner only touches private heap, sleeps 10 s between attempts, and stops after a
  successful selection.

## 6. Diagnostics mode (F8)

- joint colour = distance to its bone (green ≤5 cm, orange ≤12, yellow ≤25, red above);
- link colour = stretch ratio against the rest length table (`src/rest_links.h`), magenta
  above 1.8x, plus the `ref` ids drawn next to the worst links;
- `PEN` report every 120 frames instead of every 1200;
- `LINK …` lines with `len`, `bone`, `rest` and `x` for every suspicious link.

Note: the chain writes its simulated transforms back into the skeleton, so "joint vs bone"
distance is close to zero by construction — it is a validity check, not a stretch metric.
Stretch must be measured against the rest pose (the table in `rest_links.h`, generated from
the model skeleton).
