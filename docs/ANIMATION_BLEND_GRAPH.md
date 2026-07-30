# Animation Blend Graph — design & drop-in code

Status: design + reference implementation (not yet integrated). The clip sampler
(`samplePose` / `resolve`) already produces a shear-free posed skeleton. The gap the
OSS survey (`docs/ENGINE_REUSE_SURVEY.md`) flagged is the **runtime layer above the
sampler**: blending, additive layers, and a state graph. This document specifies that
layer as pure math on the existing `Pose` / `Trs` types — no new heavy deps, namespace
`meat`, glm, hand-rolled.

Everything blends in **local TRS space, then resolves once**. We never lerp final
skinning matrices: a component-wise `mix` of two `mat4`s shears (rotation columns lose
orthonormality between keys), which is exactly the spike artifact the sampler was fixed
to avoid. Blending TRS (slerp the rotation, lerp pos/scale) and composing afterward is
rotation-correct.

---

## 0. Refactor: expose "sample to local TRS array"

`samplePose` today builds a `std::vector<glm::mat4> locals` then calls `resolve`. Split
that into a reusable helper. Behavior of `samplePose` is **bit-identical** — it just calls
the helper and resolves.

### `Trs`, `compose`, `decompose` move to the public surface

`Trs` and the two converters live in the `.cpp` anonymous namespace today. Promote the
type to the header and declare the helpers so the blend/graph code can reach them.

```cpp
// Animator.h  — add inside namespace meat, above Pose or just after it.

// Local bone transform as translation / rotation / scale. The blend layer works in
// this space because slerp+lerp of TRS is rotation-correct, whereas lerping the
// composed mat4 shears. No shear is assumed (true of rig exports), matching the
// existing decompose() gap-fill path.
struct Trs {
    glm::vec3 pos{0.0f};
    glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scl{1.0f};
};

glm::mat4 compose(const Trs& t);
Trs       decompose(const glm::mat4& m);

// Per-bone LOCAL transforms for a clip at a time, in the offset-authoritative space
// resolve() consumes (i.e. localBind * nodeBindLocalInv * key-delta, decomposed).
// This is the body of samplePose minus the final resolve(): sample two clips with
// this, blend the arrays, then resolve() once.
std::vector<Trs> sampleLocalTrs(const SkeletalModel& model, const AnimClip& clip,
                                float timeSeconds);

// Compose an array of local TRS and run the forward/skinning pass. The TRS-space twin
// of the existing resolve(const SkeletalModel&, const std::vector<glm::mat4>&).
Pose resolveLocalTrs(const SkeletalModel& model, const std::vector<Trs>& locals);
```

Remove the local `struct Trs`, `compose`, `decompose` definitions from the anonymous
namespace in `Animator.cpp` and give them external linkage (drop them out of the
anonymous namespace into `namespace meat`). Their bodies are unchanged.

### Refactored `Animator.cpp` core

```cpp
namespace meat {

// (compose / decompose / sampleVec / sampleQuat unchanged, now in namespace meat)

namespace {

// The old body of samplePose, up to but not including resolve(): produce the per-bone
// LOCAL matrices for a clip at a time. Untracked bones hold their bind local.
std::vector<glm::mat4> sampleLocalMatrices(const SkeletalModel& model,
                                           const AnimClip& clip, float timeSeconds) {
    const std::size_t count = model.bones.size();
    assert(count <= static_cast<std::size_t>(kMaxBones));

    float ticks = timeSeconds * (clip.ticksPerSec > 0.0f ? clip.ticksPerSec : 25.0f);
    if (clip.duration > 0.0f) {
        ticks = std::fmod(ticks, clip.duration);
        if (ticks < 0.0f) ticks += clip.duration;
    }

    std::vector<glm::mat4> locals(count);
    for (std::size_t b = 0; b < count; ++b) locals[b] = model.bones[b].localBind;

    for (const BoneTrack& track : clip.tracks) {
        if (track.boneIndex < 0 || static_cast<std::size_t>(track.boneIndex) >= count)
            continue;
        const std::size_t b = static_cast<std::size_t>(track.boneIndex);
        Trs trs;
        if (track.positions.empty() || track.rotations.empty() || track.scales.empty())
            trs = decompose(model.bones[b].nodeBindLocal);
        if (!track.positions.empty()) trs.pos = sampleVec(track.positions, ticks);
        if (!track.rotations.empty()) trs.rot = sampleQuat(track.rotations, ticks);
        if (!track.scales.empty())    trs.scl = sampleVec(track.scales, ticks);
        const Bone& bone = model.bones[b];
        locals[b] = bone.localBind * bone.nodeBindLocalInv * compose(trs);
    }
    return locals;
}

// resolve() unchanged, stays in this anonymous namespace.

} // namespace

Pose samplePose(const SkeletalModel& model, const AnimClip& clip, float timeSeconds) {
    return resolve(model, sampleLocalMatrices(model, clip, timeSeconds)); // identical result
}

std::vector<Trs> sampleLocalTrs(const SkeletalModel& model, const AnimClip& clip,
                                float timeSeconds) {
    std::vector<glm::mat4> locals = sampleLocalMatrices(model, clip, timeSeconds);
    std::vector<Trs> out(locals.size());
    for (std::size_t b = 0; b < locals.size(); ++b) out[b] = decompose(locals[b]);
    return out;
}

Pose resolveLocalTrs(const SkeletalModel& model, const std::vector<Trs>& locals) {
    std::vector<glm::mat4> mats(locals.size());
    for (std::size_t b = 0; b < locals.size(); ++b) mats[b] = compose(locals[b]);
    return resolve(model, mats);
}

} // namespace meat
```

