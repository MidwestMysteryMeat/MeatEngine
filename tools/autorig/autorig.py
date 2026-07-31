#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
autorig.py -- headless Blender auto-rigger for MeatEngine.

Rigs an un-rigged humanoid MESH to a STANDARD Mixamo-named skeleton and exports
a rigged FBX, so the engine's existing Mixamo animation clips (which target the
"Rigged Male Low-Poly NPC" rig shipped in every anim pack) play on it unchanged.

Pipeline (see README.md for the full rationale):
  1. Import the target mesh (fbx/obj/gltf/glb/ply/stl), join parts, apply xforms.
  2. Clean the mesh so bone-heat weighting can find a solution
     (merge-by-distance, recalc normals, delete loose, remove interior, triangulate).
  3. Load a FIXED Mixamo-named template armature (from an existing rigged FBX; a
     hardcoded bone table is the last-resort fallback).
  4. Fit the template to the mesh: canonicalize orientation, uniform-scale to the
     mesh height, center horizontally, seat the feet on the mesh floor.
  5. Bind: parent_set(type='ARMATURE_AUTO')  (bone-heat auto weights), with the
     "failed to find solution" error captured and reported.
  6. Export a Y-up rigged FBX.

Usage:
  blender --background --python tools/autorig/autorig.py -- \
      --mesh <in.(fbx|obj|glb|gltf|ply|stl)> --out <out.fbx> [--template <rig.fbx>]
      [--fit uniform|hips] [--no-triangulate] [--keep-interior] [--verbose]

Exit codes: 0 = success, non-zero = failure (bad args, import fail, bind fail, etc).

