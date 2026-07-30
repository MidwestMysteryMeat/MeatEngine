# Animation Retargeting & Clip-Merge

How to drive MeatEngine's skeletal characters with external mocap. Two paths:

1. **Clip-merge (same skeleton family)** — attach clips from an animation-only file
   to an existing rig by matching bone names. Zero retargeting. This is the path for
   the SWAT/operator characters, which are `mixamorig`-rigged, driven by Mixamo
   exports (also `mixamorig`) or by MoCap Online packs (same bone hierarchy, just no
   `mixamorig:` namespace prefix).
2. **Retarget (foreign skeleton)** — bake a UE5-mannequin / generic-mocap clip onto
   the `mixamorig` skeleton offline at load, through a bone-name map plus a rest-pose
   basis change.

Both paths produce a `meat::AnimClip` appended to `SkeletalModel::clips`, which
`Animator::samplePose` already plays. **Nothing in the runtime sampler changes** —
`samplePose` applies each key as a delta from `nodeBindLocal`
(`local = localBind * nodeBindLocalInv * compose(key)`), so a clip whose keys are
expressed in the target's own node-bind space "just works."

---

## Why the delta path matters here

`samplePose` (Animator.cpp) evaluates, per bone `b` with a track:

```
locals[b] = localBind[b] * nodeBindLocalInv[b] * compose(key)
```

At bind, `compose(key) == nodeBindLocal[b]`, so `nodeBindLocalInv * key == I` and the
clean offset-authoritative `localBind` is preserved exactly. A moving key rotates the
bone about its node-bind frame. Two consequences drive the design below:

- **Clip-merge:** if the incoming clip's keys are already in *the target's* node-bind
  space (true when the source is the same skeleton), you copy keys verbatim. When the
  source is the *same skeleton family but a slightly different bind* (e.g. a Mixamo
  export vs the specific SWAT bind), the delta path still self-corrects because the
  key is applied relative to `nodeBindLocal`, not absolutely.
- **Retarget:** you must emit keys such that
  `nodeBindLocalInv[b] * compose(key) == deltaLocal(f)`, where `deltaLocal(f)` is the
  rest-relative local motion you computed (identity at rest). That means emit
  `compose(key) = nodeBindLocal[b] * deltaLocal(f)` — see §2.

---

## 1. Clip-merge — `appendClipsFromFile`

Loads every animation from a separate FBX/glTF and attaches it to an already-loaded
`SkeletalModel` by bone name. Bone matching is:

1. exact (`mixamorig:Hips` → `mixamorig:Hips`),
2. namespace-normalized — strip any `prefix:` (so MoCap Online's `Hips` matches
   `mixamorig:Hips`, and vice-versa), then match on the normalized key.

Tracks whose bone is absent from the target are dropped with a warning. Keys are
copied as-authored (position keys scaled by `opts.scale`, exactly like the loader's
Pass 4), so they land in the target's node-bind space and play through the existing
delta sampler untouched.

### Drop-in (add to `SkeletalModel.cpp`, declare in `SkeletalModel.h`)

Header — add near `loadSkeletalModel`:

```cpp
// Load every animation clip from an animation-only file (Mixamo export, MoCap
// Online pack, etc.) and attach it to an existing model by matching bone names.
// Matching is exact first, then namespace-normalized ("mixamorig:Hips" <-> "Hips"),
// so any file whose skeleton shares the model's bone HIERARCHY works with no
// retargeting. Tracks whose bone is not in the model are dropped (counted in a
// warning). Returns the number of clips appended (0 on load failure or no match).
// opts.scale must match the scale the model itself was loaded with.
int appendClipsFromFile(SkeletalModel& model, const std::filesystem::path& animFile,
                        const ModelImportOptions& opts = {});
```

Implementation — append to the `meat` namespace in `SkeletalModel.cpp`. It reuses the
existing `toGlm`, `scaleTranslation`, and the Pass-4 key-copy pattern already in this
file:

