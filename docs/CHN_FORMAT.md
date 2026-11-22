# `.chn` chain format and tuning parameters

Everything below was verified by editing real `.chn` files and observing the result in game
with this overlay. Values are from a coat chain (Jill Cos2 mod), compared against the hair
chain of the retail game (Sheva).

## File layout

```
0x00  Header      magic "CHN\0", version 0x80618, data_size u32, chain_count u8, pad u8[3]
0x10  ChainInfo   chain_count records, 0x20 bytes each
      +0x00  joint_info_num    u8
      +0x01  chain_type        u8   1 = chain, 2 = shake
      +0x04  joint_info_offset u32  relative to this ChainInfo
      +0x10  grav_scaler       f32
      +0x14  time_scaler       f32
      +0x18  dump_scaler       f32
...   JointInfo   0x60 bytes per joint
```

Modded files can declare more than one chain where the extra ones have zero joints; the
runtime joint count is better derived from the `JointWork` block (see the debugger).

## JointInfo

| offset | field | type | meaning |
|---|---|---|---|
| `+0x00` | `attr` | u32 | flag bits, see below |
| `+0x04` | `ref_joint_no` | u16 | bone function id the joint is attached to |
| `+0x06` | `node_num` | u8 | number of graph links (0..8) |
| `+0x10` | `min_angle[3]` | 3×f32 | lower angle limit per axis, **radians** |
| `+0x1C` | `spring_coef` | f32 | stiffness pulling back to the rest pose |
| `+0x20` | `max_angle[3]` | 3×f32 | upper angle limit per axis |
| `+0x2C` | `dumping_coef` | f32 | damping (0..1) |
| `+0x30` | `mass` | f32 | joint mass |
| `+0x34` | `restrict_flag` | u8 | **1 = apply the angle limits** |
| `+0x35` | `col_shape` | u8 | 0 sphere, 1 capsule, 2 aabb, 3 obb |
| `+0x38` | `col_radius` | f32 | joint collider radius, cm |
| `+0x3C` | `air_regist` | f32 | air resistance |
| `+0x40` | `node_joint_no[8]` | 8×u16 | graph links — these are **`ref_joint_no` values, not array indices** |
| `+0x48` | `friction` | f32 | friction |
| `+0x4C` | `soft_coef` | f32 | constraint softness |
| `+0x50` | `bend_regist` | f32 | resistance to bending between links |

### `attr` flags

| bit | name | meaning |
|---|---|---|
| `0x1` | CONSTRAINT | anchor: the joint follows its bone rigidly and is not simulated |
| `0x2` | ATTACHED | the joint is attached to the skeleton and simulated |
| `0x4` | NOROTATE | do not rotate (unused in the coat chain) |

Observed combinations: `1`, `2`, `3`. Retail hair has all roots as `3`; the coat had ten
joints with plain `1`.

**Anchored joints (`attr & 1`) ignore every simulation parameter** — limits, spring, damping.
If a value "does not change anything", check `attr` first.

## Verified behaviour

### Angle limits only work with `restrict_flag = 1`

- `restrict_flag = 0` → `min_angle`/`max_angle` are ignored entirely;
- units are radians, limits are per local axis;
- `±3.14` is not a limit, it is a full revolution;
- the coat file stores `±0.055 rad` (±3.15°) on 74 joints with the flag **off**; enabling the
  flag without rewriting the values freezes those joints ("wire");
- setting a sensible limit on one axis while leaving the others at ±0.055 still locks the
  joint — always set all three axes.

Clothing starting points (X/Z = swing, Y = twist):

| region | X | Y | Z |
|---|---|---|---|
| near pelvis | ±0.5..0.6 | ±0.25..0.3 | ±0.45..0.5 |
| middle | ±0.7 | ±0.35 | ±0.6 |
| hem | ±0.9 | ±0.4 | ±0.8 |

### `spring_coef` dominates the feel

- range 1..12 in shipped data;
- **10 is very stiff**: the joint is pulled to its rest orientation and the strand stands
  like a taut string;
- retail hair: 2..12, median 7;
- the coat: median 10 — that is where the "wire" feel came from;
- usable range for cloth: **2..4**.

### `dumping_coef`

- 0.2 = almost none, long oscillation;
- 0.6..0.9 = settles quickly;
- **1.0 is at the solver limit**: a few joints with 1.0 produced NaN for the whole chain
  (every joint became invalid, the overlay reported "no geometry"). Stay at ≤ 0.9.

### `air_regist`

- default 0.1; raising to 0.4..0.6 damps the swing without making the cloth feel heavy
  (unlike `dumping`);
- useful for long strands that wobble.

### `bend_regist`

- 0 = bending unrestricted;
- 1.0 on every joint → strands stay straight like rods when the character bends;
- for soft cloth use 0.2..0.4, and only on specific joints.

### `node_joint_no` graph

- stores **`ref_joint_no`**, not array indices (a common scripting mistake);
- each joint links to its neighbours along the strand; branching = several links;
- the coat has 16 strands: nine of seven joints, seven of two to three joints;
- **short strands (2–3 joints) cannot drape** — they are levers; only adding joints helps;
- cross-strand links are effectively unused (retail hair has none).

### `mass`, `friction`, `soft_coef`

- `mass` 1..5: more mass = more inertia (swings longer);
- `friction` typically 0; 0.1..0.3 reduces sliding;
- `soft_coef` 0.8 by default; tune last.

### `ChainInfo` globals

| field | effect |
|---|---|
| `grav_scaler` 1.0 | 0.5..0.6 → less sag and less spread |
| `time_scaler` 1.0 | < 1 → slower, calmer simulation |
| `dump_scaler` 1.0 | global damping |

## Recipes

| symptom | action |
|---|---|
| strands stand like wire | `spring` 2..4; check `restrict_flag` and the stored limits (never leave ±0.055); `bend_regist` ≤ 0.4 |
| strands fly out and wrap colliders | `damping` 0.6..0.8, `air_regist` 0.4..0.6, tighter limits near the roots |
| a long strand wobbles | `air_regist` 0.5..0.6, `damping` 0.8..0.9, limits X/Z ±0.5 |
| everything is frozen | some joint has `restrict_flag = 1` with tiny limits, or `spring` ≥ 10 |
| overlay reports "no geometry" | the chain holds NaN — check `damping = 1.0` and extreme `spring` |
| nothing changes on a joint | the joint is anchored (`attr & 1`) |

## Scripting note

```python
import struct
d = bytearray(open("pl1400.chn", "rb").read())
nj = d[0x10]
joff = struct.unpack_from("<I", d, 0x14)[0]
for j in range(nj):
    o = 0x10 + joff + 0x60 * j
    if struct.unpack_from("<I", d, o)[0] & 1:      # anchored: leave alone
        continue
    struct.pack_into("<f", d, o + 0x1C, 3.0)        # spring
    struct.pack_into("<f", d, o + 0x2C, 0.7)        # dumping
    d[o + 0x34] = 1                                 # restrict_flag
    struct.pack_into("<3f", d, o + 0x10, -0.7, -0.3, -0.6)   # min
    struct.pack_into("<3f", d, o + 0x20,  0.7,  0.3,  0.6)   # max
```

After editing, verify that `attr`, `ref_joint_no` and `node_joint_no` are byte-identical to
the original. `node_num` is at `+0x06`, not `+0x02` — writing it in the wrong place
corrupts `attr` and produces broken deformation.
