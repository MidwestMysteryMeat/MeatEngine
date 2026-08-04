#pragma once

#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace meat {

// A resolved on-screen rectangle in pixels (top-left origin).
struct UIRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// UMG-style anchor: a fractional [0,1] box within the parent that this widget's
// edges pin to. Combined with per-edge pixel insets (offset), it yields a rect
// that scales with the parent (screen) yet keeps fixed margins — the standard
// responsive-HUD model. minX==maxX (or minY==maxY) makes that axis a fixed
// fraction line; use a small span for fixed-position elements.
struct UIAnchor {
    float minX = 0.0f, minY = 0.0f, maxX = 1.0f, maxY = 1.0f;
};

enum class WidgetKind : std::uint8_t { Panel, Bar, Label, Image };

// One node of the retained UI tree. Authored as data (see UISystem::parse) and
// resolved to a pixel `rect` by UISystem::layout each frame the screen size or a
// binding changes. Renderer-agnostic: a draw pass reads `rect` + `kind` + style.
struct UIWidget {
    WidgetKind kind = WidgetKind::Panel;
    std::string id;
    UIAnchor anchor;
    // Per-edge pixel insets from the anchor lines: {left, top, right, bottom}.
    glm::vec4 offset{0.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    std::string text;    // Label content
    std::string binding; // name of a signal driving `value` (Bar fill / Label number)
    float value = 1.0f;  // Bar fill 0..1 (overwritten by a bound signal)

    UIRect rect;                     // computed by layout()
    std::vector<UIWidget> children;  // laid out relative to this widget's rect
};

// Named numeric signals a UI binds to (player.health, ammo, score, …). The game
// fills this each frame; applyBindings() pushes matching values into widgets.
using UIContext = std::unordered_map<std::string, float>;

} // namespace meat