```cpp
namespace {

// Reduce a bone/channel name to a match key: drop everything up to and including the
// last ':' (FBX namespace like "mixamorig:" or "Armature:"), so the same joint matches
// whether or not the exporter namespaced it. Bone names are otherwise case-sensitive.
std::string normalizeBoneName(const std::string& n) {
    const auto colon = n.rfind(':');
    return colon == std::string::npos ? n : n.substr(colon + 1);
}

} // namespace

int appendClipsFromFile(SkeletalModel& model, const std::filesystem::path& animFile,
                        const ModelImportOptions& opts) {
    if (model.bones.empty()) {
        log::error("appendClipsFromFile: target model has no skeleton");
        return 0;
    }

    Assimp::Importer importer;
    // Same pivot collapse as loadSkeletalModel: with pivots preserved, FBX animation
    // channels target $AssimpFbx$ pseudo-nodes and never match the bone names.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    // Animation-only import: we need the node graph + animations, not geometry
    // post-processing. Assimp still parses meshes if present (harmless — ignored).
    const aiScene* scene = importer.ReadFile(animFile.string(), 0);
    if (!scene || !scene->mRootNode) {
        log::error("appendClipsFromFile '{}' failed: {}", animFile.string(),
                   importer.GetErrorString());
        return 0;
    }
    if (scene->mNumAnimations == 0) {
        log::warn("appendClipsFromFile '{}': file has no animations", animFile.string());
        return 0;
    }

    // Normalized-name -> bone index, built once from the target. Exact names are
    // already in model.boneByName; this table catches the namespace-mismatch case.
    std::unordered_map<std::string, int> normToBone;
    normToBone.reserve(model.bones.size());
    for (int b = 0; b < static_cast<int>(model.bones.size()); ++b) {
        normToBone.emplace(normalizeBoneName(model.bones[b].name), b); // first wins
    }

    const auto resolveBone = [&](const std::string& channelName) -> int {
        if (const auto it = model.boneByName.find(channelName); it != model.boneByName.end()) {
            return it->second; // exact (mixamorig:Hips -> mixamorig:Hips)
        }
        if (const auto it = normToBone.find(normalizeBoneName(channelName));
            it != normToBone.end()) {
            return it->second; // namespace-normalized (Hips -> mixamorig:Hips)
        }
        return -1;
    };

    int appended = 0;
    int totalDropped = 0;
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation& anim = *scene->mAnimations[a];
        AnimClip clip;
        clip.name = anim.mName.length > 0 ? anim.mName.C_Str()
                                          : std::format("{}#{}", animFile.stem().string(), a);
        clip.duration = static_cast<float>(anim.mDuration);
        clip.ticksPerSec =
            anim.mTicksPerSecond > 0.0 ? static_cast<float>(anim.mTicksPerSecond) : 25.0f;

        int droppedThisClip = 0;
        for (unsigned c = 0; c < anim.mNumChannels; ++c) {
            const aiNodeAnim& ch = *anim.mChannels[c];
            const int boneIdx = resolveBone(ch.mNodeName.C_Str());
            if (boneIdx < 0) {
                ++droppedThisClip; // channel targets a bone the model doesn't have
                continue;
            }
            BoneTrack track;
            track.boneIndex = boneIdx;
            // Same key copy as loadSkeletalModel Pass 4: positions scaled, rotations
            // normalized, scales verbatim. Keys stay in the source node space, which
            // for a matching skeleton IS the target node-bind space samplePose expects.
            track.positions.reserve(ch.mNumPositionKeys);
            for (unsigned k = 0; k < ch.mNumPositionKeys; ++k) {
                const aiVectorKey& key = ch.mPositionKeys[k];
                track.positions.push_back(
                    {static_cast<float>(key.mTime),
                     glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z) * opts.scale});
            }
            track.rotations.reserve(ch.mNumRotationKeys);
            for (unsigned k = 0; k < ch.mNumRotationKeys; ++k) {
                const aiQuatKey& key = ch.mRotationKeys[k];
                track.rotations.push_back(
                    {static_cast<float>(key.mTime),
                     glm::normalize(glm::quat(key.mValue.w, key.mValue.x, key.mValue.y,
                                              key.mValue.z))});
            }
            track.scales.reserve(ch.mNumScalingKeys);
            for (unsigned k = 0; k < ch.mNumScalingKeys; ++k) {
                const aiVectorKey& key = ch.mScalingKeys[k];
                track.scales.push_back({static_cast<float>(key.mTime),
                                        glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z)});
            }
            clip.tracks.push_back(std::move(track));
        }

        if (clip.tracks.empty()) {
            log::warn("appendClipsFromFile '{}': clip '{}' matched 0 of {} channels — "
                      "skeleton mismatch? (needs retargetClipsFromFile)",
                      animFile.filename().string(), clip.name, anim.mNumChannels);
            totalDropped += droppedThisClip;
            continue;
        }
        totalDropped += droppedThisClip;
        model.clips.push_back(std::move(clip));
        ++appended;
    }

    if (totalDropped > 0) {
        log::warn("appendClipsFromFile '{}': dropped {} channels with no matching bone",
                  animFile.filename().string(), totalDropped);
    }
    log::info("appendClipsFromFile '{}': +{} clips (model now {} clips)",
              animFile.filename().string(), appended, model.clips.size());
    return appended;
}
```

