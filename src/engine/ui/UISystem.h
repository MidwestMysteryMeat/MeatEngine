#pragma once

#include "engine/ui/UIWidget.h"

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace meat {

// The data-driven UI core: turn an authored widget tree into resolved on-screen
// rectangles and keep its bound values current. No GPU or windowing here — a
// renderer pass consumes the resolved rects, and the visual editor edits the same
// tree. Pure + deterministic, so it is fully unit-testable headless.
namespace ui {

// Resolve `w` (and its subtree) into pixel rects, laying each widget out inside
// `parent`. Call with the screen rect as `parent` for the root.
void layout(UIWidget& w, const UIRect& parent);

// Push bound signal values into the tree: a widget with a `binding` present in
// `ctx` takes that value (Bars clamp to [0,1] for fill). Recurses into children.
void applyBindings(UIWidget& w, const UIContext& ctx);

// Parse a UI definition (see docs) into a widget tree. Unknown/malformed fields
// fall back to defaults rather than throwing, so a bad HUD can't crash the game.
UIWidget parse(const nlohmann::json& def);

// Convenience: parse from a JSON string. Returns an empty Panel on parse error.
UIWidget parseString(const std::string& json);

// Depth-first search for a widget by id (nullptr if absent). Const + mutable.
const UIWidget* find(const UIWidget& root, const std::string& id);
UIWidget* find(UIWidget& root, const std::string& id);

} // namespace ui
} // namespace meat
