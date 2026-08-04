#include "engine/anim/AnimGraph.h"

#include <algorithm>

namespace meat {

BlendSpace1D::Result BlendSpace1D::resolve(float x) const {
    if (samples.empty()) return {};
    if (x <= samples.front().pos)
        return {samples.front().clip, samples.front().clip, 0.0f};
    if (x >= samples.back().pos)
        return {samples.back().clip, samples.back().clip, 0.0f};
    for (std::size_t i = 1; i < samples.size(); ++i) {
        if (x <= samples[i].pos) {
            const BlendSample& a = samples[i - 1];
            const BlendSample& b = samples[i];
            const float span = b.pos - a.pos;
            const float w = span > 1e-6f ? (x - a.pos) / span : 0.0f;
            // Collapse to a single clip when x lands on a sample, so an exact match
            // never leaves a zero-weight clip in the blend.
            if (w <= 1e-6f) return {a.clip, a.clip, 0.0f};
            if (w >= 1.0f - 1e-6f) return {b.clip, b.clip, 0.0f};
            return {a.clip, b.clip, w};
        }
    }
    return {samples.back().clip, samples.back().clip, 0.0f};
}

int AnimGraph::indexOf(const std::string& name) const {
    for (std::size_t i = 0; i < states.size(); ++i)
        if (states[i].name == name) return static_cast<int>(i);
    return -1;
}

const AnimTransition* AnimGraph::transition(const std::string& from,
                                            const std::string& to) const {
    for (const AnimTransition& t : transitions)
        if (t.from == from && t.to == to) return &t;
    return nullptr;
}

void AnimGraphPlayer::setGraph(const AnimGraph* graph, const std::string& start) {
    m_graph = graph;
    m_current = graph ? graph->indexOf(start) : -1;
    m_target = m_current;
    m_transitioning = false;
    m_crossT = 0.0f;
    m_crossDur = 0.0f;
}

bool AnimGraphPlayer::requestState(const std::string& to) {
    if (!m_graph || m_current < 0) return false;
    const int ti = m_graph->indexOf(to);
    if (ti < 0) return false;
    if (ti == m_current && !m_transitioning) return true; // already there
    if (m_transitioning && ti == m_target) return true;   // already heading there
    const AnimTransition* t =
        m_graph->transition(m_graph->states[m_current].name, to);
    if (!t) return false; // no edge from the current state
    m_target = ti;
    m_transitioning = true;
    m_crossT = 0.0f;
    m_crossDur = t->duration;
    return true;
}

void AnimGraphPlayer::update(float dt) {
    if (!m_transitioning) return;
    m_crossT += dt;
    if (m_crossDur <= 0.0f || m_crossT >= m_crossDur) {
        m_current = m_target; // cross-fade complete
        m_transitioning = false;
        m_crossT = 0.0f;
    }
}

std::vector<ClipWeight> AnimGraphPlayer::stateBlend(int stateIndex,
                                                    const AnimParams& params) const {
    const AnimState& s = m_graph->states[static_cast<std::size_t>(stateIndex)];
    if (s.blend.samples.empty()) return {{s.clip, 1.0f}};
    float x = 0.0f;
    if (const auto it = params.find(s.blendParam); it != params.end()) x = it->second;
    const BlendSpace1D::Result r = s.blend.resolve(x);
    if (r.clipA == r.clipB) return {{r.clipA, 1.0f}};
    return {{r.clipA, 1.0f - r.w}, {r.clipB, r.w}};
}

namespace {
void mergeAdd(std::vector<ClipWeight>& out, int clip, float weight) {
    for (ClipWeight& cw : out)
        if (cw.clip == clip) {
            cw.weight += weight;
            return;
        }
    out.push_back({clip, weight});
}
} // namespace

std::vector<ClipWeight> AnimGraphPlayer::currentBlend(const AnimParams& params) const {
    if (!m_graph || m_current < 0) return {};
    std::vector<ClipWeight> cur = stateBlend(m_current, params);
    if (!m_transitioning) return cur;

    // Cross-fade: scale the current state down and the target up, then merge so a
    // clip shared by both states keeps a single summed weight.
    const float crossW = m_crossDur > 1e-6f ? std::clamp(m_crossT / m_crossDur, 0.0f, 1.0f) : 1.0f;
    std::vector<ClipWeight> out;
    for (const ClipWeight& cw : cur) mergeAdd(out, cw.clip, cw.weight * (1.0f - crossW));
    for (const ClipWeight& cw : stateBlend(m_target, params))
        mergeAdd(out, cw.clip, cw.weight * crossW);
    return out;
}

const std::string& AnimGraphPlayer::currentState() const {
    static const std::string kNone;
    if (!m_graph || m_current < 0) return kNone;
    return m_graph->states[static_cast<std::size_t>(m_current)].name;
}

} // namespace meat