### Wiring it into `loadAnimTestActor` (Engine.cpp)

After `actor->model = std::move(*model);`, before the `hasRealClip` check, pull extra
clips from a CLI-supplied or conventionally-named animation file:

```cpp
// Merge external animation-only files (same skeleton family — no retarget).
// --animclip <file> wins; else any conventionally named sidecar next to the mesh.
for (const std::string& clipFile : m_animClips) { // vector<string>, from --animclip
    appendClipsFromFile(actor->model, clipFile, {.scale = 1.0f});
}
```

Then `hasRealClip` picks up the merged clips exactly as it does the embedded one.

---

## 2. Retarget — foreign skeletons → `mixamorig`

Needed only when the source skeleton has a *different bone hierarchy or bind axes*:
UE5/UEFN mannequin (`pelvis`, `calf_l`, `clavicle_l`), or the Blender `Armature` rig.
The output is still a `meat::AnimClip` in the target's node-bind space, so `samplePose`
plays it with no runtime change.

### 2a. Bone-name map (UE5 Manny ↔ mixamorig)

```cpp
// UE Mannequin (UE4 3-spine baseline; UE5 extras folded down) -> mixamorig joint.
// Unmapped UE bones (root, ik_*, *_twist_*, spine_04/05 beyond the fold) have no
// mixamorig equivalent and are simply skipped by the retargeter.
inline const std::unordered_map<std::string, std::string>& ue5ToMixamo() {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"pelvis",       "mixamorig:Hips"},
        {"spine_01",     "mixamorig:Spine"},
        {"spine_02",     "mixamorig:Spine1"},
        {"spine_03",     "mixamorig:Spine2"},
        {"spine_04",     "mixamorig:Spine2"},   // UE5 extra spine -> fold into Spine2
        {"spine_05",     "mixamorig:Spine2"},   // (last writer wins; drives the neck base)
        {"neck_01",      "mixamorig:Neck"},
        {"neck_02",      "mixamorig:Neck"},     // mixamo has a single Neck
        {"head",         "mixamorig:Head"},

        {"clavicle_l",   "mixamorig:LeftShoulder"},
        {"upperarm_l",   "mixamorig:LeftArm"},
        {"lowerarm_l",   "mixamorig:LeftForeArm"},
        {"hand_l",       "mixamorig:LeftHand"},
        {"clavicle_r",   "mixamorig:RightShoulder"},
        {"upperarm_r",   "mixamorig:RightArm"},
        {"lowerarm_r",   "mixamorig:RightForeArm"},
        {"hand_r",       "mixamorig:RightHand"},

        {"thigh_l",      "mixamorig:LeftUpLeg"},
        {"calf_l",       "mixamorig:LeftLeg"},
        {"foot_l",       "mixamorig:LeftFoot"},
        {"ball_l",       "mixamorig:LeftToeBase"},
        {"thigh_r",      "mixamorig:RightUpLeg"},
        {"calf_r",       "mixamorig:RightLeg"},
        {"foot_r",       "mixamorig:RightFoot"},
        {"ball_r",       "mixamorig:RightToeBase"},

        // Fingers (UE thumb_/index_/middle_/ring_/pinky_ 01..03 -> mixamo *1..*3).
        {"thumb_01_l",  "mixamorig:LeftHandThumb1"},  {"thumb_02_l",  "mixamorig:LeftHandThumb2"},
        {"thumb_03_l",  "mixamorig:LeftHandThumb3"},
        {"index_01_l",  "mixamorig:LeftHandIndex1"},  {"index_02_l",  "mixamorig:LeftHandIndex2"},
        {"index_03_l",  "mixamorig:LeftHandIndex3"},
        {"middle_01_l", "mixamorig:LeftHandMiddle1"}, {"middle_02_l", "mixamorig:LeftHandMiddle2"},
        {"middle_03_l", "mixamorig:LeftHandMiddle3"},
        {"ring_01_l",   "mixamorig:LeftHandRing1"},   {"ring_02_l",   "mixamorig:LeftHandRing2"},
        {"ring_03_l",   "mixamorig:LeftHandRing3"},
        {"pinky_01_l",  "mixamorig:LeftHandPinky1"},  {"pinky_02_l",  "mixamorig:LeftHandPinky2"},
        {"pinky_03_l",  "mixamorig:LeftHandPinky3"},
        {"thumb_01_r",  "mixamorig:RightHandThumb1"}, {"thumb_02_r",  "mixamorig:RightHandThumb2"},
        {"thumb_03_r",  "mixamorig:RightHandThumb3"},
        {"index_01_r",  "mixamorig:RightHandIndex1"}, {"index_02_r",  "mixamorig:RightHandIndex2"},
        {"index_03_r",  "mixamorig:RightHandIndex3"},
        {"middle_01_r", "mixamorig:RightHandMiddle1"},{"middle_02_r", "mixamorig:RightHandMiddle2"},
        {"middle_03_r", "mixamorig:RightHandMiddle3"},
        {"ring_01_r",   "mixamorig:RightHandRing1"},  {"ring_02_r",   "mixamorig:RightHandRing2"},
        {"ring_03_r",   "mixamorig:RightHandRing3"},
        {"pinky_01_r",  "mixamorig:RightHandPinky1"}, {"pinky_02_r",  "mixamorig:RightHandPinky2"},
        {"pinky_03_r",  "mixamorig:RightHandPinky3"},
    };
    return kMap;
}
```

