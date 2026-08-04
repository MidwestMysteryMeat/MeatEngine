// Neural-policy inference seed (Phase 7). The MLP forward pass is the runtime an
// NpcBrain will use, so the tests pin the math to hand-computed values, check the
// argmax action selection flips with the input, verify each activation, and
// confirm a malformed model/input fails closed (empty) instead of crashing.

#include "Harness.h"

#include "engine/ai/MLP.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using meattest::check;

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// Two-layer net: ReLU hidden then identity output, with weights chosen so the
// output is trivially hand-checkable.
meat::MLP buildValueNet() {
    meat::MLP net;
    // Hidden: neuron0 = x0, neuron1 = -x1 (row-major [1,0, 0,-1]), ReLU.
    net.addLayer(2, 2, meat::Activation::ReLU, {1.0f, 0.0f, 0.0f, -1.0f}, {0.0f, 0.0f});
    // Output: o0 = h0 + h1 + 0.5, o1 = h0 - h1 - 0.5 (identity).
    net.addLayer(2, 2, meat::Activation::Identity, {1.0f, 1.0f, 1.0f, -1.0f}, {0.5f, -0.5f});
    return net;
}

void testForwardMatchesHandComputation() {
    std::printf("MLP forward pass matches a hand-computed result\n");
    const meat::MLP net = buildValueNet();
    // input [2,3] → hidden pre [2,-3] → ReLU [2,0] → out [2+0+0.5, 2-0-0.5] = [2.5,1.5].
    const std::vector<float> out = net.forward({2.0f, 3.0f});
    check(out.size() == 2 && near(out[0], 2.5f) && near(out[1], 1.5f),
          "the two-layer ReLU→identity net produces the expected logits");
    check(net.forward({2.0f, 3.0f}) == net.forward({2.0f, 3.0f}),
          "forward is deterministic: same input, same output");
    check(net.inputDim() == 2 && net.outputDim() == 2 && net.layerCount() == 2,
          "the net reports its shape");
}

void testArgmaxSelectsActionFromInput() {
    std::printf("MLP argmax picks an action that depends on the input\n");
    meat::MLP net;
    net.addLayer(2, 2, meat::Activation::ReLU, {1.0f, 0.0f, 0.0f, -1.0f}, {0.0f, 0.0f});
    net.addLayer(2, 2, meat::Activation::Identity, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f});
    // [2,3] → hidden [2,0] → out [2,0] → action 0.
    check(net.argmax({2.0f, 3.0f}) == 0, "positive-x0 input selects action 0");
    // [-4,-1] → hidden pre [-4,1] → ReLU [0,1] → out [0,1] → action 1.
    check(net.argmax({-4.0f, -1.0f}) == 1, "negative-x1 input flips the choice to action 1");
}

void testActivations() {
    std::printf("MLP activations compute correctly\n");
    auto oneUnit = [](meat::Activation act) {
        meat::MLP n;
        n.addLayer(1, 1, act, {1.0f}, {0.0f});
        return n;
    };
    check(near(oneUnit(meat::Activation::Tanh).forward({0.5f})[0], std::tanh(0.5f)),
          "tanh activation");
    check(near(oneUnit(meat::Activation::Sigmoid).forward({0.0f})[0], 0.5f),
          "sigmoid(0) = 0.5");
    check(near(oneUnit(meat::Activation::ReLU).forward({-3.0f})[0], 0.0f),
          "ReLU clamps a negative input to zero");
}

void testMalformedFailsClosed() {
    std::printf("a malformed model or input fails closed, not crashing\n");
    meat::MLP net;
    net.addLayer(2, 3, meat::Activation::Identity, {1.0f, 2.0f}, {0.0f}); // wrong weight count
    check(net.layerCount() == 0, "a layer with a mismatched weight count is rejected");

    const meat::MLP good = buildValueNet();
    check(good.forward({1.0f}).empty(), "wrong-size input yields no output");
    check(good.argmax({}) == -1, "argmax on bad input returns -1");
}

} // namespace

namespace meattest {

void runMLP() {
    testForwardMatchesHandComputation();
    testArgmaxSelectsActionFromInput();
    testActivations();
    testMalformedFailsClosed();
}

} // namespace meattest
