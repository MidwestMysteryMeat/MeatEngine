// Data-driven UI core (Phase 5). The layout, binding, and parse logic are pure
// and GPU-free, so the tests pin the anchor math to exact pixel rects, check that
// nesting is relative to the parent, that bound signals drive (and clamp) widget
// values, and that a JSON HUD definition parses into the expected tree.

#include "Harness.h"

#include "engine/ui/UISystem.h"

#include <cmath>
#include <cstdio>

namespace {

using meattest::check;

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

bool rectIs(const meat::UIRect& r, float x, float y, float w, float h) {
    return near(r.x, x) && near(r.y, y) && near(r.w, w) && near(r.h, h);
}

const meat::UIRect kScreen{0.0f, 0.0f, 1000.0f, 800.0f};

void testAnchorLayout() {
    std::printf("UI anchors resolve to the expected pixel rects\n");
    // Full-screen stretch fills the parent.
    meat::UIWidget full;
    full.anchor = {0.0f, 0.0f, 1.0f, 1.0f};
    meat::ui::layout(full, kScreen);
    check(rectIs(full.rect, 0, 0, 1000, 800), "a {0,0,1,1} anchor fills the screen");

    // Uniform 20px inset margin.
    meat::UIWidget margin;
    margin.anchor = {0.0f, 0.0f, 1.0f, 1.0f};
    margin.offset = {20.0f, 20.0f, 20.0f, 20.0f};
    meat::ui::layout(margin, kScreen);
    check(rectIs(margin.rect, 20, 20, 960, 760), "equal insets make a uniform margin");

    // A bottom-left health bar: fractional box in the lower-left.
    meat::UIWidget bar;
    bar.anchor = {0.02f, 0.9f, 0.3f, 0.95f};
    meat::ui::layout(bar, kScreen);
    check(rectIs(bar.rect, 20, 720, 280, 40), "a fractional anchor places a HUD bar");
}

void testNestingIsRelativeToParent() {
    std::printf("child widgets lay out inside their parent's rect\n");
    meat::UIWidget root;
    root.anchor = {0.0f, 0.0f, 0.5f, 1.0f}; // left half → {0,0,500,800}
    meat::UIWidget child;
    child.anchor = {0.0f, 0.0f, 1.0f, 1.0f};
    child.offset = {10.0f, 10.0f, 10.0f, 10.0f};
    root.children.push_back(child);
    meat::ui::layout(root, kScreen);
    check(rectIs(root.rect, 0, 0, 500, 800), "the parent takes the left half");
    check(rectIs(root.children[0].rect, 10, 10, 480, 780),
          "the child insets within the parent, not the screen");
}

void testBindingsDriveAndClamp() {
    std::printf("bound signals set widget values and bars clamp to [0,1]\n");
    meat::UIWidget bar;
    bar.kind = meat::WidgetKind::Bar;
    bar.binding = "health";
    meat::UIContext ctx;

    ctx["health"] = 0.5f;
    meat::ui::applyBindings(bar, ctx);
    check(near(bar.value, 0.5f), "a bound bar takes its signal's value");

    ctx["health"] = 1.7f;
    meat::ui::applyBindings(bar, ctx);
    check(near(bar.value, 1.0f), "a bar fill clamps above 1");

    ctx["health"] = -3.0f;
    meat::ui::applyBindings(bar, ctx);
    check(near(bar.value, 0.0f), "a bar fill clamps below 0");
}

void testParseHudDefinition() {
    std::printf("a JSON HUD definition parses into the expected tree\n");
    const std::string def = R"({
        "kind":"panel","id":"root","anchor":[0,0,1,1],
        "children":[
            {"kind":"bar","id":"hp","binding":"player.health","anchor":[0.02,0.9,0.3,0.95]},
            {"kind":"label","id":"score","text":"0","anchor":[0.8,0.02,0.98,0.08]}
        ]
    })";
    meat::UIWidget root = meat::ui::parseString(def);
    check(root.kind == meat::WidgetKind::Panel && root.children.size() == 2,
          "the root panel parses with its two children");

    const meat::UIWidget* hp = meat::ui::find(root, "hp");
    const meat::UIWidget* score = meat::ui::find(root, "score");
    check(hp && hp->kind == meat::WidgetKind::Bar && hp->binding == "player.health",
          "the health bar parsed with its kind and binding");
    check(score && score->kind == meat::WidgetKind::Label && score->text == "0",
          "the score label parsed with its text");
    check(meat::ui::find(root, "nope") == nullptr, "find returns null for a missing id");

    // Parsed tree lays out + binds end to end.
    meat::ui::layout(root, kScreen);
    meat::UIContext ctx{{"player.health", 0.25f}};
    meat::ui::applyBindings(root, ctx);
    check(rectIs(meat::ui::find(root, "hp")->rect, 20, 720, 280, 40) &&
              near(meat::ui::find(root, "hp")->value, 0.25f),
          "the parsed HUD lays out and binds correctly");

    check(meat::ui::parseString("{ not json").children.empty(),
          "malformed JSON yields an empty widget, not a crash");
}

} // namespace

namespace meattest {

void runUI() {
    testAnchorLayout();
    testNestingIsRelativeToParent();
    testBindingsDriveAndClamp();
    testParseHudDefinition();
}

} // namespace meattest
