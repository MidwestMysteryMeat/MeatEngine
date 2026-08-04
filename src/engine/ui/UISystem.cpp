#include "engine/ui/UISystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace meat {
namespace ui {

namespace {

WidgetKind kindFromString(const std::string& s) {
    if (s == "bar") return WidgetKind::Bar;
    if (s == "label") return WidgetKind::Label;
    if (s == "image") return WidgetKind::Image;
    return WidgetKind::Panel;
}

// Read a JSON array of `n` floats into `out` (up to n), leaving defaults on miss.
template <int N>
void readFloats(const nlohmann::json& j, const char* key, float (&out)[N]) {
    if (!j.contains(key) || !j[key].is_array()) return;
    const auto& arr = j[key];
    for (int i = 0; i < N && i < static_cast<int>(arr.size()); ++i)
        if (arr[i].is_number()) out[i] = arr[i].get<float>();
}

} // namespace

void layout(UIWidget& w, const UIRect& parent) {
    // Edges = anchor line within the parent + a pixel inset (right/bottom insets
    // pull inward, so a full anchor with equal insets is a uniform margin).
    const float left = parent.x + w.anchor.minX * parent.w + w.offset.x;
    const float top = parent.y + w.anchor.minY * parent.h + w.offset.y;
    const float right = parent.x + w.anchor.maxX * parent.w - w.offset.z;
    const float bottom = parent.y + w.anchor.maxY * parent.h - w.offset.w;
    w.rect = {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
    for (UIWidget& c : w.children) layout(c, w.rect);
}

void applyBindings(UIWidget& w, const UIContext& ctx) {
    if (!w.binding.empty()) {
        if (const auto it = ctx.find(w.binding); it != ctx.end()) {
            w.value = it->second;
            if (w.kind == WidgetKind::Bar) w.value = std::clamp(w.value, 0.0f, 1.0f);
        }
    }
    for (UIWidget& c : w.children) applyBindings(c, ctx);
}

UIWidget parse(const nlohmann::json& def) {
    UIWidget w;
    if (!def.is_object()) return w;
    if (def.contains("kind") && def["kind"].is_string())
        w.kind = kindFromString(def["kind"].get<std::string>());
    if (def.contains("id") && def["id"].is_string()) w.id = def["id"].get<std::string>();
    if (def.contains("text") && def["text"].is_string()) w.text = def["text"].get<std::string>();
    if (def.contains("binding") && def["binding"].is_string())
        w.binding = def["binding"].get<std::string>();
    if (def.contains("value") && def["value"].is_number())
        w.value = def["value"].get<float>();

    float anchor[4] = {w.anchor.minX, w.anchor.minY, w.anchor.maxX, w.anchor.maxY};
    readFloats(def, "anchor", anchor);
    w.anchor = {anchor[0], anchor[1], anchor[2], anchor[3]};

    float offset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    readFloats(def, "offset", offset);
    w.offset = {offset[0], offset[1], offset[2], offset[3]};

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    readFloats(def, "color", color);
    w.color = {color[0], color[1], color[2], color[3]};

    if (def.contains("children") && def["children"].is_array())
        for (const auto& child : def["children"]) w.children.push_back(parse(child));
    return w;
}

UIWidget parseString(const std::string& json) {
    auto parsed = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) return UIWidget{};
    return parse(parsed);
}

const UIWidget* find(const UIWidget& root, const std::string& id) {
    if (root.id == id) return &root;
    for (const UIWidget& c : root.children)
        if (const UIWidget* hit = find(c, id)) return hit;
    return nullptr;
}

UIWidget* find(UIWidget& root, const std::string& id) {
    return const_cast<UIWidget*>(find(static_cast<const UIWidget&>(root), id));
}

} // namespace ui
} // namespace meat