### 2b. Rest-pose compensation (T-pose vs A-pose, differing bind axes)

Two skeletons never share bone axis conventions (UE bones point down-X; mixamo down-Y)
and often differ in rest posture (UE mannequin ships a slight A-pose; Mixamo is
T-pose). You cannot copy local rotations across. The robust fix — the same one
**ozz-animation**'s retargeting sample uses — works in **global orientation space**
against each skeleton's own rest pose, so the axis convention and the A/T difference
cancel out automatically:

For a mapped joint pair (source `s`, target `t`), let

- `Rs_rest` = source bone rest **global** orientation, `Rt_rest` = target rest global,
- `Rs(f)` = source bone animated global orientation at frame `f`.

Then the world-space motion the source joint underwent since rest is
`dR = Rs(f) * inverse(Rs_rest)`, and the target should undergo the *same world motion
from its own rest*: `Rt_world(f) = dR * Rt_rest`. Because each side is measured
relative to its **own** rest global, the fixed A↔T and axis offsets divide out — only
the frame-to-frame motion transfers. Convert to the target's local and then to a
rest-relative delta the delta-sampler consumes.

This is exactly ozz's approach: it samples the source `animation` against the source
`skeleton`'s rest pose and re-expresses joints on the target skeleton via per-joint
rest references. (ozz-animation, Guillaume Blanc — **MIT license**; see licensing note
below. No code is copied; only the offline global-delta technique is reused, which is
also the standard Assimp/Ogre "retarget by rest-relative global delta" method.)

