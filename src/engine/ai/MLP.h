#pragma once

#include <cstddef>
#include <vector>

namespace meat {

// Per-layer non-linearity. Identity is the usual choice for a policy's output
// (raw logits → argmax picks the action).
enum class Activation { Identity, ReLU, Tanh, Sigmoid };

// A minimal feed-forward multilayer perceptron for NPC policy inference — the
// runtime seed of the Phase-7 neural-policy `NpcBrain`. Reference (learned-from,
// not copied): SorawitChok/Neural-Network-from-scratch-in-Cpp (MIT). Only the
// forward pass lives here; training happens offline and ships weights as data.
//
// DETERMINISM: pure function of its weights + input, evaluated in a fixed order,
// so the same model + input reproduce the same output within a platform. Bit-for-
// bit cross-platform reproducibility (the authoritative-tick requirement) needs
// the planned integer/fixed-point rework — this float version is the seed and is
// safe for tooling and single-host use today.
class MLP {
public:
    // One dense layer: `outDim` neurons, each a weighted sum over `inDim` inputs
    // (weights row-major, one row per output neuron) plus a bias, then `act`.
    void addLayer(std::size_t inDim, std::size_t outDim, Activation act,
                  std::vector<float> weights, std::vector<float> biases);

    std::size_t inputDim() const { return m_layers.empty() ? 0 : m_layers.front().inDim; }
    std::size_t outputDim() const { return m_layers.empty() ? 0 : m_layers.back().outDim; }
    std::size_t layerCount() const { return m_layers.size(); }

    // Run inference. Returns the output-layer activations (empty if the input size
    // doesn't match inputDim() or the net is empty).
    std::vector<float> forward(const std::vector<float>& input) const;

    // Policy action = index of the largest output logit (−1 if the net is empty
    // or the input is the wrong size). Ties resolve to the lowest index.
    int argmax(const std::vector<float>& input) const;

private:
    struct Layer {
        std::size_t inDim = 0;
        std::size_t outDim = 0;
        Activation act = Activation::Identity;
        std::vector<float> weights; // outDim * inDim, row-major
        std::vector<float> biases;  // outDim
    };
    std::vector<Layer> m_layers;
};

} // namespace meat