Note on the round-trip: `sampleLocalTrs` decomposes `localBind * nodeBindLocalInv *
compose(trs)`. That product is rigid (rotation + translation + uniform-ish scale, no
shear) for rig exports, so `decompose` is lossless to float tolerance — the same
assumption the sampler's gap-fill already relies on. `samplePose` itself does **not** go
through the round-trip, so its output is unchanged.

---

## 1. Pose blending (two clips at weight w)

```cpp
// Animator.h
Trs  blendTrs(const Trs& a, const Trs& b, float w);
Pose blendPose(const SkeletalModel& model,
               const AnimClip& clipA, float tA,
               const AnimClip& clipB, float tB, float w);
```

```cpp
// Animator.cpp  (namespace meat)

// Per-bone local blend. slerp is rotation-correct and (per sampleQuat's note) glm::slerp
// already negates for the shortest arc; normalize kills drift. pos/scale lerp linearly.
Trs blendTrs(const Trs& a, const Trs& b, float w) {
    Trs r;
    r.pos = glm::mix(a.pos, b.pos, w);
    r.scl = glm::mix(a.scl, b.scl, w);
    r.rot = glm::normalize(glm::slerp(a.rot, b.rot, w));
    return r;
}

// Sample BOTH clips to local TRS, blend per bone, resolve once. w is clamped: w=0 -> A,
// w=1 -> B. Callers pass independent phases tA/tB (a blend space passes the same phase
// to keep footfalls in sync — see §3).
Pose blendPose(const SkeletalModel& model,
               const AnimClip& clipA, float tA,
               const AnimClip& clipB, float tB, float w) {
    w = glm::clamp(w, 0.0f, 1.0f);
    std::vector<Trs> a = sampleLocalTrs(model, clipA, tA);
    std::vector<Trs> b = sampleLocalTrs(model, clipB, tB);
    assert(a.size() == b.size());
    std::vector<Trs> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) out[i] = blendTrs(a[i], b[i], w);
    return resolveLocalTrs(model, out);
}
```

This generalizes to an N-way weighted blend: accumulate a weighted average per bone
(normalize pos/scale by total weight; for rotation, `slerp` pairwise in weight order or
use the running-normalized-lerp trick). The 2-way form covers idle↔walk↔run because a 1D
blend space only ever mixes the two bracketing clips (§3).

---

## 2. Additive layers (aim offset, hit reactions)

An additive clip stores a **delta from a reference pose**, not an absolute pose. Aim
offsets and hit reactions are authored this way so they layer on top of whatever
locomotion is playing. The delta is per bone:

```
dRot = inverse(ref.rot) * add.rot     (rotation delta in the bone's local frame)
dPos = add.pos - ref.pos
dScl = add.scl / ref.scl              (component-wise)
```

Applying at weight `w` (with optional per-bone mask for partial/upper-body blends):

```
out.rot = base.rot * slerp(identity, dRot, w)
out.pos = base.pos + dPos * w
out.scl = base.scl * mix(vec3(1), dScl, w)
```

The reference pose is usually the additive clip's own frame 0 (author poses the aim-center
there), or a dedicated reference clip.

