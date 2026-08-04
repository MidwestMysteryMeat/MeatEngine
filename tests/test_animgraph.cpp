// Animation state machine + 1D blend space (ANIMATION_BLEND_GRAPH.md §3). Pure and
// skeleton-free: the graph decides which clips at what weights to sample, so the
// tests pin that decision — blend-space bracketing/clamping, single vs blended
// states, a cross-fade whose weights always sum to 1 and that finalizes to the
// target, and transitions that only fire along a defined edge.

#include "Harness.h"

#include "engine/anim/AnimGraph.h"

#include <cmath>
#include <cstdio>

namespace {

using meattest::check;

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

float weightOf(const std::vector<meat::ClipWeight>& b, int clip) {
    for (const meat::ClipWeight& cw : b)
        if (cw.clip == clip) return cw.weight;
    return 0.0f;
}
float sumW(const std::vector<meat::ClipWeight>& b) {
    float s = 0.0f;
    for (const meat::ClipWeight& cw : b) s += cw.weight;
    return s;
}

// Idle(clip 0) + a locomotion blend space (walk/run by speed) + an unreachable
// Dead(clip 4). Idle<->Loco transitions exist; nothing leads to Dead.
meat::AnimGraph buildGraph() {
    meat::AnimGraph g;
    meat::AnimState idle;
    idle.name = "Idle";
    idle.clip = 0;
    meat::AnimState loco;
    loco.name = "Loco";
    loco.blendParam = "speed";
    loco.blend.samples = {{1, 0.0f}, {2, 4.5f}, {3, 7.0f}}; // idle-pose, walk, run
    meat::AnimState dead;
    dead.name = "Dead";
    dead.clip = 4;
    g.states = {idle, loco, dead};
    g.transitions = {{"Idle", "Loco", 0.2f}, {"Loco", "Idle", 0.2f}};
    return g;
}

void testBlendSpaceResolve() {
    std::printf("1D blend space brackets and clamps along its axis\n");
    const meat::AnimGraph g = buildGraph();
    const meat::BlendSpace1D& bs = g.states[1].blend;

    auto below = bs.resolve(-1.0f);
    check(below.clipA == 1 && below.clipB == 1, "below the axis clamps to the first clip");
    auto exact = bs.resolve(4.5f);
    check(exact.clipA == 2 && exact.clipB == 2, "exactly on a sample gives that clip alone");
    auto mid = bs.resolve(2.25f);
    check(mid.clipA == 1 && mid.clipB == 2 && near(mid.w, 0.5f),
          "halfway between two samples gives both at 0.5");
    auto above = bs.resolve(100.0f);
    check(above.clipA == 3 && above.clipB == 3, "above the axis clamps to the last clip");
}

void testStateBlends() {
    std::printf("single-clip and blend-space states report the right clip weights\n");
    const meat::AnimGraph g = buildGraph();
    meat::AnimGraphPlayer p;

    p.setGraph(&g, "Idle");
    check(p.currentState() == "Idle", "starts in the requested state");
    auto idle = p.currentBlend({});
    check(idle.size() == 1 && idle[0].clip == 0 && near(idle[0].weight, 1.0f),
          "a single-clip state is that clip at weight 1");

    // Jump straight into Loco (fresh player) to read its blend without a cross-fade.
    meat::AnimGraphPlayer q;
    q.setGraph(&g, "Loco");
    auto walk = q.currentBlend({{"speed", 2.25f}});
    check(near(weightOf(walk, 1), 0.5f) && near(weightOf(walk, 2), 0.5f) && near(sumW(walk), 1.0f),
          "a blend-space state mixes the two bracketing clips, summing to 1");
}

void testCrossFadeAndGating() {
    std::printf("transitions cross-fade over time and only fire along a real edge\n");
    const meat::AnimGraph g = buildGraph();
    meat::AnimGraphPlayer p;
    p.setGraph(&g, "Idle");

    check(!p.requestState("Dead"), "no edge Idle->Dead: the request is refused");
    check(p.currentState() == "Idle", "and the state is unchanged");

    check(p.requestState("Loco"), "Idle->Loco has an edge: the transition starts");
    check(p.transitioning(), "the player is now cross-fading");

    p.update(0.1f); // halfway through the 0.2 s cross-fade
    auto mid = p.currentBlend({{"speed", 4.5f}});
    check(near(weightOf(mid, 0), 0.5f) && near(weightOf(mid, 2), 0.5f) && near(sumW(mid), 1.0f),
          "midway: half the old Idle clip, half the new Loco clip, summing to 1");

    p.update(0.1f); // completes the cross-fade
    check(!p.transitioning() && p.currentState() == "Loco",
          "the cross-fade finalizes to the target state");
    auto done = p.currentBlend({{"speed", 4.5f}});
    check(done.size() == 1 && done[0].clip == 2 && near(done[0].weight, 1.0f),
          "after finalizing it is purely the target's clip");
}

} // namespace

namespace meattest {

void runAnimGraph() {
    testBlendSpaceResolve();
    testStateBlends();
    testCrossFadeAndGating();
}

} // namespace meattest