### 2c. Algorithm (pseudocode)

```
build target rest data (from the already-loaded SkeletalModel):
    restGlobalT[b] = inverse(model.bones[b].offset)      # bind global in mesh space
    restLocalT[b]  = model.bones[b].localBind            # authoritative rest local
    (parent chain from model.bones[b].parent)

load source file with Assimp (pivots collapsed):
    for each source node: restLocalS[name] = node.mTransformation
    restGlobalS[name] = walk hierarchy accumulating restLocalS   # global rest

for f in 0 .. numFrames:                                 # sample at target 30 fps
    Rt_world = {}                                         # per target bone this frame
    Pt_world = {}
    for each target bone b in TOPOLOGICAL order:          # parents before children
        s = map[ model.bones[b].name ]                    # reverse of ue5ToMixamo
        parent = model.bones[b].parent

        if s exists in source anim:
            Rs_f   = rotation( sampleSourceGlobal(s, f) ) # animated source global rot
            dR     = Rs_f * inverse(rotation(restGlobalS[s]))
            Rt_w   = normalize(dR * rotation(restGlobalT[b]))
        else:
            Rt_w   = (parent>=0 ? Rt_world[parent] : I) * rotation(restLocalT[b])  # hold rest

        Rt_world[b] = Rt_w
        Rt_parent   = (parent>=0 ? Rt_world[parent] : I)
        Rt_local    = inverse(Rt_parent) * Rt_w           # target local orientation
        deltaLocal  = inverse(rotation(restLocalT[b])) * Rt_local   # identity at rest

        # Emit so samplePose reproduces localBind * deltaLocal:
        #   key = nodeBindLocal[b] with rotation *= deltaLocal
        keyRot = quat(model.bones[b].nodeBindLocal) * deltaLocal
        emit rotation key {time=f, keyRot}
        emit scale key    {time=f, 1}                     # mocap carries no scale

    # Root translation (Hips only) — scale by hip-height ratio to curb foot slide:
    hips = map^-1("mixamorig:Hips")
    srcHipsPos = translation( sampleSourceGlobal(hips_src, f) )
    ratio      = restGlobalT[Hips].y / restGlobalS[hips_src].y
    emit position key for Hips {time=f, decompose(nodeBindLocal[Hips]).pos
                                          + (srcHipsPos - restHipsPosS) * ratio}
```

Notes:
- Iterating targets in topological order (as the model already stores them) lets
  `Rt_world[parent]` be ready when a child needs it, including the "hold rest" branch
  for unmapped intermediate bones (e.g. twist bones).
- `sampleSourceGlobal(s, f)` = compose the source local TRS at time `f` (interpolate
  the source channel keys, falling back to `restLocalS` where a channel is absent) down
  the source parent chain. Reuse `sampleVec`/`sampleQuat` logic from Animator.cpp.
- Rotations dominate; translation is applied only to Hips. Feet may still slide without
  IK — acceptable for a first pass and matches how most engines ship un-IK'd retargets.

### 2d. Signature