```cpp
// Animator.h
struct AdditiveTrs {
    glm::vec3 dPos{0.0f};
    glm::quat dRot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 dScl{1.0f};
};

std::vector<AdditiveTrs> makeAdditive(const SkeletalModel& model,
                                      const AnimClip& add, float tAdd,
                                      const AnimClip& ref, float tRef);
// Convenience: reference is the additive clip's own frame 0.
std::vector<AdditiveTrs> makeAdditive(const SkeletalModel& model,
                                      const AnimClip& add, float tAdd);

// Apply a precomputed additive delta onto a base local-TRS array in place. mask is
// per-bone 0/1 (empty = all bones); enables partial blends (aim moves only the spine +
// arms, legs keep walking).
void applyAdditive(std::vector<Trs>& base,
                   const std::vector<AdditiveTrs>& delta, float w,
                   const std::vector<std::uint8_t>* mask = nullptr);
```

```cpp
// Animator.cpp  (namespace meat)

std::vector<AdditiveTrs> makeAdditive(const SkeletalModel& model,
                                      const AnimClip& add, float tAdd,
                                      const AnimClip& ref, float tRef) {
    std::vector<Trs> a = sampleLocalTrs(model, add, tAdd);
    std::vector<Trs> r = sampleLocalTrs(model, ref, tRef);
    assert(a.size() == r.size());
    std::vector<AdditiveTrs> d(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        d[i].dPos = a[i].pos - r[i].pos;
        d[i].dRot = glm::normalize(glm::inverse(r[i].rot) * a[i].rot);
        d[i].dScl = a[i].scl / r[i].scl; // rig scales are ~1; guard if a rig ships 0
    }
    return d;
}

std::vector<AdditiveTrs> makeAdditive(const SkeletalModel& model,
                                      const AnimClip& add, float tAdd) {
    return makeAdditive(model, add, tAdd, add, 0.0f); // ref = this clip's frame 0
}

void applyAdditive(std::vector<Trs>& base,
                   const std::vector<AdditiveTrs>& delta, float w,
                   const std::vector<std::uint8_t>* mask) {
    static const glm::quat kIdentity(1.0f, 0.0f, 0.0f, 0.0f);
    w = glm::clamp(w, 0.0f, 1.0f);
    for (std::size_t i = 0; i < base.size() && i < delta.size(); ++i) {
        float bw = w;
        if (mask && i < mask->size()) bw *= static_cast<float>((*mask)[i]);
        if (bw <= 0.0f) continue;
        base[i].rot = glm::normalize(base[i].rot * glm::slerp(kIdentity, delta[i].dRot, bw));
        base[i].pos += delta[i].dPos * bw;
        base[i].scl *= glm::mix(glm::vec3(1.0f), delta[i].dScl, bw);
    }
}
```

Rotation order is `base.rot * delta` — the additive rotation is expressed and applied in
the bone's own local frame, which is what ozz's additive blend job does. A hit-reaction
layer is identical math with a different clip/mask and a short envelope on `w`.

---

## 3. State machine / blend tree — `AnimGraph`

A small, data-driven graph. **States** produce a local-TRS array (a single clip or a 1D
blend space). **Transitions** cross-fade between states over a blend duration when their
conditions on **parameters** hold. Everything the runtime touches is POD so it can later
be authored from JSON/Lua — the C++ core stays the single source of truth.

### Data structures