NOTE: this file lives entirely under tools/autorig/ and touches nothing else in
the repo. It only reads other assets (templates) read-only.
"""

import os
import sys
import argparse
import traceback

import bpy
import bmesh
from mathutils import Vector, Matrix

# --------------------------------------------------------------------------- #
# Defaults
# --------------------------------------------------------------------------- #

# The rig every MeatEngine anim pack ships with -- the clips are authored for it,
# so it is the correct default template (identical bone names + rest orientation).
DEFAULT_TEMPLATE_CANDIDATES = [
    r"F:\MeatEngine\assets\anim_packs\Action_Adventure_Pack\Rigged Male Low-Poly NPC.fbx",
    r"F:\MeatEngine\assets\models\npc_char.fbx",
]

# Last-resort fallback: a clean, symmetric Mixamo core skeleton expressed directly
# in the pipeline's canonical frame -- Blender Z-up, metres, feet on Z=0, the
# lateral (arm-span) axis along +X, facing -Y.  Captured from the low-poly rig's
# world-space rest pose.  (head, tail, parent).  Fingers are intentionally omitted:
# clips that animate absent bones simply skip them, and fewer tiny bones means
# far fewer bone-heat failures on low-poly PSX meshes.
FALLBACK_BONES = [
    # name,                    head (x,y,z),            tail (x,y,z),            parent
    ("mixamorig:Hips",          (0.000, 0.001, 1.005), (0.000, 0.001, 1.119), None),
    ("mixamorig:Spine",         (0.000, 0.008, 1.109), (0.000, 0.015, 1.231), "mixamorig:Hips"),
    ("mixamorig:Spine1",        (0.000, 0.015, 1.231), (0.000, 0.023, 1.370), "mixamorig:Spine"),
    ("mixamorig:Spine2",        (0.000, 0.023, 1.370), (0.000, 0.033, 1.523), "mixamorig:Spine1"),
    ("mixamorig:Neck",          (0.000, 0.033, 1.526), (0.000, 0.033, 1.626), "mixamorig:Spine2"),
    ("mixamorig:Head",          (0.000, 0.004, 1.622), (0.000, 0.004, 1.841), "mixamorig:Neck"),
    ("mixamorig:HeadTop_End",   (0.000, -0.059, 1.832), (0.000, -0.059, 2.052), "mixamorig:Head"),
    ("mixamorig:LeftShoulder",  (0.068, 0.033, 1.506), (0.205, 0.034, 1.467), "mixamorig:Spine2"),
    ("mixamorig:LeftArm",       (0.205, 0.034, 1.467), (0.440, 0.019, 1.307), "mixamorig:LeftShoulder"),
    ("mixamorig:LeftForeArm",   (0.440, 0.019, 1.307), (0.676, -0.066, 1.147), "mixamorig:LeftArm"),
    ("mixamorig:LeftHand",      (0.676, -0.066, 1.147), (0.738, -0.080, 1.107), "mixamorig:LeftForeArm"),
    ("mixamorig:RightShoulder", (-0.068, 0.033, 1.506), (-0.205, 0.032, 1.467), "mixamorig:Spine2"),
    ("mixamorig:RightArm",      (-0.205, 0.032, 1.467), (-0.440, 0.019, 1.307), "mixamorig:RightShoulder"),
    ("mixamorig:RightForeArm",  (-0.440, 0.019, 1.307), (-0.676, -0.065, 1.147), "mixamorig:RightArm"),
    ("mixamorig:RightHand",     (-0.676, -0.065, 1.147), (-0.742, -0.082, 1.100), "mixamorig:RightForeArm"),
    ("mixamorig:LeftUpLeg",     (0.103, 0.009, 0.947), (0.149, 0.024, 0.508), "mixamorig:Hips"),
    ("mixamorig:LeftLeg",       (0.149, 0.024, 0.508), (0.178, 0.046, 0.101), "mixamorig:LeftUpLeg"),
    ("mixamorig:LeftFoot",      (0.178, 0.046, 0.101), (0.181, -0.107, 0.002), "mixamorig:LeftLeg"),
    ("mixamorig:LeftToeBase",   (0.181, -0.107, 0.002), (0.181, -0.187, 0.002), "mixamorig:LeftFoot"),
    ("mixamorig:LeftToe_End",   (0.181, -0.187, 0.002), (0.181, -0.267, 0.001), "mixamorig:LeftToeBase"),
    ("mixamorig:RightUpLeg",    (-0.103, 0.008, 0.947), (-0.149, 0.024, 0.508), "mixamorig:Hips"),
    ("mixamorig:RightLeg",      (-0.149, 0.024, 0.508), (-0.178, 0.049, 0.101), "mixamorig:RightUpLeg"),
    ("mixamorig:RightFoot",     (-0.178, 0.049, 0.101), (-0.181, -0.106, 0.002), "mixamorig:RightLeg"),
    ("mixamorig:RightToeBase",  (-0.181, -0.106, 0.002), (-0.181, -0.185, 0.002), "mixamorig:RightFoot"),
    ("mixamorig:RightToe_End",  (-0.181, -0.185, 0.002), (-0.181, -0.263, 0.001), "mixamorig:RightToeBase"),
]

# Bones used to measure fit landmarks (canonical fallback names; matched by suffix
# so a "mixamorig1:" prefix or similar still resolves).
LM = {
    "hips":     "Hips",
    "shoulderL": "LeftShoulder",
    "shoulderR": "RightShoulder",
    "handL":    "LeftHand",
    "handR":    "RightHand",
    "footL":    "LeftFoot",
    "footR":    "RightFoot",
    "headtop":  "HeadTop_End",
}

log_lines = []


def log(msg):
    print("[autorig] " + str(msg))
    log_lines.append(str(msg))


class RigError(Exception):
    pass


# --------------------------------------------------------------------------- #
# stdout fd capture -- Blender prints the bone-heat warning at the C level, so a
# Python-side redirect is not enough; we dup the real file descriptor 1.
# --------------------------------------------------------------------------- #

class CapturedStdout:
    def __init__(self):
        self.text = ""

    def __enter__(self):
        sys.stdout.flush()
        self._fd = sys.stdout.fileno()
        self._saved = os.dup(self._fd)
        self._tmp = os.path.join(
            bpy.app.tempdir or os.path.expanduser("~"), "autorig_capture.txt")
        self._f = open(self._tmp, "w+")
        os.dup2(self._f.fileno(), self._fd)
        return self

    def __exit__(self, *exc):
        sys.stdout.flush()
        os.dup2(self._saved, self._fd)
        os.close(self._saved)
        self._f.seek(0)
        try:
            self.text = self._f.read()
        except Exception:
            self.text = ""
        self._f.close()
        try:
            os.remove(self._tmp)
        except OSError:
            pass
        # echo what was captured so it still lands in the log
        if self.text.strip():
            for line in self.text.splitlines():
                print("    | " + line)
        return False


# --------------------------------------------------------------------------- #
# Scene helpers
# --------------------------------------------------------------------------- #

def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def set_active(obj):
    bpy.context.view_layer.objects.active = obj


def only_select(obj):
    for o in bpy.data.objects:
        o.select_set(False)
    obj.select_set(True)
    set_active(obj)


def import_any(path):
    """Import a mesh file by extension. Returns list of newly created objects."""
    ext = os.path.splitext(path)[1].lower()
    before = set(bpy.data.objects)

    def _try(calls):
        last = None
        for fn in calls:
            try:
                fn()
                return True
            except Exception as e:  # noqa
                last = e
        if last:
            raise last
        return False

    if ext == ".fbx":
        _try([lambda: bpy.ops.import_scene.fbx(filepath=path)])
    elif ext == ".obj":
        _try([lambda: bpy.ops.wm.obj_import(filepath=path),
              lambda: bpy.ops.import_scene.obj(filepath=path)])
    elif ext in (".gltf", ".glb"):
        _try([lambda: bpy.ops.import_scene.gltf(filepath=path)])
    elif ext == ".ply":
        _try([lambda: bpy.ops.wm.ply_import(filepath=path),
              lambda: bpy.ops.import_mesh.ply(filepath=path)])
    elif ext == ".stl":
        _try([lambda: bpy.ops.wm.stl_import(filepath=path),
              lambda: bpy.ops.import_mesh.stl(filepath=path)])
    else:
        raise RigError("unsupported mesh extension: %s" % ext)

    return [o for o in bpy.data.objects if o not in before]


def apply_transforms(obj):
    only_select(obj)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def world_bbox(obj):
    ws = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    mn = Vector((min(v.x for v in ws), min(v.y for v in ws), min(v.z for v in ws)))
    mx = Vector((max(v.x for v in ws), max(v.y for v in ws), max(v.z for v in ws)))
    return mn, mx


# --------------------------------------------------------------------------- #
# Mesh import + cleanup
# --------------------------------------------------------------------------- #

def prepare_mesh(path, do_triangulate=True, remove_interior=True):
    objs = import_any(path)
    meshes = [o for o in objs if o.type == 'MESH']
    if not meshes:
        raise RigError("no mesh found in %s" % path)

    # Join all mesh parts into one object.
    if len(meshes) > 1:
        for o in bpy.data.objects:
            o.select_set(False)
        for m in meshes:
            m.select_set(True)
        set_active(meshes[0])
        bpy.ops.object.join()
        log("joined %d mesh parts into one" % len(meshes))
    mesh = meshes[0]

    # If the mesh is parented (e.g. we were handed an already-rigged FBX to
    # re-rig), unparent it KEEPING its world transform -- otherwise deleting the
    # parent armature silently drops the armature's unit/scale, collapsing the
    # mesh into raw local units.
    if mesh.parent is not None:
        only_select(mesh)
        bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')

    # Drop any non-mesh objects that rode along (stray armatures/empties/cameras)
    # and any existing skinning so we rebind from scratch.
    for o in list(bpy.data.objects):
        if o is not mesh and o.type != 'MESH':
            bpy.data.objects.remove(o, do_unlink=True)
    mesh.vertex_groups.clear()
    for m in list(mesh.modifiers):
        if m.type == 'ARMATURE':
            mesh.modifiers.remove(m)

    apply_transforms(mesh)

    v0 = len(mesh.data.vertices)
    f0 = len(mesh.data.polygons)

    bm = bmesh.new()
    bm.from_mesh(mesh.data)

    # 1. merge by distance (weld coincident verts -- the #1 cause of bone-heat fail)
    n_before = len(bm.verts)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-4)
    n_merged = n_before - len(bm.verts)

    # 2. delete loose geometry (verts/edges with no face)
    loose_verts = [v for v in bm.verts if not v.link_faces]
    n_loose = len(loose_verts)
    if loose_verts:
        bmesh.ops.delete(bm, geom=loose_verts, context='VERTS')
    loose_edges = [e for e in bm.edges if not e.link_faces]
    if loose_edges:
        bmesh.ops.delete(bm, geom=loose_edges, context='EDGES')

    # 3. recalc normals consistently outward
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

    # 4. triangulate (PSX-friendly + heat solver likes triangles)
    if do_triangulate:
        bmesh.ops.triangulate(bm, faces=bm.faces[:])

    bm.to_mesh(mesh.data)
    bm.free()
    mesh.data.update()

    # 5. interior-face removal + non-manifold report use edit-mode ops.
    interior_removed = 0
    nonmanifold = _mesh_edit_pass(mesh, remove_interior)

    v1 = len(mesh.data.vertices)
    f1 = len(mesh.data.polygons)

    log("mesh cleanup:")
    log("    verts %d -> %d   (merged %d doubles, removed %d loose)"
        % (v0, v1, n_merged, n_loose))
    log("    polys %d -> %d   (triangulate=%s, interior-removed=%d)"
        % (f0, f1, do_triangulate, nonmanifold.get('interior', 0)))
    log("    non-manifold edges after cleanup: %d %s"
        % (nonmanifold.get('nonmanifold', -1),
           "(watertight-ish, good for bone heat)"
           if nonmanifold.get('nonmanifold', -1) == 0 else
           "(open shell -- usually still fine; heat solves per-vertex)"))

    return mesh


def _mesh_edit_pass(mesh, remove_interior):
    """Interior-face removal + non-manifold count via edit-mode ops.
    Returns dict; degrades gracefully if an op is unavailable in background."""
    out = {}
    only_select(mesh)
    try:
        bpy.ops.object.mode_set(mode='EDIT')
    except Exception as e:
        log("    (edit-mode unavailable, skipping interior/manifold pass: %s)" % e)
        return out

    bm = bmesh.from_edit_mesh(mesh.data)

    if remove_interior:
        f_before = len(bm.faces)
        try:
            bpy.ops.mesh.select_all(action='DESELECT')
            bpy.ops.mesh.select_interior_faces()
            bpy.ops.mesh.delete(type='FACE')
            bm = bmesh.from_edit_mesh(mesh.data)
            out['interior'] = f_before - len(bm.faces)
        except Exception as e:
            out['interior'] = 0
            log("    (interior-face removal skipped: %s)" % e)

    # count non-manifold edges
    try:
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_non_manifold()
        bm = bmesh.from_edit_mesh(mesh.data)
        out['nonmanifold'] = sum(1 for e in bm.edges if e.select)
    except Exception:
        out['nonmanifold'] = -1

    bpy.ops.object.mode_set(mode='OBJECT')
    return out


# --------------------------------------------------------------------------- #
# Template armature
# --------------------------------------------------------------------------- #

def load_template_armature(template_path):
    """Return an armature object with Mixamo bone names, in Z-up world space.
    Tries the given/candidate FBX paths, then the hardcoded fallback table."""
    tried = []
    paths = []
    if template_path:
        paths.append(template_path)
    paths.extend(DEFAULT_TEMPLATE_CANDIDATES)

    for p in paths:
        if not p or not os.path.isfile(p):
            tried.append("%s (missing)" % p)
            continue
        try:
            arm = _import_template_fbx(p)
            if arm:
                log("template armature: imported from %s (%d bones)"
                    % (p, len(arm.data.bones)))
                return arm
        except Exception as e:
            tried.append("%s (%s)" % (p, e))
    log("template FBX import unavailable (%s) -- building from hardcoded table"
        % "; ".join(tried) if tried else "no paths")
    return _build_fallback_armature()


def _import_template_fbx(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    arms = [o for o in new if o.type == 'ARMATURE']
    if not arms:
        for o in new:
            bpy.data.objects.remove(o, do_unlink=True)
        raise RigError("no armature in template")
    arm = arms[0]
    # sanity: needs Mixamo-ish names
    if not any("Hips" in b.name for b in arm.data.bones):
        for o in new:
            bpy.data.objects.remove(o, do_unlink=True)
        raise RigError("armature is not Mixamo-named")
    # discard the template's own mesh + extras, keep only the armature
    for o in new:
        if o is not arm:
            bpy.data.objects.remove(o, do_unlink=True)
    apply_transforms(arm)   # bake into Z-up world so bone_local == world
    return arm


def _build_fallback_armature():
    arm_data = bpy.data.armatures.new("mixamorig_template")
    arm = bpy.data.objects.new("Armature", arm_data)
    bpy.context.scene.collection.objects.link(arm)
    only_select(arm)
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm_data.edit_bones
    made = {}
    for name, head, tail, parent in FALLBACK_BONES:
        b = eb.new(name)
        b.head = Vector(head)
        b.tail = Vector(tail)
        made[name] = b
    for name, head, tail, parent in FALLBACK_BONES:
        if parent:
            made[name].parent = made[parent]
    bpy.ops.object.mode_set(mode='OBJECT')
    log("built fallback armature (%d bones)" % len(FALLBACK_BONES))
    return arm


def find_bone(arm, suffix):
    for b in arm.data.bones:
        if b.name == "mixamorig:" + suffix or b.name.endswith(":" + suffix) or b.name == suffix:
            return b
    return None


def bone_world_head(arm, bone):
    return arm.matrix_world @ bone.head_local


# --------------------------------------------------------------------------- #
# Fit
# --------------------------------------------------------------------------- #

def fit_armature(arm, mesh, mode="uniform"):
    """Uniform-scale + place the template into the mesh.  Anim-safe: a rigid
    transform plus uniform scale preserves every bone's local axes/roll, so the
    Mixamo clips keep playing correctly."""
    mn, mx = world_bbox(mesh)
    dims = mx - mn
    center = (mn + mx) * 0.5

    # Up axis assumed Z (FBX importer convention; task assumes upright mesh).
    mesh_h = dims.z
    # widest horizontal axis of the mesh == arm span in a T/A-pose humanoid.
    mesh_lat_axis = 0 if dims.x >= dims.y else 1   # 0=X, 1=Y
    mesh_lat = max(dims.x, dims.y)

    # --- template measurements (world, before fit) ---
    hips = find_bone(arm, LM["hips"])
    shL = find_bone(arm, LM["shoulderL"])
    footL = find_bone(arm, LM["footL"])
    top = find_bone(arm, LM["headtop"])
    if not (hips and footL):
        raise RigError("template missing Hips/Foot landmark bones")

    t_min_z = min((arm.matrix_world @ b.head_local).z for b in arm.data.bones)
    t_min_z = min(t_min_z, min((arm.matrix_world @ b.tail_local).z for b in arm.data.bones))
    t_max_z = max((arm.matrix_world @ b.tail_local).z for b in arm.data.bones)
    t_height = t_max_z - t_min_z
    t_hip_z = bone_world_head(arm, hips).z - t_min_z

    # template lateral axis = axis of the shoulder offset from centreline.
    if shL:
        sh = bone_world_head(arm, shL)
        t_lat_axis = 0 if abs(sh.x) >= abs(sh.y) else 1
    else:
        t_lat_axis = 0

    # --- choose uniform scale ---
    if mode == "hips":
        # match hip height precisely (best ground contact); still uniform.
        mesh_hip = mn.z + 0.5 * mesh_h        # hips ~ 50% of stature
        scale = (mesh_hip - mn.z) / max(t_hip_z, 1e-6)
    else:
        scale = mesh_h / max(t_height, 1e-6)

    # --- rotation about Z so template lateral axis aligns to mesh lateral axis ---
    rot = Matrix.Identity(4)
    if t_lat_axis != mesh_lat_axis:
        rot = Matrix.Rotation(1.5707963 if mesh_lat_axis == 1 else -1.5707963, 4, 'Z')

    # Compose: scale about origin, rotate about Z, then translate feet->floor & centre.
    S = Matrix.Diagonal((scale, scale, scale, 1.0))
    arm.matrix_world = rot @ S @ arm.matrix_world

    # recompute feet + centre after scale/rot, then translate into place
    mn2, mx2 = _arm_world_bounds(arm)
    ctr2 = (mn2 + mx2) * 0.5
    delta = Vector((center.x - ctr2.x, center.y - ctr2.y, mn.z - mn2.z))
    arm.matrix_world = Matrix.Translation(delta) @ arm.matrix_world

    # --- report fit landmarks vs mesh ---
    log("fit (mode=%s): scale x%.4f  rot-Z=%s  lateral mesh-axis=%s template-axis=%s"
        % (mode, scale, "yes" if t_lat_axis != mesh_lat_axis else "no",
           "XY"[mesh_lat_axis], "XY"[t_lat_axis]))
    log("    mesh stature=%.3f  bbox dims X=%.3f Y=%.3f Z=%.3f" % (mesh_h, dims.x, dims.y, dims.z))
    _report_landmark(arm, mesh, mn, mx, mesh_h, mesh_lat)


def _arm_world_bounds(arm):
    pts = []
    for b in arm.data.bones:
        pts.append(arm.matrix_world @ b.head_local)
        pts.append(arm.matrix_world @ b.tail_local)
    mn = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    mx = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    return mn, mx


def _report_landmark(arm, mesh, mn, mx, mesh_h, mesh_lat):
    def z_of(suffix):
        b = find_bone(arm, suffix)
        return bone_world_head(arm, b).z if b else None
    hip = z_of(LM["hips"])
    shL = z_of(LM["shoulderL"])
    if hip is not None:
        log("    hips  at %.1f%% stature (target ~50%%)" % (100 * (hip - mn.z) / mesh_h))
    if shL is not None:
        log("    shldr at %.1f%% stature (target ~82%%)" % (100 * (shL - mn.z) / mesh_h))
    # arm span
    hL = find_bone(arm, LM["handL"]); hR = find_bone(arm, LM["handR"])
    if hL and hR:
        span = (bone_world_head(arm, hL) - bone_world_head(arm, hR)).length
        log("    hand-to-hand span=%.3f  vs mesh width=%.3f (ratio %.2f)"
            % (span, mesh_lat, span / max(mesh_lat, 1e-6)))
    an, ax = _arm_world_bounds(arm)
    log("    fitted rig bbox: X[%.2f,%.2f] Y[%.2f,%.2f] Z[%.2f,%.2f]"
        % (an.x, ax.x, an.y, ax.y, an.z, ax.z))


# --------------------------------------------------------------------------- #
# Bind
# --------------------------------------------------------------------------- #

def bind(mesh, arm):
    # armature must be ACTIVE, mesh also selected.
    for o in bpy.data.objects:
        o.select_set(False)
    mesh.select_set(True)
    arm.select_set(True)
    set_active(arm)

    log("binding: parent_set(type='ARMATURE_AUTO')  (bone-heat auto weights)")
    with CapturedStdout() as cap:
        try:
            bpy.ops.object.parent_set(type='ARMATURE_AUTO')
        except Exception as e:
            raise RigError("parent_set failed: %s" % e)
    heat_failed = "failed to find solution" in cap.text.lower()

    # verify weights actually landed
    deform = [b.name for b in arm.data.bones if b.use_deform]
    groups = {vg.name for vg in mesh.vertex_groups}
    weighted_bones = 0
    total_w = [0.0] * len(mesh.data.vertices)
    name_to_idx = {vg.name: vg.index for vg in mesh.vertex_groups}
    for b in deform:
        if b in name_to_idx:
            idx = name_to_idx[b]
            has = False
            for v in mesh.data.vertices:
                for g in v.groups:
                    if g.group == idx and g.weight > 0.0:
                        total_w[v.index] += g.weight
                        has = True
            if has:
                weighted_bones += 1
    unweighted = sum(1 for w in total_w if w <= 0.0)

    # confirm the Armature modifier exists
    has_mod = any(m.type == 'ARMATURE' for m in mesh.modifiers)

    log("bind result: %d/%d deform bones got weights; %d/%d verts unweighted; armature-modifier=%s"
        % (weighted_bones, len(deform), unweighted, len(mesh.data.vertices), has_mod))

    if heat_failed:
        log("WARNING: Blender reported \"Bone Heat Weighting: failed to find solution "
            "for one or more bones\".")
        log("  CAUSE: the solver could not weight some verts -- almost always duplicate/"
            "coincident verts, non-manifold or interior geometry, zero-area faces, or a "
            "bone lying outside the mesh volume.")
        log("  FIX (this tool already applies the first three): merge-by-distance, "
            "recalc normals, delete loose + interior faces, triangulate; ensure the mesh "
            "is a single closed-ish shell and the skeleton sits inside it. As a last "
            "resort those verts keep the nearest-bone fallback weight Blender assigns.")

    # Treat total weighting collapse as fatal; partial heat-fail is a warning only.
    if weighted_bones == 0 or not has_mod:
        raise RigError("binding produced no usable weights (heat solver collapsed)")
    if unweighted > 0:
        log("NOTE: %d verts fell back to nearest-bone weighting (non-fatal)." % unweighted)
    return heat_failed


# --------------------------------------------------------------------------- #
# Export
# --------------------------------------------------------------------------- #

def export_fbx(mesh, arm, out_path):
    out_dir = os.path.dirname(os.path.abspath(out_path))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    for o in bpy.data.objects:
        o.select_set(False)
    mesh.select_set(True)
    arm.select_set(True)
    set_active(arm)

    log("exporting FBX -> %s" % out_path)
    bpy.ops.export_scene.fbx(
        filepath=out_path,
        use_selection=True,
        object_types={'ARMATURE', 'MESH'},
        # Mixamo / Unity-standard axis conversion: Blender Z-up -> FBX Y-up.
        axis_forward='-Z',
        axis_up='Y',
        # FBX_SCALE_ALL keeps the FbxAxisSystem + unit scaling baked so assimp
        # reads the same metres the clips use (MeatEngine FBX law).
        apply_scale_options='FBX_SCALE_ALL',
        global_scale=1.0,
        use_mesh_modifiers=False,     # keep the raw skinned mesh; don't bake the armature mod
        add_leaf_bones=False,         # no synthetic _end leaves -> cleaner assimp node chain
        primary_bone_axis='Y',
        secondary_bone_axis='X',
        bake_anim=False,              # rig only; clips live in their own files
        mesh_smooth_type='FACE',
        path_mode='COPY',
        embed_textures=False,
    )
    if not os.path.isfile(out_path):
        raise RigError("export produced no file at %s" % out_path)
    log("exported OK (%d bytes)" % os.path.getsize(out_path))


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    p = argparse.ArgumentParser(prog="autorig.py", description="Headless Mixamo auto-rigger")
    p.add_argument("--mesh", required=True, help="input un-rigged mesh (fbx/obj/glb/gltf/ply/stl)")
    p.add_argument("--out", required=True, help="output rigged .fbx path")
    p.add_argument("--template", default=None, help="Mixamo-rigged FBX to source the skeleton from")
    p.add_argument("--fit", choices=["uniform", "hips"], default="uniform",
                   help="uniform=scale to stature (default); hips=scale so hips hit 50%% height")
    p.add_argument("--no-triangulate", action="store_true", help="skip triangulation in cleanup")
    p.add_argument("--keep-interior", action="store_true", help="skip interior-face removal")
    p.add_argument("--verbose", action="store_true")
    return p.parse_args(argv)


def main():
    args = parse_args()
    log("=== MeatEngine autorig ===")
    log("blender %s" % bpy.app.version_string)
    log("mesh    : %s" % args.mesh)
    log("out     : %s" % args.out)
    log("template: %s" % (args.template or "(default: anim-pack rig)"))

    if not os.path.isfile(args.mesh):
        raise RigError("mesh not found: %s" % args.mesh)

    reset_scene()

    # 1-2: mesh import + cleanup
    mesh = prepare_mesh(args.mesh,
                        do_triangulate=not args.no_triangulate,
                        remove_interior=not args.keep_interior)

    # 3: template armature
    arm = load_template_armature(args.template)

    # 4: fit
    fit_armature(arm, mesh, mode=args.fit)

    # 5: bind
    bind(mesh, arm)

    # 6: export
    export_fbx(mesh, arm, args.out)

    log("=== DONE ===")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("[autorig] FATAL: %s" % e)
        traceback.print_exc()
        sys.exit(1)
    sys.exit(0)