```cpp
// Bake every animation from a foreign-skeleton file (UE5 mannequin, generic mocap)
// onto the model's mixamorig skeleton and append as native clips. `srcToTarget` maps
// SOURCE bone name -> TARGET bone name (build by inverting ue5ToMixamo(), or pass a
// custom table for other rigs). Returns clips appended. Use this ONLY when
// appendClipsFromFile matched no bones; same-family files need no retarget.
int retargetClipsFromFile(SkeletalModel& model, const std::filesystem::path& animFile,
                          const std::unordered_map<std::string, std::string>& srcToTarget,
                          const ModelImportOptions& opts = {});
```

The body implements §2c using the same Assimp patterns (`toGlm`, `scaleTranslation`)
and the same key-emission structs as `appendClipsFromFile`. It is longer but has no new
dependencies; the pseudocode above maps line-for-line to glm calls
(`glm::quat_cast`, `glm::inverse`, `glm::normalize`, `glm::slerp`).

---

## 3. Assets

### 3a. Directly usable on the SWAT (mixamorig — no retarget)

- **The SWAT itself already carries a clip.** `F:\_anim_ref_models\anim_test_swat_backup.fbx`
  is `mixamorig`-rigged and embeds `SWAT|SWATAction.001` (~199 anim-curve nodes). Loaded
  by `loadSkeletalModel`, it plays through `samplePose` today.
