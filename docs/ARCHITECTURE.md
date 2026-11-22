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

A background thread scans **private committed memory only** (`MEM_PRIVATE`, region
`<= 64 MB`, `base >= 0x02000000`; the module image and file mappings are skipped). Reads go
through `ReadProcessMemory`, so a region that disappears mid-scan is an error, not a crash.

A candidate is a dword equal to `uChainEx` (`0x00F6DEEC`) or `uChain` (`0x010B789C`) plus
`delta = moduleBase − 0x400000`. Each candidate is validated:

```
uChain + 0x140 -> resource with magic "CHN\0" at +0x60 and version 0x80618 at +0x64
```

Candidates are scored by distance from the camera to the strand centroid (the player chain
is the closest one); the choice is re-evaluated every 120 frames. The scan thread exits once
a chain is selected and is restarted after the level unloads (detected by 120 consecutive
frames where the chain fails its liveness check).

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