```cpp
// AnimGraph.h  (namespace meat)
#include "engine/anim/Animator.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace meat {

// Named blend parameters. Bools (isAiming) store as 0/1. A JSON/Lua front-end just
// populates this map; gameplay writes it each frame (speed from velocity, etc.).
struct AnimParams {
    std::unordered_map<std::string, float> values;
    void  set(const std::string& k, float v) { values[k] = v; }
    void  set(const std::string& k, bool b)  { values[k] = b ? 1.0f : 0.0f; }
    float get(const std::string& k) const {
        auto it = values.find(k);
        return it == values.end() ? 0.0f : it->second;
    }
};

// One clip in a 1D blend space, at its position along the driving axis (e.g. m/s).
struct BlendSample { float point; int clipIndex; };

// Blend space sorted ascending by point: idle@0, walk@1.5, run@4.0. Sampled at a shared
// normalized phase so footfalls stay aligned across the mixed clips.
struct BlendSpace1D {
    std::string param;                 // AnimParams axis that drives it ("speed")
    std::vector<BlendSample> samples;  // ascending by point, >=1 entry
};

enum class StateKind { Clip, Blend1D };

struct AnimState {
    std::string  name;
    StateKind    kind = StateKind::Clip;
    int          clipIndex = 0;        // when kind==Clip
    BlendSpace1D blend;                // when kind==Blend1D
    float        speedScale = 1.0f;    // playback rate multiplier
    bool         loop = true;
};

enum class Cmp { Greater, Less, GreaterEq, LessEq, EqBool };
struct Condition { std::string param; Cmp cmp = Cmp::Greater; float value = 0.0f; };

struct Transition {
    int   from = -1;                   // state index, -1 = ANY state
    int   to   = 0;
    std::vector<Condition> conditions; // ANDed; all must hold to fire
    float blendDuration = 0.15f;       // seconds of cross-fade
};

// Optional additive layer applied after the base state each tick (aim offset / recoil).
struct AdditiveLayerDesc {
    int         clipIndex = -1;        // additive clip
    int         refClipIndex = -1;     // reference clip; -1 => clip's own frame 0
    std::string weightParam;           // AnimParams axis -> weight 0..1 ("aiming")
    std::vector<std::uint8_t> mask;    // per-bone 0/1, empty = all bones
};

struct AnimGraph {
    std::vector<AnimState>        states;
    std::vector<Transition>       transitions;
    std::vector<AdditiveLayerDesc> additiveLayers;
    int defaultState = 0;
};

} // namespace meat
```

### Runtime + tick

```cpp
// AnimGraph.h  (namespace meat) — the mutable per-actor instance
struct AnimGraphPlayer {
    const AnimGraph*     graph = nullptr;
    const SkeletalModel* model = nullptr;

    int   current = 0;        // active state index
    float currentTime = 0.0f; // seconds elapsed in current state

    // active cross-fade (blending == true means we mix prev -> current)
    bool  blending = false;
    int   prev = -1;
    float prevTime = 0.0f;
    float blendElapsed = 0.0f;
    float blendDuration = 0.0f;

    void start(const AnimGraph& g, const SkeletalModel& m);
    Pose update(float dt, const AnimParams& params);
};
```

