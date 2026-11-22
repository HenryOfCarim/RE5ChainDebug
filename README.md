# re5-chaindbg

In-game debug overlay for **Resident Evil 5 (PC)** chain simulation and CCL collision
primitives. It draws the real simulated strand joints, the strand graph and the collision
shapes on top of the running game, and writes a numeric penetration report to a log file.

Built for reverse-engineering / modding work: when a hair or coat strand clips through the
body, folds into a spike or starts vibrating, this shows *why* — with actual per-joint data
from the running process, not from a Blender approximation.

```
green points   chain joints (JointWork world positions)
green lines    strand graph (JointInfo.node_joint_no)
red circles    CCL sphere primitives
yellow circles CCL capsule primitives
violet circles CCL aabb/obb primitives
corner square  red = chain not found yet, green = chain found
```

With **F8** the overlay switches to diagnostics mode: joints are coloured by their distance
to the skeleton, strand links are coloured by how much they are stretched relative to the
rest pose, and the ids (`ref_joint_no`) of the worst joints are drawn next to them.

## Requirements

| | |
|---|---|
| game | Resident Evil 5 PC (Steam), `re5dx9.exe` **1.10**, 15 046 608 bytes, MD5 `553fdbb0afb20ac73b8ee44bd9361128` |
| loader | any 32-bit ASI loader (e.g. `dinput8.dll` style loader or a mod manager that supports ASI) |
| build | i686 GCC (TDM-GCC / mingw-w64), no external libraries |

> The packed Enigma-protected build of the game has different addresses and is not
> supported: the tool locates objects by vtable, but the fixed offsets below are from 1.10.

## Install

1. Copy `re5_chaindbg.asi` next to the ASI loader (not necessarily the game folder —
   wherever your loader picks ASI files up).
2. Start the game and load into a **level** (no chains exist in the main menu).
3. Press **F7** to toggle the overlay, **F8** to toggle diagnostics.

Log file: `re5_chaindbg.log` next to `re5dx9.exe`, otherwise in `%TEMP%`.

## Controls

| key | action |
|---|---|
| `F7` | toggle the overlay (off by default) |
| `F8` | toggle diagnostics: per-joint deviation colours, stretch colouring, `ref` labels, faster `PEN` reports |

## Building

```sh
# Linux / MSYS2 cross build
make

# Windows, TDM-GCC
build.bat

# or directly
g++ -m32 -shared -O2 -Wall -static-libgcc -static-libstdc++ -o re5_chaindbg.asi src/dllmain.cpp
```

No D3DX, no ImGui, no runtime dependencies beyond `d3d9.dll`/`kernel32`.

## Log reference

| line | meaning |
|---|---|
| `base=… delta=…` | module base and its delta from 0x400000 (ASLR) |
| `d3d9 Direct3DCreate9 patched` / `intercepted` | `d3d9.dll` export hook installed |
| `scan thread started` / `scan thread done` | background chain search (restarted after a level change) |
| `vtable route: N candidate object(s)` | objects whose vtable is `uChain`/`uChainEx` |
| `vtable cand … joints=… prims=… camDist=…` | candidate with its chain/resource statistics |
| `uChain chosen …` | selected chain (nearest to the camera) |
| `counts: res=… hdr=… nch=…` | `rChain` header |
| `counts: joints=N (chaininfo=… block=…) prims=M` | joint/primitive counts actually resolved |
| `model: joints at model+0x… stride=0x… mat=+0x…` | discovered model joint array |
| `projection: view=cN mode=M sign=S cost=… spread=…` | locked camera constants |
| `projection: switching to … (min axis … px)` | projection sanity guard switched convention |
| `projection: fallback c4/-1` | no candidate passed, documented pair used |
| `perf: … fps (… ms/frame), show=… draw=… ms` | performance, once per 120 frames |
| `PEN jNN ref=… pen=+0.42 INSIDE` | joint inside a primitive (`pen` is depth in cm) |
| `PEN summary: N joints, K inside` | summary |
| `LINK a-b len=… bone=… rest=… x=…` | strand link length, bone distance and stretch ratio (F8) |
| `chain gone for 120 frames, resetting` | level unloaded; state reset and scanner restarted |
| `device reset - dropping cached chain` | `IDirect3DDevice9::Reset` cleared caches |
| `draw: N joints but no geometry on screen` | the chain currently holds invalid (NaN) values |

## How it works (short version)

1. `Direct3DCreate9` is patched inside the `d3d9.dll` export with a 5-byte jump (IAT hooks
   are not used: the packed executable breaks them). `CreateDevice`, `Present`, `Reset` and
   `SetVertexShaderConstantF` are hooked through their vtables.
2. A background thread scans **private heap** memory for `uChain`/`uChainEx` vtables, then
   validates each candidate through `mpChain` → `rChain` magic `CHN\0` and version `0x80618`.
3. The chain nearest to the camera is selected (the player), joints are counted from the
   contiguous `JointWork` block (stride `0x1D0`), not from `ChainInfo`.
4. CCL primitives come from `mpCollision` → `rChainCol`; each primitive is attached to a
   **model bone**, found structurally (`uModel + 0x318`, stride `0x90`, matrix `+0x50`,
   function id at byte `+0x07`). `mpColJointWMat` is null on PC, so chain-side matrices are
   not usable.
5. The camera is read from the vertex shader constant blocks (`c4..c6` view, `c7/c8`
   projection) with a one-shot calibration and a per-frame sanity guard. Drawing happens in
   `Present`, once per frame (`EndScene` is called several times per frame in RE5).
6. Everything that can disappear with the level is read through `ReadProcessMemory`, so an
   unloaded level returns errors instead of crashing the game.

More detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
The `.chn` format and every tuning parameter: [docs/CHN_FORMAT.md](docs/CHN_FORMAT.md).

## Verified offsets (PC 1.10)

| field | offset |
|---|---|
| `uChain.mpChain` | `+0x140` |
| `uChain.mChainInfoNum` | `+0x144` |
| `uChain.mpJointWork` | `+0x14C` (array per chain, each element a contiguous `JointWork` run) |
| `uChain.mpColModel` | `+0x210` |
| `uChain.mpCollision` | `+0x218` |
| `uChain.mColInfoNum` | `+0x21C` |
| `uChain.mColTimeStamp` | `+0x220` |
| `uChain.mpCollisionWork` | `+0x224` (0x120 bytes per primitive) |
| `uChain.mpColJointWMat` | `+0x250` — **null in game**, unused |
| `JointWork.mWorldMat` | `+0x10` |
| `JointWork.mRefJointNo` | `+0x1C4` |
| `rChain.mpChainInfo` | `+0x70` (`ChainInfo` 0x20) |
| `rChainCol.mpColInfo` | `+0x70` (`ColInfo` 0x40) |
| `rChain`/`rChainCol` header | `+0x60` |
| `uModel` → joint array | `model + 0x318`, stride `0x90`, matrix `+0x50`, function id `+0x07` |

## Limitations

- exe 1.10 only.
- Main menu has no chains — the indicator stays red there, that is expected.
- If several characters carry chains, the nearest to the camera wins.
- The projection calibration may flip convention in an unusual scene; the sanity guard
  corrects it, but a single frame may jump.
- Short strands (2–3 joints) cannot drape: the simulation treats them as levers.

## Repository layout

```
src/dllmain.cpp     single-file implementation
src/rest_links.h    rest lengths of the chains of one test mod (diagnostics only)
docs/               architecture and .chn format notes
Makefile             cross build (mingw-w64)
build.bat            Windows build (TDM-GCC)
```

## License

MIT — see [LICENSE](LICENSE).
