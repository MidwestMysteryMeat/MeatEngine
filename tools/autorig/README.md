# autorig — headless Blender auto-rigger

Rigs an **un-rigged humanoid mesh** to a **standard Mixamo-named skeleton** and
exports a rigged FBX, so the animation clips already in `assets/anim_packs/**`
play on the new character without any retargeting.

```
blender --background --python tools/autorig/autorig.py -- \
    --mesh  path/to/character.fbx \
    --out   path/to/character_rigged.fbx \
    [--template "assets/anim_packs/Action_Adventure_Pack/Rigged Male Low-Poly NPC.fbx"] \
    [--fit uniform|hips] [--no-triangulate] [--keep-interior] [--verbose]
```

Exit code `0` on success, non-zero on any failure (bad args, missing mesh,
import failure, collapsed weighting, export failure). All progress is printed
with an `[autorig]` prefix.

Supported mesh inputs: `.fbx .obj .glb .gltf .ply .stl` (dispatched by extension).
Output is always FBX.

---

## Why this approach (and not a neural auto-rigger)

Neural auto-riggers (RigNet, etc.) emit an **arbitrary skeleton** with arbitrary
bone names and joint counts. The MeatEngine anim clips are Mixamo clips keyed to
**specific bone names** (`mixamorig:Hips`, `mixamorig:Spine/Spine1/Spine2`,
`Neck`, `Head`, `Left/RightShoulder|Arm|ForeArm|Hand`,
`Left/RightUpLeg|Leg|Foot`, and mirrors). A clip only drives a bone whose name
it matches, so an arbitrary skeleton plays **nothing**.

So instead of inventing a skeleton, we take a **fixed, correctly-named template
skeleton** and fit it into each mesh:

1. **Template** — import the Mixamo rig the clips were authored for (every anim
   pack ships `Rigged Male Low-Poly NPC.fbx`; that is the default template).
   Its bone names *and rest-pose bone orientations* are exactly what the clips
   expect. A hardcoded bone table (`FALLBACK_BONES` in `autorig.py`) is the
   last-resort fallback if no template FBX can be imported.
2. **Fit** — uniform-scale + place that skeleton inside the target mesh.
3. **Bind** — `bpy.ops.object.parent_set(type='ARMATURE_AUTO')` runs Blender's
   bone-heat solver to auto-weight the mesh to the embedded skeleton.

Because the bone names and per-bone local axes come straight from the reference
rig and are only moved by a **rigid transform + uniform scale** (which preserves
every bone's roll/local frame), the Mixamo clips remain valid. Verified: the
exported rig shares **100 %** of its bone names with `idle.fbx` from the pack.

---

## Pipeline stages

### 1. Mesh import + cleanup (critical for bone heat)
The bone-heat solver fails on dirty geometry, so we clean first (all logged with
before/after counts):

- unparent (keep transform) + strip any existing rig/vertex-groups so we rebind
  from scratch;
- **merge by distance** (weld coincident verts — the single most common cause of
  `failed to find solution`);
- **delete loose** verts/edges;
- **recalculate normals** outward;
- **remove interior faces** (hidden inner shells confuse the volumetric solver;
  skip with `--keep-interior`);
- **triangulate** (PSX-friendly and heat-solver-friendly; skip with
  `--no-triangulate`);
- report the **non-manifold edge count** so you can see how watertight the shell is.

### 2. Fit heuristic
Assumes the mesh is **upright and roughly T/A-posed** (per the task spec). Blender's
FBX/glTF importers land the model **Z-up**, which the fit relies on.

- **up = world Z**; stature = mesh bounding-box Z extent.
- **lateral axis** = the wider of the mesh's two horizontal extents (X vs Y) —
  for a T/A-pose humanoid that is the arm span. The template is rotated about Z
  (0° or 90°) so its own lateral axis (measured from the shoulder offset) lines
  up with the mesh's, so skeleton arms sit over mesh arms.
- **uniform scale** so template stature == mesh stature (`--fit uniform`,
  default), *or* so the hips land at 50 % of stature (`--fit hips`, best ground
  contact). Both are **uniform** and therefore anim-safe — non-uniform per-axis
  scaling would shear the diagonal arm/leg bones' roll and corrupt the clips, so
  it is deliberately not done.
- **placement**: feet seated on the mesh floor (min Z), hips centered on the mesh
  X/Y center.
- Landmarks (hips %, shoulder %, hand-to-hand span vs mesh width, fitted rig
  bbox) are logged so you can judge fit quality at a glance.

### 3. Bind
Mesh + armature selected with the **armature active**, then
`parent_set(type='ARMATURE_AUTO')`. Afterwards the tool verifies weights actually
landed (deform bones with weight, unweighted vert count, presence of the Armature
modifier). Zero usable weights is fatal; partial coverage is a warning.

### 4. Export
`export_scene.fbx` with:

| setting | value | why |
|---|---|---|
| `axis_up` / `axis_forward` | `Y` / `-Z` | Blender Z-up → FBX Y-up, matching how Mixamo/Unity store FBX and how the clips are encoded |
| `apply_scale_options` | `FBX_SCALE_ALL` | bakes unit + axis scaling so assimp reads the same metres the clips use (MeatEngine FBX law) |
| `add_leaf_bones` | `False` | no synthetic `_end` leaf bones → clean assimp node chain |
| `primary/secondary_bone_axis` | `Y` / `X` | Mixamo bone-axis convention |
| `use_mesh_modifiers` | `False` | keep the raw skinned mesh; don't bake the armature modifier |
| `bake_anim` | `False` | rig only; clips live in their own files |
| `object_types` | `{ARMATURE, MESH}` | export exactly the rig + mesh, nothing else |

---

## Failure modes

- **`Bone Heat Weighting: failed to find solution for one or more bones`** —
  Blender prints this at the C level; the tool captures it (file-descriptor
  redirect) and reports it. Causes: duplicate/coincident verts, non-manifold or
  interior geometry, zero-area faces, or a bone lying outside the mesh volume.
  The cleanup pass fixes the first three automatically. Affected verts fall back
  to nearest-bone weighting (non-fatal); a *total* collapse (no bone gets any
  weight) is treated as fatal.
- **Mesh not upright / not Z-up** — the fit assumes Z-up + upright. A mesh lying
  on its side, or an `.obj` exported with a non-standard up-axis, will be scaled
  to the wrong dimension. Re-export the source upright, or pre-rotate it, before
  running. (FBX/glTF from Blender/Maya/Mixamo are already Z-up on import.)
- **Extreme non-humanoid proportions** — the arm-span-is-widest and
  hips-at-50 %-stature heuristics assume a humanoid. Props/quadrupeds will bind
  but land badly; that is out of scope.
- **No template + no fallback names match** — if the given/candidate FBX has no
  `mixamorig:` bones the tool builds the hardcoded fallback skeleton instead.

## Fingers / extremities
The default template carries the full 41-bone Mixamo rig (incl. fingers). On very
low-poly PSX meshes some fingertip/toe-tip bones may get **no** weights (no nearby
verts) — this is expected and harmless: those bones simply stay at rest, and any
clip that keys them is unaffected on the bones that *are* weighted.

## Tested
Blender **5.1.2** (also present: portable 5.0.1 under `F:\Blender`). Verified on
`Rigged Male Low-Poly NPC` (559 verts): produced a 41-bone `mixamorig:` rig,
skinned mesh (34 vertex groups + armature modifier), 0 unweighted verts, and
**100 % bone-name overlap with the pack's `idle.fbx`** clip.