```cpp
// AnimGraph.cpp  (namespace meat)
#include "engine/anim/AnimGraph.h"
#include <cassert>

namespace meat {
namespace {

bool holds(const Condition& c, const AnimParams& p) {
    const float v = p.get(c.param);
    switch (c.cmp) {
        case Cmp::Greater:   return v >  c.value;
        case Cmp::Less:      return v <  c.value;
        case Cmp::GreaterEq: return v >= c.value;
        case Cmp::LessEq:    return v <= c.value;
        case Cmp::EqBool:    return (v != 0.0f) == (c.value != 0.0f);
    }
    return false;
}

bool fires(const Transition& t, const AnimParams& p) {
    for (const Condition& c : t.conditions)
        if (!holds(c, p)) return false;
    return !t.conditions.empty() || true; // empty conditions = unconditional
}

// Evaluate one state to a local-TRS array. Blend spaces mix the two bracketing clips at
// a SHARED phase (time), so walk/run footfalls line up rather than sliding.
std::vector<Trs> evalState(const SkeletalModel& model, const AnimState& s,
                           float time, const AnimParams& params) {
    const float t = time * s.speedScale;
    if (s.kind == StateKind::Clip)
        return sampleLocalTrs(model, model.clips[s.clipIndex], t);

    const auto& S = s.blend.samples;
    assert(!S.empty());
    const float x = params.get(s.blend.param);
    if (x <= S.front().point)
        return sampleLocalTrs(model, model.clips[S.front().clipIndex], t);
    if (x >= S.back().point)
        return sampleLocalTrs(model, model.clips[S.back().clipIndex], t);

    std::size_t i = 1;
    while (i < S.size() && S[i].point < x) ++i;      // S[i-1].point <= x < S[i].point
    const BlendSample& lo = S[i - 1];
    const BlendSample& hi = S[i];
    const float w = (x - lo.point) / (hi.point - lo.point);

    std::vector<Trs> a = sampleLocalTrs(model, model.clips[lo.clipIndex], t);
    std::vector<Trs> b = sampleLocalTrs(model, model.clips[hi.clipIndex], t);
    std::vector<Trs> out(a.size());
    for (std::size_t k = 0; k < a.size(); ++k) out[k] = blendTrs(a[k], b[k], w);
    return out;
}

} // namespace

void AnimGraphPlayer::start(const AnimGraph& g, const SkeletalModel& m) {
    graph = &g; model = &m;
    current = g.defaultState; currentTime = 0.0f;
    blending = false; prev = -1; prevTime = 0.0f; blendElapsed = 0.0f; blendDuration = 0.0f;
}

Pose AnimGraphPlayer::update(float dt, const AnimParams& params) {
    // 1) advance clocks
    currentTime += dt;
    if (blending) { prevTime += dt; blendElapsed += dt; }

    // 2) transition check (only when not already blending — simple, no interrupts yet).
    //    ANY-state transitions (from == -1) let e.g. a hit react fire from any locomotion.
    if (!blending) {
        for (const Transition& t : graph->transitions) {
            if (t.to == current) continue;
            if (t.from != -1 && t.from != current) continue;
            if (!fires(t, params)) continue;
            prev = current; prevTime = currentTime;
            current = t.to;  currentTime = 0.0f;
            blendDuration = t.blendDuration; blendElapsed = 0.0f;
            blending = t.blendDuration > 0.0f;
            break;
        }
    }

    // 3) evaluate active state(s) to local TRS
    std::vector<Trs> pose = evalState(*model, graph->states[current], currentTime, params);
    if (blending) {
        std::vector<Trs> from = evalState(*model, graph->states[prev], prevTime, params);
        float alpha = blendDuration > 0.0f ? glm::clamp(blendElapsed / blendDuration, 0.0f, 1.0f) : 1.0f;
        for (std::size_t i = 0; i < pose.size(); ++i)
            pose[i] = blendTrs(from[i], pose[i], alpha); // from -> current
        if (alpha >= 1.0f) { blending = false; prev = -1; }
    }

    // 4) additive layers (aim offset, recoil) on top of the base locomotion
    for (const AdditiveLayerDesc& L : graph->additiveLayers) {
        float w = params.get(L.weightParam);
        if (w <= 0.0f || L.clipIndex < 0) continue;
        std::vector<AdditiveTrs> d = (L.refClipIndex >= 0)
            ? makeAdditive(*model, model->clips[L.clipIndex], currentTime,
                                   model->clips[L.refClipIndex], 0.0f)
            : makeAdditive(*model, model->clips[L.clipIndex], currentTime);
        const std::vector<std::uint8_t>* mask = L.mask.empty() ? nullptr : &L.mask;
        applyAdditive(pose, d, w, mask);
    }

    // 5) one resolve for the whole frame
    return resolveLocalTrs(*model, pose);
}

} // namespace meat
```

### Example graph (locomotion + aim), authored in C++ for now

```cpp
AnimGraph g;
// state 0: locomotion blend space idle->walk->run by "speed"
AnimState loco; loco.name = "Locomotion"; loco.kind = StateKind::Blend1D;
loco.blend.param = "speed";
loco.blend.samples = { {0.0f, IDLE}, {1.5f, WALK}, {4.0f, RUN} }; // clip indices
g.states.push_back(loco);
// state 1: jump (one-shot clip)
AnimState jump; jump.name = "Jump"; jump.kind = StateKind::Clip; jump.clipIndex = JUMP;
jump.loop = false;
g.states.push_back(jump);
// transitions
g.transitions.push_back({0, 1, {{"jump", Cmp::EqBool, 1.0f}}, 0.10f}); // loco->jump
g.transitions.push_back({1, 0, {{"jump", Cmp::EqBool, 0.0f}}, 0.20f}); // jump->loco
// additive aim: spine+arms mask, weight from "aiming"
AdditiveLayerDesc aim; aim.clipIndex = AIM_POSE; aim.refClipIndex = -1;
aim.weightParam = "aiming"; aim.mask = upperBodyMask; // per-bone 0/1
g.additiveLayers.push_back(aim);

AnimGraphPlayer player; player.start(g, model);
// each frame:
AnimParams p; p.set("speed", velocity); p.set("aiming", isAiming); p.set("jump", jumping);
Pose pose = player.update(dt, p);
```

### Deferred authoring (JSON/Lua)

Every graph struct is POD-ish (`std::string` + numbers + vectors). A loader maps a JSON
object to `AnimState`/`Transition`/`AdditiveLayerDesc` and resolves clip names to indices
via `SkeletalModel::clips`. `Cmp`/`StateKind` map from strings. The C++ tick above never
changes — the data does. This mirrors ozz (offline tooling builds runtime data) and
Esoterica (node-graph tool compiles to a runtime graph).