- **MoCap Online packs — same hierarchy, prefix only differs → use `appendClipsFromFile`.**
  Their bone names are `Hips, Spine, LeftArm, LeftForeArm, LeftHand, LeftUpLeg, LeftLeg,
  LeftFoot, LeftToeBase, LeftHandIndex1…` — the Autodesk/MotionBuilder biped family,
  i.e. Mixamo names minus the `mixamorig:` namespace. `normalizeBoneName` bridges them
  with no retargeting. Concrete FBX roots in `G:\VaultCache\FabLibrary\`:
  - `Rifle_Starter_Animation_-_MoCap_Pack-c695f04c\fbx\...\FBX_Rifle_Starter_27A2\Animation\` (aim offsets, fire, reload, locomotion)
  - `Pistol_Starter_-_MoCap_Pack-95dbdb60\` (31 FBX)
  - `Mobility_Starter_-_MoCap_Animation_Pack-2c29cfb9\` (60 FBX — walk/run/jump)
  - `Ninja_Starter_-_MoCap_Pack-fc64fd4b\`, `Death_Animations_-_MoCap_Pack-2b00697b\` (17 FBX),
    `Zombie_Starter_-_MoCap_Animation_Pack-c52d1834\`
  > Verify per-file: MoCap Online sometimes also ships a UE-mannequin variant in the
  > same pack. If `appendClipsFromFile` logs "matched 0 of N channels", that file is the
  > mannequin export — send it through the retargeter instead.

### 3b. UE5-mannequin mocap (needs the retargeter)

All under `G:\VaultCache\FabLibrary\`; bone names verified as `pelvis / calf_l /
clavicle_l / spine_01`:
- `Pistol_and_Rifle_Locomotion_Animations_1700-509f8c0a\` (~1,759 FBX, UEFN mannequin — huge locomotion set)
- `Game_Animation_Sample_Animations_Retargeted_to_ue5_mannequin_Animations_only-259f8545\` (~1,374 FBX — Epic's GASP set)
- `Paragon_animations_retargeted_to_Manny-e6de87b1\` (~5,385 FBX)
- `Lyra_Animation_Sequences_Only-ada3c1dd\` (~576 FBX)

Other candidate packs seen in the inventory (skeleton unverified — probe before use):
`Armed_Unarmed_Locomotion_Animation_Set-9c8bb57d`, `30_Preacher_NPC_Mocap_Animations-f7d344a6`,
`ActorCore_Sample_Motions-4776b9fa`, `Free_Pack_-_Basic_Motions-6c1ce1a7`.

> **Licensing:** these are Fab/Epic marketplace downloads governed by the Fab Standard
> (or Epic Content) EULA the user accepted at download. That license permits use in the
> user's own shipped titles but **forbids redistribution of the raw assets** — so they
> stay out of the public MeatEngine repo (already covered by the `assets/` gitignore).
> No bundled `LICENSE`/`EULA` file ships inside the pack folders (verified — none
> present); the terms live in the user's Fab account, not on disk.

### 3c. Free Mixamo clips (drop-in on the SWAT, zero retarget)

Mixamo animations use the identical `mixamorig` skeleton, so any Mixamo download runs
through `appendClipsFromFile` (or even `loadSkeletalModel` directly) with no map. Manual
steps (mixamo.com requires a free Adobe login — cannot be scripted headless):

1. Sign in at **mixamo.com** with a free Adobe account.
2. Pick an animation (Walk, Run, Idle, Rifle Aim, Reload, …).
3. **Download** → Format **FBX Binary (.fbx)**, Skin **Without Skin** (animation only),
   Frames per Second 30, Keyframe Reduction none.
4. For locomotion (walk/run/strafe), tick **"In Place"** so the clip does not bake root
   translation — the game moves the actor; a non-in-place clip would fight it and slide.
   (For a one-shot like a death or emote, leave In Place off if you want the root arc.)
5. Drop the `.fbx` next to the mesh and pass it via `--animclip`, or load it directly.

---

## For the reviewer (Codex)

- **Runtime sampler is untouched.** Both new functions only append `AnimClip`s to
  `SkeletalModel::clips`; `Animator::samplePose` plays them via the existing
  `local = localBind * nodeBindLocalInv * compose(key)` delta. Confirm the invariant
  that **keys are emitted in the target's node-bind space**: clip-merge copies source
  keys verbatim (valid because the skeleton matches); the retargeter explicitly emits
  `keyRot = quat(nodeBindLocal[b]) * deltaLocal` so `nodeBindLocalInv * key == deltaLocal`
  and, at rest, `deltaLocal == I` → bind preserved exactly.
- **`appendClipsFromFile` is complete drop-in C++** reusing this file's `toGlm`,
  `scaleTranslation`, and Pass-4 key-copy; only `normalizeBoneName` is new. Matching is
  exact-then-namespace-normalized; unmatched channels are dropped and counted; a clip
  that matches zero channels is skipped with a "needs retarget" warning. `opts.scale`
  must equal the model's load scale (position keys are scaled to match).
- **Retargeter is design + full map + pseudocode→glm**, deliberately not hand-expanded
  to final C++ because it must be validated against a real UE-mannequin clip on the
  R720 VLM booth before trusting it (I misjudge dark PSX renders — per project law, gate
  animation correctness on `tools/vlm_gate_r720.py`, not self-review). The global-delta
  algorithm is the standard rest-relative method (ozz-animation, Assimp/Ogre); no third-
  party code is copied.
- **Key asset finding:** MoCap Online packs (Rifle/Pistol/Mobility/Ninja/Zombie Starter)
  are the *same* MotionBuilder bone hierarchy as `mixamorig`, differing only by the
  `mixamorig:` namespace — so they need **clip-merge, not retarget**. Only the Kingboars
  UE5-mannequin packs (Game Animation Sample, Paragon, Lyra, Pistol&Rifle 1700) need the
  retargeter. The SWAT already embeds its own clip.
- **Licensing verified from disk:** `raptoid` = CC-BY-4.0 (credit Reallusion) — the only
  bundled LICENSE found under `F:\_anim_ref_models`. **No local ozz copy exists** (the
  memo's "ozz refs" are not on disk); the MIT citation is from public knowledge — verify
  the actual LICENSE before vendoring any ozz code. Fab mocap packs carry no on-disk
  license; they are Fab-EULA (owner's-game use, no raw redistribution) → keep out of the
  public repo.
- **Integration points:** declare both functions in `SkeletalModel.h`; add a
  `std::vector<std::string> m_animClips` + `--animclip` CLI flag and the merge loop shown
  in §1 to `Engine::loadAnimTestActor`. No other call sites change.
