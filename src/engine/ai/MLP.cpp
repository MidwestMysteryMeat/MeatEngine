#include "engine/ai/MLP.h"

#include <cmath>

namespace meat {

namespace {

float activate(Activation act, float x) {
    switch (act) {
    case Activation::ReLU:
        return x > 0.0f ? x : 0.0f;
    case Activation::Tanh:
        return std::tanh(x);
    case Activation::Sigmoid:
        return 1.0f / (1.0f + std::exp(-x));
    case Activation::Identity:
    default:
        return x;
    }
}

} // namespace

void MLP::addLayer(std::size_t inDim, std::size_t outDim, Activation act,
                   std::vector<float> weights, std::vector<float> biases) {
    // Silently ignore a malformed layer rather than crash — a bad model must not
    // take down the server; forward() then rejects mismatched input.
    if (inDim == 0 || outDim == 0 || weights.size() != inDim * outDim ||
        biases.size() != outDim)
        return;
    if (!m_layers.empty() && m_layers.back().outDim != inDim)
        return; // shape must chain
    m_layers.push_back({inDim, outDim, act, std::move(weights), std::move(biases)});
}

std::vector<float> MLP::forward(const std::vector<float>& input) const {
    if (m_layers.empty() || input.size() != m_layers.front().inDim) return {};
    std::vector<float> cur = input;
    for (const Layer& layer : m_layers) {
        std::vector<float> next(layer.outDim);
        for (std::size_t o = 0; o < layer.outDim; ++o) {
            float sum = layer.biases[o];
            const std::size_t base = o * layer.inDim; // row o of the weight matrix
            for (std::size_t i = 0; i < layer.inDim; ++i)
                sum += layer.weights[base + i] * cur[i];
            next[o] = activate(layer.act, sum);
        }
        cur = std::move(next);
    }
    return cur;
}

int MLP::argmax(const std::vector<float>& input) const {
    const std::vector<float> out = forward(input);
    if (out.empty()) return -1;
    int best = 0;
    for (std::size_t i = 1; i < out.size(); ++i)
        if (out[i] > out[best]) best = static_cast<int>(i);
    return best;
}

} // namespace meat