### Known simplifications (call out for a v2)

- **No transition interrupts.** A new transition can't start mid-blend. A production graph
  snapshots the current blended pose as the "from" and re-blends; the runtime already
  isolates that in the `blending` block.
- **Blend-space phase is raw seconds, not normalized.** Aligning footfalls perfectly wants
  the mixed clips sampled at a *normalized* phase (0..1 of each clip's duration) and a
  playback rate that follows the blend so cadence scales with speed. The `time` argument is
  the single hook to add that.
- **N-way blend** is 2-way here because a 1D blend space only mixes the two bracketing
  clips. 2D aim/strafe blend spaces (4-clip bilinear) reuse `blendTrs` twice.

---

## 4. MIT references (verified)

Licenses read this session from each project's own license file:

| Project | License | Verified from | What confirms the design |
| --- | --- | --- | --- |
| **ozz-animation** (Guillaume Blanc) | **MIT** — "Copyright (c) 2020 Guillaume Blanc" | `LICENSE.md` on `master` | Local-space blend job = per-bone weighted TRS soa lerp/nlerp then resolve; **additive blend job** = per-bone delta multiply against a reference/rest pose; **partial (masked) blend** per joint. Our `blendTrs` / `applyAdditive` / mask mirror these one-for-one. |
| **Esoterica** (Bobby Anguelov et al.) | **MIT** — "Copyright (c) 2022-2024 Bobby Anguelov" (+2024-2026 w/ Kirill Bazhenov) | `LICENSE.md` on `master` | Animation graph runtime: nodes (clip / 1D+2D blend space / state machine), parameterized transitions with blend durations, additive/layer nodes — the shape of our `AnimState`/`Transition`/`AdditiveLayerDesc` + tick. |
| **Ogre** (Torus Knot Software) | **MIT** — "Copyright (c) 2000-2013 Torus Knot Software Ltd" | `LICENSE` on `master` | `Ogre::Animation`/`AnimationState` blending (`ANIMBLEND_AVERAGE` vs `_CUMULATIVE`) and skeletal `blend`/`_blend` are the classic TRS-key + accumulate-then-resolve reference. |

**Provenance:** these are *design confirmations*, not code sources. All code above is
original math on MeatEngine's own `Pose`/`Trs`. No GPL projects were consulted for
implementation — in particular **ogldev is GPLv3 and was deliberately avoided**; nothing
here derives from it.

---

## For the reviewer (Codex)

- **Scope:** design + drop-in reference code only. **No source files were edited** — this
  document is the deliverable; a coordinator integrates it to avoid merge conflicts with
  the just-landed sampler.
- **Core invariant:** all mixing happens in **local TRS** (slerp rot, lerp pos/scale),
  never on composed `mat4`s — matrix lerp shears and reintroduces the exact spike artifact
  the sampler was fixed to remove. Everything funnels into a **single `resolve()` per
  frame**.
- **Refactor safety:** `samplePose` is preserved bit-identically — it becomes
  `resolve(model, sampleLocalMatrices(...))`, where `sampleLocalMatrices` is its old body
  verbatim. `sampleLocalTrs` (used by blend/graph) adds a `decompose→compose` round-trip;
  it is lossless only under the no-shear assumption the existing `decompose` gap-fill
  already makes. Verify on a real Mixamo rig that `resolveLocalTrs(sampleLocalTrs(...))`
  matches `samplePose(...)` to float tolerance before trusting the blend path.
- **Header churn:** `Trs`/`compose`/`decompose` move from the `.cpp` anonymous namespace to
  `namespace meat` public surface. New file suggested: `AnimGraph.h/.cpp`. No new deps
  (glm only).
- **Additive rotation order** is `base.rot * slerp(identity, delta, w)` — delta expressed
  in the bone's local frame, matching ozz. Confirm aim offsets compose on the correct side
  if a rig's bone axes differ.
- **Edge cases to test:** `blendPose` with w at exactly 0/1; blend space below/above the
  sample range (clamps to end clips); empty additive mask (= all bones); `dScl = a/r`
  division if any rig ships a zero scale (rig scales are ~1, but guard if paranoid).
- **Licenses:** ozz / Esoterica / Ogre all **MIT**, verified from their own license files
  this session (copyright lines quoted in §4). GPLv3 **ogldev** avoided; no GPL code used.
