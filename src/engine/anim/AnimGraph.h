#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace meat {

// Animation state machine + 1D blend space (ANIMATION_BLEND_GRAPH.md §3), layered
// on the existing two-clip blend primitives in Animator. This part is pure and
// GPU-free: given the driving parameters it decides *which clips at what weights*
// to sample this frame; the sampler (Animator::blendPose) does the actual posing.
// So the whole graph is deterministic and unit-testable without a skeleton.

// Named float inputs that drive states/transitions (speed, aim, isMoving, …).
using AnimParams = std::unordered_map<std::string, float>;

// One clip placed on a blend axis. `clip` indexes the model's clip list.
struct BlendSample {
    int clip = -1;
    float pos = 0.0f; // axis value where this clip is the pure pose
};

// A 1D blend space: clips along an axis (e.g. locomotion by speed). Samples must be
// sorted by `pos` ascending. resolve(x) returns the two bracketing clips + a [0,1]
// weight; below the first / above the last it clamps to that end clip (a==b, w=0).
struct BlendSpace1D {
    std::vector<BlendSample> samples;
    struct Result {
        int clipA = -1;
        int clipB = -1;
        float w = 0.0f; // 0 → clipA, 1 → clipB
    };
    Result resolve(float x) const;
};

// A graph state: either a single clip (blend.samples empty) or a 1D blend space
// driven by `blendParam`.
struct AnimState {
    std::string name;
    int clip = -1;         // single-clip state
    BlendSpace1D blend;    // when non-empty, overrides `clip`
    std::string blendParam;
    bool loop = true;
};

// A directed transition with a cross-fade duration (seconds).
struct AnimTransition {
    std::string from;
    std::string to;
    float duration = 0.2f;
};

struct AnimGraph {
    std::vector<AnimState> states;
    std::vector<AnimTransition> transitions;
    int indexOf(const std::string& name) const;
    const AnimTransition* transition(const std::string& from, const std::string& to) const;
};

// One weighted clip layer the sampler should evaluate. Weights across the vector
// sum to ~1.
struct ClipWeight {
    int clip = -1;
    float weight = 0.0f;
};

// Per-actor runtime: tracks the active state and any in-progress cross-fade.
class AnimGraphPlayer {
public:
    void setGraph(const AnimGraph* graph, const std::string& start);

    // Begin a cross-fade to `to` if a transition from the current state exists (or
    // it is already the current/target). Returns false (no change) otherwise.
    bool requestState(const std::string& to);

    // Advance the cross-fade timer. Finalizes the transition when it completes.
    void update(float dt);

    // The clips + weights to sample this frame given `params` (1 or 2 states, each
    // 1 or 2 clips, merged; weights sum to ~1). Empty if no graph/state.
    std::vector<ClipWeight> currentBlend(const AnimParams& params) const;

    const std::string& currentState() const;
    bool transitioning() const { return m_transitioning; }

private:
    std::vector<ClipWeight> stateBlend(int stateIndex, const AnimParams& params) const;

    const AnimGraph* m_graph = nullptr;
    int m_current = -1;
    int m_target = -1;
    bool m_transitioning = false;
    float m_crossT = 0.0f;   // seconds into the cross-fade
    float m_crossDur = 0.0f; // cross-fade length
};

} // namespace meat
