#include "editor/RoomEditor.h"

#include "engine/core/ViewMath.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <ImGuizmo.h> // must follow imgui.h — its header uses ImGui types

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <utility>

namespace meat {

namespace {

constexpr float kMaxPitch = glm::radians(89.0f);
constexpr float kPickDistance = 100.0f;
constexpr int kMaxBatchOps = 4096;   // sanity cap: a runaway drag must not flood the server
constexpr int kMaxPreviewLights = 16;

// Preview markers are point lights, not sprites: submitSprite needs a
// TextureHandle the editor doesn't own, while lights are visible, free,
// and already capped by the renderer's per-frame budget.
constexpr glm::vec3 kBuildPreview{0.3f, 0.9f, 0.3f};
constexpr glm::vec3 kErasePreview{0.9f, 0.3f, 0.3f};
constexpr glm::vec3 kVolumePreview{0.15f, 0.2f, 0.6f};

const char* const kToolNames[] = {"Place",   "Erase",   "Wall",  "Floor",
                                  "Platform", "Doorway", "Light", "Seed Volume"};

// Content-browser group labels, indexed by RoomEditor::AssetKind (minus Count).
const char* const kAssetKindNames[] = {"Models", "Textures", "Scripts", "Shaders", "Other"};

// Short tile tag drawn on a content-browser placeholder square, indexed by kind.
const char* const kAssetKindTags[] = {"MDL", "TEX", "LUA", "SHD", "FILE"};

// Distinct restrained accent per asset kind for the placeholder tile — colours
// read as a family (muted, not neon) so the grid scans by type at a glance.
ImVec4 kindTileColor(int kind) {
    switch (kind) {
    case 0: return ImVec4(0.22f, 0.42f, 0.70f, 1.00f); // Models  — blue
    case 1: return ImVec4(0.28f, 0.55f, 0.42f, 1.00f); // Textures— green
    case 2: return ImVec4(0.62f, 0.45f, 0.24f, 1.00f); // Scripts — amber
    case 3: return ImVec4(0.50f, 0.33f, 0.62f, 1.00f); // Shaders — violet
    default: return ImVec4(0.34f, 0.35f, 0.38f, 1.00f); // Other  — grey
    }
}
// Folder tiles get their own warm-grey so they never look like an asset kind.
const ImVec4 kFolderTileColor(0.55f, 0.48f, 0.28f, 1.00f);

// Truncate label to fit within maxW pixels, appending an ellipsis when clipped,
// so tile captions stay on one line under a fixed-width square.
std::string fitLabel(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string out = s;
    while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > maxW)
        out.pop_back();
    return out + "...";
}

// Human-readable byte count for the content browser's size column.
std::string humanSize(std::uintmax_t bytes) {
    const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

int snapDown(int v, int step) {
    // floor-division snap so negative coords snap toward -inf, not toward 0
    const int r = v % step;
    return v - (r < 0 ? r + step : r);
}

glm::vec3 voxelCenter(glm::ivec3 v) {
    return (glm::vec3(v) + 0.5f) * kVoxelSize;
}

bool worldClickable(bool flying) {
    if (!ImGui::IsMouseClicked(0)) return false;
    if (ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return false;
    // While flying the cursor is captured and frozen — ImGui's hover state is
    // stale, so only respect WantCaptureMouse with a free cursor.
    return flying || !ImGui::GetIO().WantCaptureMouse;
}

} // namespace

void RoomEditor::update(EditorContext& ctx, float dt) {
    if (!m_themed) { // one-shot: restyle ImGui the first frame the editor runs
        applyEditorTheme();
        m_themed = true;
    }
    m_previewLights = 0;
    ctx.buildBlock = m_buildBlock; // ctx is rebuilt per frame; the editor owns persistence
    if (m_statusTtl > 0.0f && (m_statusTtl -= dt) <= 0.0f) m_status.clear();
    if (m_codeStatusTtl > 0.0f && (m_codeStatusTtl -= dt) <= 0.0f) m_codeStatus.clear();
    if (m_importStatusTtl > 0.0f && (m_importStatusTtl -= dt) <= 0.0f) m_importStatus.clear();

    // OS drag-drop: any files dropped on the window this frame get imported, so a
    // dev can drop an .fbx straight onto the viewport instead of typing a path.
    for (const std::string& dropped : ctx.input.drainDroppedPaths()) doImport(ctx, dropped);

    updateFlyCamera(ctx, dt);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float aspect = vp->Size.y > 0.0f ? vp->Size.x / vp->Size.y : 16.0f / 9.0f;
    const glm::mat4 view = ctx.camera.view();
    const glm::mat4 proj = ctx.camera.proj(aspect);

    drawTopBar(ctx);
    drawToolbar();
    drawOutliner(ctx);
    drawAssetBrowser(ctx);
    drawContentBrowser(ctx);
    drawCodeEditor(ctx);
    if (!m_flying) drawGizmo(ctx, view, proj);

    // Picking ray: camera center while flying, otherwise unproject the cursor.
    glm::vec3 rayDir = ctx.camera.forward();
    if (!m_flying) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float nx = 2.0f * (mouse.x - vp->Pos.x) / vp->Size.x - 1.0f;
        const float ny = 1.0f - 2.0f * (mouse.y - vp->Pos.y) / vp->Size.y;
        const glm::mat4 invVp = glm::inverse(proj * view);
        glm::vec4 pNear = invVp * glm::vec4(nx, ny, -1.0f, 1.0f);
        glm::vec4 pFar = invVp * glm::vec4(nx, ny, 1.0f, 1.0f);
        pNear /= pNear.w;
        pFar /= pFar.w;
        rayDir = glm::normalize(glm::vec3(pFar) - glm::vec3(pNear));
    }
    handleTool(ctx, ctx.camera.pos, rayDir);

    // Visualize the selected seed volume with dim corner lights.
    if (m_selKind == Selection::Volume && m_selIndex >= 0 &&
        m_selIndex < static_cast<int>(ctx.seedVolumes.size())) {
        const SeedVolume& sv = ctx.seedVolumes[static_cast<std::size_t>(m_selIndex)];
        previewBox(ctx, sv.min, sv.max, kVolumePreview, false);
    }

    if (ctx.input.pressed(GLFW_KEY_ESCAPE)) m_anchor.reset();

    // Delete removes the selected prop (matching the Outliner's Delete button).
    if (m_selKind == Selection::Prop && ctx.input.pressed(GLFW_KEY_DELETE) && m_selIndex >= 0 &&
        m_selIndex < static_cast<int>(ctx.props.size())) {
        ctx.props.erase(ctx.props.begin() + m_selIndex);
        m_selKind = Selection::None;
        m_selIndex = -1;
    }
}

void RoomEditor::updateFlyCamera(EditorContext& ctx, float dt) {
    const bool rmb = ctx.input.down(GLFW_MOUSE_BUTTON_RIGHT);
    if (!m_flying && rmb && !ImGui::GetIO().WantCaptureMouse && !ImGuizmo::IsUsing()) {
        m_flying = true;
        ctx.setRelativeMouse(true);
        ctx.input.consumeScrollSteps(); // discard notches accumulated while free,
                                        // else the speed jumps on fly start
    } else if (m_flying && !rmb) {
        m_flying = false;
        ctx.setRelativeMouse(false);
    }
    if (!m_flying) return;

    const glm::vec2 look = ctx.input.mouseDelta() * ctx.input.sensitivity;
    ctx.camera.yaw -= look.x; // matches the game: mouse right turns right (yaw decreases)
    ctx.camera.pitch = glm::clamp(ctx.camera.pitch - look.y, -kMaxPitch, kMaxPitch);

    if (const int steps = ctx.input.consumeScrollSteps(); steps != 0)
        m_flySpeed = glm::clamp(m_flySpeed + 2.0f * static_cast<float>(steps), 2.0f, 64.0f);

    const glm::vec3 fwd = ctx.camera.forward();
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 move{0.0f};
    if (ctx.input.down(GLFW_KEY_W)) move += fwd;
    if (ctx.input.down(GLFW_KEY_S)) move -= fwd;
    if (ctx.input.down(GLFW_KEY_D)) move += right;
    if (ctx.input.down(GLFW_KEY_A)) move -= right;
    if (ctx.input.down(GLFW_KEY_E)) move.y += 1.0f;
    if (ctx.input.down(GLFW_KEY_Q)) move.y -= 1.0f;
    if (glm::dot(move, move) > 0.0f) {
        const float speed = m_flySpeed * (ctx.input.down(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
        ctx.camera.pos += glm::normalize(move) * speed * dt;
    }
}

// Modern dark tool theme: neutral greys with one restrained blue accent for
// active/selected/header states. Metrics give panels a little breathing room and
// gentle rounding. Applied once to the shared ImGui style; only editor windows
// are drawn while this is active, so the in-game HUD is untouched.
void RoomEditor::applyEditorTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Metrics — soft rounding + consistent padding for a tidy tool look.
    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = ImVec2(10.0f, 10.0f);
    s.FramePadding = ImVec2(8.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 10.0f;
    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);

    // Palette: neutral charcoal surfaces + a single blue accent.
    const ImVec4 accent(0.26f, 0.59f, 0.98f, 1.00f); // active/selected blue
    const ImVec4 accentDim(0.26f, 0.59f, 0.98f, 0.55f);
    const ImVec4 bg(0.11f, 0.115f, 0.13f, 1.00f);      // window body
    const ImVec4 panel(0.16f, 0.17f, 0.19f, 1.00f);    // frames/inputs
    const ImVec4 panelHover(0.22f, 0.23f, 0.26f, 1.00f);
    const ImVec4 header(0.20f, 0.21f, 0.24f, 1.00f);
    const ImVec4 border(0.00f, 0.00f, 0.00f, 0.35f);
    const ImVec4 text(0.88f, 0.89f, 0.91f, 1.00f);
    const ImVec4 textDim(0.50f, 0.52f, 0.56f, 1.00f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.135f, 0.15f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.14f, 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = panelHover;
    c[ImGuiCol_FrameBgActive] = accentDim;
    c[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.095f, 0.11f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.15f, 0.18f, 0.97f); // slightly translucent
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.09f, 0.095f, 0.11f, 0.75f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.135f, 0.15f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.095f, 0.11f, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.29f, 0.32f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.37f, 0.40f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.68f, 1.00f, 1.00f);
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = panelHover;
    c[ImGuiCol_ButtonActive] = accentDim;
    c[ImGuiCol_Header] = header;                     // selectables/tree
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    c[ImGuiCol_HeaderActive] = accentDim;
    c[ImGuiCol_Separator] = ImVec4(0.28f, 0.29f, 0.32f, 0.60f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.29f, 0.32f, 0.60f);
    c[ImGuiCol_ResizeGripHovered] = accentDim;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered] = accentDim;
    c[ImGuiCol_TabActive] = ImVec4(0.20f, 0.29f, 0.42f, 1.00f);
    c[ImGuiCol_TabUnfocused] = ImVec4(0.11f, 0.115f, 0.13f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.16f, 0.19f, 0.24f, 1.00f);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TextSelectedBg] = accentDim;
    c[ImGuiCol_NavHighlight] = accent;
}

void RoomEditor::drawTopBar(EditorContext& ctx) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + 12.0f, vp->Pos.y + 12.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Room Designer", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("tool: %s", kToolNames[static_cast<int>(m_tool)]);

    int block = static_cast<int>(m_buildBlock);
    ImGui::RadioButton("stone", &block, 1);
    ImGui::SameLine();
    ImGui::RadioButton("dirt", &block, 2);
    ImGui::SameLine();
    ImGui::RadioButton("grass", &block, 3);
    m_buildBlock = static_cast<BlockId>(block);
    ctx.buildBlock = m_buildBlock;

    if (ImGui::Button("Save World")) {
        ctx.requestWorldSave();
        setStatus("world save requested");
    }
    ImGui::TextDisabled("RMB fly (WASD/QE, wheel = speed) | LMB apply | Esc cancel | F1 exit");
    if (!m_status.empty())
        ImGui::TextColored({1.0f, 0.85f, 0.3f, 1.0f}, "%s", m_status.c_str());
    ImGui::End();
}

void RoomEditor::drawToolbar() {
    ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    int tool = static_cast<int>(m_tool);
    for (int i = 0; i < static_cast<int>(std::size(kToolNames)); ++i)
        ImGui::RadioButton(kToolNames[i], &tool, i);
    if (tool != static_cast<int>(m_tool)) {
        m_tool = static_cast<Tool>(tool);
        m_anchor.reset();
    }

    ImGui::Separator();
    if (m_tool == Tool::Wall)
        ImGui::SliderInt("height (vox)", &m_wallHeight, 1, 16);
    if (m_tool == Tool::Platform)
        ImGui::SliderInt("offset (vox)", &m_platformOffset, 1, 32);
    if (m_tool == Tool::SeedVolume) {
        int seed = static_cast<int>(m_nextSeed);
        if (ImGui::InputInt("seed", &seed)) m_nextSeed = static_cast<std::uint32_t>(seed);
    }

    ImGui::Text("snap");
    ImGui::SameLine();
    ImGui::RadioButton("off", &m_snap, 1);
    ImGui::SameLine();
    ImGui::RadioButton("2", &m_snap, 2);
    ImGui::SameLine();
    ImGui::RadioButton("4", &m_snap, 4);

    if (m_anchor)
        ImGui::Text("anchor %d %d %d", m_anchor->x, m_anchor->y, m_anchor->z);
    ImGui::End();
}

void RoomEditor::drawOutliner(EditorContext& ctx) {
    ImGui::Begin("Outliner", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    for (int i = 0; i < static_cast<int>(ctx.lights.size()); ++i) {
        const EditorLight& light = ctx.lights[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        char label[32];
        std::snprintf(label, sizeof(label), "light %d (%s)", i,
                      light.type == 0 ? "point" : "spot");
        if (ImGui::Selectable(label, m_selKind == Selection::Light && m_selIndex == i)) {
            m_selKind = Selection::Light;
            m_selIndex = i;
        }
        ImGui::PopID();
    }
    for (int i = 0; i < static_cast<int>(ctx.seedVolumes.size()); ++i) {
        const SeedVolume& sv = ctx.seedVolumes[static_cast<std::size_t>(i)];
        ImGui::PushID(1000 + i);
        char label[40];
        std::snprintf(label, sizeof(label), "volume %d [seed %u]", i, sv.seed);
        if (ImGui::Selectable(label, m_selKind == Selection::Volume && m_selIndex == i)) {
            m_selKind = Selection::Volume;
            m_selIndex = i;
        }
        ImGui::PopID();
    }
    for (int i = 0; i < static_cast<int>(ctx.props.size()); ++i) {
        const EditorProp& p = ctx.props[static_cast<std::size_t>(i)];
        ImGui::PushID(2000 + i);
        const std::size_t slash = p.assetPath.find_last_of('/');
        const std::string leaf =
            slash == std::string::npos ? p.assetPath : p.assetPath.substr(slash + 1);
        char label[80];
        std::snprintf(label, sizeof(label), "prop %d (%s)", i, leaf.c_str());
        if (ImGui::Selectable(label, m_selKind == Selection::Prop && m_selIndex == i)) {
            m_selKind = Selection::Prop;
            m_selIndex = i;
        }
        ImGui::PopID();
    }
    if (ctx.lights.empty() && ctx.seedVolumes.empty() && ctx.props.empty())
        ImGui::TextDisabled("(empty)");

    if (m_selKind == Selection::Light && m_selIndex >= 0 &&
        m_selIndex < static_cast<int>(ctx.lights.size())) {
        ImGui::Separator();
        ImGui::Text("Properties");
        EditorLight& light = ctx.lights[static_cast<std::size_t>(m_selIndex)];
        const char* const types[] = {"point", "spot"};
        ImGui::Combo("type", &light.type, types, 2);
        ImGui::ColorEdit3("color", glm::value_ptr(light.color));
        ImGui::DragFloat("radius", &light.radius, 0.1f, 0.5f, 64.0f);
        if (light.type == 1) {
            ImGui::SliderFloat("angle", &light.angle, 0.05f, 1.4f, "%.2f rad");
            if (ImGui::Button("dir = camera fwd")) light.dir = ctx.camera.forward();
        }
        if (ImGui::Button("Delete")) {
            ctx.lights.erase(ctx.lights.begin() + m_selIndex);
            m_selKind = Selection::None;
            m_selIndex = -1;
        }
    } else if (m_selKind == Selection::Volume && m_selIndex >= 0 &&
               m_selIndex < static_cast<int>(ctx.seedVolumes.size())) {
        ImGui::Separator();
        ImGui::Text("Properties");
        SeedVolume& sv = ctx.seedVolumes[static_cast<std::size_t>(m_selIndex)];
        int seed = static_cast<int>(sv.seed);
        if (ImGui::InputInt("seed", &seed)) sv.seed = static_cast<std::uint32_t>(seed);
        ImGui::Text("min %d %d %d  max %d %d %d", sv.min.x, sv.min.y, sv.min.z, sv.max.x,
                    sv.max.y, sv.max.z);
        if (ImGui::Button("Delete")) {
            ctx.seedVolumes.erase(ctx.seedVolumes.begin() + m_selIndex);
            m_selKind = Selection::None;
            m_selIndex = -1;
        }
    } else if (m_selKind == Selection::Prop && m_selIndex >= 0 &&
               m_selIndex < static_cast<int>(ctx.props.size())) {
        ImGui::Separator();
        ImGui::Text("Properties");
        EditorProp& prop = ctx.props[static_cast<std::size_t>(m_selIndex)];
        ImGui::TextDisabled("%s", prop.assetPath.c_str());
        const glm::vec3 pos = glm::vec3(prop.transform[3]);
        ImGui::Text("pos %.1f %.1f %.1f", pos.x, pos.y, pos.z);
        ImGui::Text("gizmo");
        ImGui::SameLine();
        ImGui::RadioButton("move", &m_propGizmoOp, 0);
        ImGui::SameLine();
        ImGui::RadioButton("rotate", &m_propGizmoOp, 1);
        ImGui::SameLine();
        ImGui::RadioButton("scale", &m_propGizmoOp, 2);
        if (ImGui::Button("Delete")) {
            ctx.props.erase(ctx.props.begin() + m_selIndex);
            m_selKind = Selection::None;
            m_selIndex = -1;
        }
    }
    ImGui::End();
}

void RoomEditor::drawGizmo(EditorContext& ctx, const glm::mat4& view, const glm::mat4& proj) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);

    if (m_selKind == Selection::Light && m_selIndex >= 0 &&
        m_selIndex < static_cast<int>(ctx.lights.size())) {
        // Lights are a point: translate only.
        EditorLight& light = ctx.lights[static_cast<std::size_t>(m_selIndex)];
        glm::mat4 model = glm::translate(glm::mat4(1.0f), light.pos);
        if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), ImGuizmo::TRANSLATE,
                                 ImGuizmo::WORLD, glm::value_ptr(model)))
            light.pos = glm::vec3(model[3]);
    } else if (m_selKind == Selection::Prop && m_selIndex >= 0 &&
               m_selIndex < static_cast<int>(ctx.props.size())) {
        // Props carry a full TRS: move/rotate/scale, manipulated in place. Scale
        // reads more predictably in local space; move/rotate use world axes.
        // NOTE: gizmo edits mutate only the LOCAL render copy — the server prop and
        // its collider are not updated. Gizmo-move-after-place sync is out of scope
        // for the prop-sync pass (placement + collision + sync + save is the goal).
        EditorProp& prop = ctx.props[static_cast<std::size_t>(m_selIndex)];
        const ImGuizmo::OPERATION op = m_propGizmoOp == 1   ? ImGuizmo::ROTATE
                                       : m_propGizmoOp == 2 ? ImGuizmo::SCALE
                                                            : ImGuizmo::TRANSLATE;
        const ImGuizmo::MODE mode = op == ImGuizmo::SCALE ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode,
                             glm::value_ptr(prop.transform));
    }
}

void RoomEditor::handleTool(EditorContext& ctx, glm::vec3 rayOrigin, glm::vec3 rayDir) {
    const auto hit = ctx.voxels.raycast(rayOrigin, rayDir, kPickDistance);
    if (!hit) return;
    const bool click = worldClickable(m_flying);

    switch (m_tool) {
    case Tool::Place: {
        const glm::ivec3 target = hit->voxel + hit->normal;
        submitPreviewLight(ctx, voxelCenter(target), kBuildPreview, 2.0f);
        if (click) ctx.requestVoxelOp(target, ctx.buildBlock);
        break;
    }
    case Tool::Erase: {
        submitPreviewLight(ctx, voxelCenter(hit->voxel), kErasePreview, 2.0f);
        if (click) ctx.requestVoxelOp(hit->voxel, 0);
        break;
    }
    case Tool::Wall:
    case Tool::Floor:
    case Tool::Platform: {
        const glm::ivec3 target = snapVoxel(hit->voxel + hit->normal);
        if (!m_anchor) {
            submitPreviewLight(ctx, voxelCenter(target), kBuildPreview, 2.0f);
            if (click) m_anchor = target;
        } else {
            glm::ivec3 lo, hi;
            if (toolBox(*m_anchor, target, lo, hi)) {
                previewBox(ctx, lo, hi, kBuildPreview, true);
                if (click) {
                    fillBox(ctx, lo, hi, ctx.buildBlock);
                    m_anchor.reset();
                }
            }
        }
        break;
    }
    case Tool::Doorway: {
        submitPreviewLight(ctx, voxelCenter(hit->voxel), kErasePreview, 2.0f);
        if (click) carveDoorway(ctx, hit->voxel, hit->normal);
        break;
    }
    case Tool::Light: {
        const glm::vec3 point =
            rayOrigin + rayDir * hit->t + glm::vec3(hit->normal) * 0.25f;
        submitPreviewLight(ctx, point, kBuildPreview, 2.0f);
        if (click) {
            EditorLight light;
            light.pos = point;
            light.color = {1.0f, 0.9f, 0.75f}; // warm white
            light.radius = 8.0f;
            light.dir = ctx.camera.forward(); // MVP spot dir = placing view direction
            ctx.lights.push_back(light);
            m_selKind = Selection::Light;
            m_selIndex = static_cast<int>(ctx.lights.size()) - 1;
        }
        break;
    }
    case Tool::SeedVolume: {
        const glm::ivec3 target = snapVoxel(hit->voxel + hit->normal);
        if (!m_anchor) {
            submitPreviewLight(ctx, voxelCenter(target), kVolumePreview, 2.0f);
            if (click) m_anchor = target;
        } else {
            const glm::ivec3 lo = glm::min(*m_anchor, target);
            const glm::ivec3 hi = glm::max(*m_anchor, target);
            previewBox(ctx, lo, hi, kVolumePreview, true);
            if (click) {
                ctx.seedVolumes.push_back(SeedVolume{lo, hi, m_nextSeed});
                m_selKind = Selection::Volume;
                m_selIndex = static_cast<int>(ctx.seedVolumes.size()) - 1;
                m_anchor.reset();
            }
        }
        break;
    }
    }
}

// Region between two anchors for the box brushes. Walls collapse to 1 voxel
// thick along whichever horizontal axis moved least.
bool RoomEditor::toolBox(glm::ivec3 a, glm::ivec3 b, glm::ivec3& lo, glm::ivec3& hi) const {
    switch (m_tool) {
    case Tool::Wall: {
        const int y0 = std::min(a.y, b.y);
        if (std::abs(b.x - a.x) >= std::abs(b.z - a.z)) {
            lo = {std::min(a.x, b.x), y0, a.z};
            hi = {std::max(a.x, b.x), y0 + m_wallHeight - 1, a.z};
        } else {
            lo = {a.x, y0, std::min(a.z, b.z)};
            hi = {a.x, y0 + m_wallHeight - 1, std::max(a.z, b.z)};
        }
        return true;
    }
    case Tool::Floor:
    case Tool::Platform: {
        const int y = a.y + (m_tool == Tool::Platform ? m_platformOffset : 0);
        lo = {std::min(a.x, b.x), y, std::min(a.z, b.z)};
        hi = {std::max(a.x, b.x), y, std::max(a.z, b.z)};
        return true;
    }
    default:
        return false;
    }
}

void RoomEditor::fillBox(EditorContext& ctx, glm::ivec3 lo, glm::ivec3 hi, BlockId block) {
    const glm::ivec3 d = hi - lo + 1;
    const long long count =
        static_cast<long long>(d.x) * static_cast<long long>(d.y) * static_cast<long long>(d.z);
    if (count > kMaxBatchOps) {
        setStatus("region too large (" + std::to_string(count) + " voxels, cap " +
                  std::to_string(kMaxBatchOps) + ")");
        return;
    }
    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                ctx.requestVoxelOp({x, y, z}, block);
}

void RoomEditor::carveDoorway(EditorContext& ctx, glm::ivec3 voxel, glm::ivec3 normal) {
    // 2 wide x 4 high (1 m x 2 m), depth 1 along the face normal's axis. An even
    // width can't center exactly on a voxel: the clicked column is the left/lower
    // of the pair; vertically the click sits one voxel above the opening's base.
    const glm::ivec3 wAxis = (normal.x != 0) ? glm::ivec3(0, 0, 1) : glm::ivec3(1, 0, 0);
    for (int h = -1; h <= 2; ++h)
        for (int w = 0; w <= 1; ++w)
            ctx.requestVoxelOp(voxel + wAxis * w + glm::ivec3(0, h, 0), 0);
}

glm::ivec3 RoomEditor::snapVoxel(glm::ivec3 v) const {
    if (m_snap <= 1) return v;
    return {snapDown(v.x, m_snap), snapDown(v.y, m_snap), snapDown(v.z, m_snap)};
}

void RoomEditor::submitPreviewLight(EditorContext& ctx, glm::vec3 pos, glm::vec3 color,
                                    float radius) {
    if (m_previewLights >= kMaxPreviewLights) return;
    ++m_previewLights;
    ctx.renderer.submitPointLight(pos, color, radius);
}

void RoomEditor::previewBox(EditorContext& ctx, glm::ivec3 lo, glm::ivec3 hi, glm::vec3 color,
                            bool withCenter) {
    const glm::vec3 wLo = glm::vec3(lo) * kVoxelSize;
    const glm::vec3 wHi = glm::vec3(hi + 1) * kVoxelSize;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner{(i & 1) ? wHi.x : wLo.x, (i & 2) ? wHi.y : wLo.y,
                               (i & 4) ? wHi.z : wLo.z};
        submitPreviewLight(ctx, corner, color, 2.0f);
    }
    if (withCenter) submitPreviewLight(ctx, (wLo + wHi) * 0.5f, color, 2.0f);
}

void RoomEditor::setStatus(std::string text) {
    m_status = std::move(text);
    m_statusTtl = 4.0f;
}

// ===== IDE panels ==========================================================

// Cached listing: fetch from ctx.listFiles once per dir, then serve from the
// cache until Refresh clears it — so an open tree doesn't re-list every frame.
const std::vector<std::string>& RoomEditor::listDir(EditorContext& ctx,
                                                    const std::string& dir) {
    auto it = m_dirCache.find(dir);
    if (it == m_dirCache.end()) {
        std::vector<std::string> entries;
        if (ctx.listFiles) entries = ctx.listFiles(dir);
        it = m_dirCache.emplace(dir, std::move(entries)).first;
    }
    return it->second;
}

void RoomEditor::drawDirTree(EditorContext& ctx, const std::string& dir) {
    // listDir is cached, so recursing here only calls ctx.listFiles the first
    // time a node is expanded (or after Refresh) — not every frame.
    for (const std::string& name : listDir(ctx, dir)) {
        if (name.empty()) continue;
        const bool isDir = name.back() == '/';
        if (isDir) {
            const std::string leaf = name.substr(0, name.size() - 1);
            const std::string child = dir + "/" + leaf;
            // str_id = full path (unique), label = leaf name.
            if (ImGui::TreeNode(child.c_str(), "%s", leaf.c_str())) {
                drawDirTree(ctx, child);
                ImGui::TreePop();
            }
            continue;
        }

        // File leaf. Show its extension as the type tag. TODO: preview textures/
        // models/shaders in-panel (thumbnail via TextureHandle / SceneCapture);
        // for the MVP we only list non-script files by extension.
        const std::string path = dir + "/" + name;
        const char* dot = std::strrchr(name.c_str(), '.');
        const char* ext = dot ? dot + 1 : "?";
        const bool isLua = dot && std::strcmp(dot, ".lua") == 0;

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                   ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (m_selectedAsset == path) flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::TreeNodeEx(path.c_str(), flags, "%s  [%s]", name.c_str(), ext);
        if (ImGui::IsItemClicked()) m_selectedAsset = path;
        // Double-click a .lua to open it; a selected .lua also gets an "open"
        // button so the action is discoverable without a double-click.
        if (isLua && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            openLuaFile(ctx, path);
        if (isLua && m_selectedAsset == path) {
            ImGui::SameLine();
            ImGui::PushID(path.c_str());
            if (ImGui::SmallButton("open")) openLuaFile(ctx, path);
            ImGui::PopID();
        }
    }
}

void RoomEditor::drawAssetBrowser(EditorContext& ctx) {
    ImGui::Begin("Assets", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::Button("Refresh")) m_dirCache.clear(); // drop cache → re-list on expand
    ImGui::SameLine();
    if (m_selectedAsset.empty())
        ImGui::TextDisabled("(no selection)");
    else
        ImGui::Text("sel: %s", m_selectedAsset.c_str());
    ImGui::TextDisabled("double-click a .lua to edit it below");

    // Import row: paste/type a source path + Import, or drag a file onto the
    // window (handled in update). MVP has no native file-open dialog.
    ImGui::SetNextItemWidth(320.0f);
    const bool entered = ImGui::InputText("##importpath", m_importPath, sizeof(m_importPath),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Import...") || entered) doImport(ctx, m_importPath);
    ImGui::TextDisabled("paste a path to an .fbx/.obj/.glb/.png/.jpg (or drag a file onto the window)");
    if (!m_importStatus.empty()) {
        const bool ok = m_importStatus.rfind("imported", 0) == 0;
        ImGui::TextColored(ok ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "%s", m_importStatus.c_str());
    }
    ImGui::Separator();

    if (!ctx.listFiles) {
        ImGui::TextDisabled("(file listing unavailable)");
        ImGui::End();
        return;
    }
    // Rooted at "assets"; listFiles surfaces scripts/models/textures/shaders as
    // subdirs (trailing "/"), which expand on demand via drawDirTree.
    drawDirTree(ctx, "assets");
    ImGui::End();
}

// Route a source path through ctx.importAsset (the engine validates + copies) and
// surface the result. A successful import invalidates the dir cache so the tree
// re-lists and the new file appears without a manual Refresh.
void RoomEditor::doImport(EditorContext& ctx, const std::string& sourcePath) {
    if (sourcePath.empty()) return;
    if (!ctx.importAsset) {
        setImportStatus("import unavailable");
        return;
    }
    const std::string result = ctx.importAsset(sourcePath);
    if (result.empty()) return; // not attempted
    setImportStatus(result);
    if (result.rfind("imported", 0) == 0) m_dirCache.clear();
}

void RoomEditor::setImportStatus(std::string text) {
    m_importStatus = std::move(text);
    m_importStatusTtl = 6.0f;
}

void RoomEditor::openLuaFile(EditorContext& ctx, const std::string& path) {
    if (!ctx.readFile) {
        setCodeStatus("readFile unavailable");
        return;
    }
    m_codeText = ctx.readFile(path);
    m_codePath = path;
    m_codeDirty = false;
    setCodeStatus("opened " + path);
}

// Grows m_codeText as the user types past its current capacity. UserData is the
// std::string itself; keep data->Buf pointed at its storage after the resize.
int RoomEditor::codeResizeCb(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}

void RoomEditor::drawCodeEditor(EditorContext& ctx) {
    ImGui::Begin("Code");
    if (m_codePath.empty()) {
        ImGui::TextDisabled("Open a .lua file from the Assets panel.");
        ImGui::End();
        return;
    }

    // Header shows the open file; '*' marks unsaved edits.
    ImGui::Text("%s%s", m_codePath.c_str(), m_codeDirty ? " *" : "");

    // Saving a script under scripts/ hot-reloads it into the running server so
    // gameplay changes apply live: writeFile → (if under scripts/) reloadScripts.
    const bool underScripts = m_codePath.find("scripts/") != std::string::npos;
    if (ImGui::Button("Save")) {
        if (ctx.writeFile && ctx.writeFile(m_codePath, m_codeText)) {
            m_codeDirty = false;
            if (underScripts && ctx.reloadScripts) {
                const bool ok = ctx.reloadScripts();
                setCodeStatus(ok ? "saved + reloaded" : "saved (reload FAILED)");
            } else {
                setCodeStatus("saved");
            }
        } else {
            setCodeStatus("save FAILED");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) { // re-read, discarding local edits
        if (ctx.readFile) {
            m_codeText = ctx.readFile(m_codePath);
            m_codeDirty = false;
            setCodeStatus("reloaded from disk");
        } else {
            setCodeStatus("readFile unavailable");
        }
    }
    if (!m_codeStatus.empty()) {
        ImGui::SameLine();
        ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.0f}, "%s", m_codeStatus.c_str());
    }

    // Plain editable text — no syntax highlighting for the MVP.
    // TODO: swap for ImGuiColorTextEdit (MIT, ImGui-native) to get Lua colouring.
    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize;
    if (ImGui::InputTextMultiline("##code", m_codeText.data(), m_codeText.capacity() + 1,
                                  ImVec2(-FLT_MIN, -FLT_MIN), flags,
                                  &RoomEditor::codeResizeCb, &m_codeText)) {
        m_codeDirty = true;
    }
    ImGui::End();
}

void RoomEditor::setCodeStatus(std::string text) {
    m_codeStatus = std::move(text);
    m_codeStatusTtl = 4.0f;
}

// ===== Content browser =====================================================

RoomEditor::AssetKind RoomEditor::classifyExt(const std::string& ext) {
    if (ext == ".fbx" || ext == ".obj" || ext == ".glb" || ext == ".gltf")
        return AssetKind::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
        return AssetKind::Texture;
    if (ext == ".lua") return AssetKind::Script;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl")
        return AssetKind::Shader;
    return AssetKind::Other;
}

// Walk assets/ once and cache a flat, type-sorted listing with per-file sizes.
// The editor context doesn't surface --project's dir, so the scan is rooted at
// the runtime assets/ dir — the same root the Assets tree lists. All filesystem
// work is error_code-based so a missing/locked dir can't throw across the UI.
void RoomEditor::rescanContent() {
    namespace fs = std::filesystem;
    m_content.clear();
    m_contentScanned = true;

    std::error_code ec;
    const fs::path root = "assets";
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;

    const fs::recursive_directory_iterator end;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;

        ContentEntry entry;
        entry.name = it->path().filename().string();
        entry.path = fs::relative(it->path(), root, fe).generic_string();
        if (fe || entry.path.empty()) entry.path = it->path().generic_string();
        entry.size = it->file_size(fe);
        if (fe) entry.size = 0;
        entry.kind = classifyExt(toLowerAscii(it->path().extension().string()));
        m_content.push_back(std::move(entry));
    }

    std::sort(m_content.begin(), m_content.end(),
              [](const ContentEntry& a, const ContentEntry& b) {
                  if (a.kind != b.kind) return a.kind < b.kind;
                  return a.name < b.name;
              });
}

void RoomEditor::drawContentBrowser(EditorContext& ctx) {
    // Open roomy and clear of the other default-cascaded panels so the tile grid
    // has space; the dev can still move/resize it (FirstUseEver only seeds it).
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + vp->Size.x - 452.0f, vp->Pos.y + 40.0f},
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({440.0f, 480.0f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Content Browser");
    if (!m_contentScanned) rescanContent(); // first open: scan once, not per frame

    if (ImGui::Button("Refresh")) rescanContent();
    ImGui::SameLine();
    ImGui::Text("%d assets under assets/", static_cast<int>(m_content.size()));

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##contentfilter", "filter by name...", m_contentFilter,
                             sizeof(m_contentFilter));
    const std::string needle = toLowerAscii(m_contentFilter);
    const auto matches = [&](const std::string& name) {
        return needle.empty() || toLowerAscii(name).find(needle) != std::string::npos;
    };

    // Breadcrumb: "assets" root plus each segment of the current sub-folder, each
    // clickable to jump back up. The scan is flat + recursive; m_contentDir just
    // scopes which tiles show. Guard against a stale dir the last scan dropped.
    const std::string prefix = m_contentDir.empty() ? std::string() : m_contentDir + "/";
    ImGui::Separator();
    if (ImGui::SmallButton("assets")) m_contentDir.clear();
    {
        std::string acc;
        std::size_t start = 0;
        while (start <= m_contentDir.size()) {
            const std::size_t slash = m_contentDir.find('/', start);
            const std::size_t end = slash == std::string::npos ? m_contentDir.size() : slash;
            if (end == start) break; // trailing/empty segment
            const std::string seg = m_contentDir.substr(start, end - start);
            acc = acc.empty() ? seg : acc + "/" + seg;
            ImGui::SameLine();
            ImGui::TextDisabled("/");
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(start));
            if (ImGui::SmallButton(seg.c_str())) m_contentDir = acc;
            ImGui::PopID();
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
    }

    // Split the flat scan into this folder's immediate sub-folders + files. A path
    // like "anim_packs/Pack/idle.fbx" under dir "anim_packs" contributes folder
    // "Pack"; with no further slash it is a file that lives in the current dir.
    std::vector<std::string> folders;      // immediate child folder leaf names
    std::vector<const ContentEntry*> files; // files directly in m_contentDir
    for (const ContentEntry& e : m_content) {
        if (e.path.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = e.path.substr(prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            if (matches(e.name)) files.push_back(&e);
        } else {
            const std::string leaf = rest.substr(0, slash);
            if (std::find(folders.begin(), folders.end(), leaf) == folders.end())
                folders.push_back(leaf);
        }
    }
    std::sort(folders.begin(), folders.end());

    // Tile grid: folders first, then files (already kind-then-name sorted by the
    // scan). Each tile is a colour-coded placeholder square + truncated caption;
    // a per-row counter wraps to the panel width. Real rendered previews are out
    // of scope — the square's colour + tag stands in for a thumbnail.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float tile = 84.0f;
    const float availW = ImGui::GetContentRegionAvail().x;
    const int perRow =
        std::max(1, static_cast<int>((availW + style.ItemSpacing.x) / (tile + style.ItemSpacing.x)));

    ImGui::BeginChild("##contentgrid", ImVec2(0.0f, 300.0f), ImGuiChildFlags_Borders);
    int cell = 0;
    const auto nextCell = [&]() {
        if (++cell % perRow != 0) ImGui::SameLine();
    };

    for (const std::string& leaf : folders) {
        ImGui::PushID(("dir/" + leaf).c_str());
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Button, kFolderTileColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(kFolderTileColor.x + 0.12f, kFolderTileColor.y + 0.12f,
                                     kFolderTileColor.z + 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kFolderTileColor);
        const bool enter = ImGui::Button("DIR", ImVec2(tile, tile));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s  (folder)", leaf.c_str());
        if (enter) m_contentDir = prefix + leaf;
        ImGui::TextUnformatted(fitLabel(leaf, tile).c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        nextCell();
    }

    for (const ContentEntry* e : files) {
        ImGui::PushID(e->path.c_str());
        const int k = static_cast<int>(e->kind);
        const bool selected = m_contentSelected == e->path;
        ImVec4 col = kindTileColor(k);
        if (selected) { // brighten the selected tile so it reads as active
            col.x = std::min(1.0f, col.x + 0.18f);
            col.y = std::min(1.0f, col.y + 0.18f);
            col.z = std::min(1.0f, col.z + 0.18f);
        }
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(std::min(1.0f, col.x + 0.12f), std::min(1.0f, col.y + 0.12f),
                                     std::min(1.0f, col.z + 0.12f), 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
        const bool clicked = ImGui::Button(kAssetKindTags[k], ImVec2(tile, tile));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s  |  %s", e->name.c_str(),
                              kAssetKindNames[k], humanSize(e->size).c_str());
            // Double-click a model tile to drop it into the world (mirrors the
            // Place button + the Assets panel's double-click-to-open convention).
            if (e->kind == AssetKind::Model && ImGui::IsMouseDoubleClicked(0)) {
                m_contentSelected = e->path;
                placeSelectedProp(ctx);
            }
        }
        if (clicked) m_contentSelected = e->path;
        ImGui::TextUnformatted(fitLabel(e->name, tile).c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        nextCell();
    }

    if (folders.empty() && files.empty())
        ImGui::TextDisabled(m_content.empty() ? "(no assets under assets/)" : "(empty folder)");
    ImGui::EndChild();

    // Details for the current selection.
    const ContentEntry* sel = nullptr;
    for (const ContentEntry& e : m_content)
        if (e.path == m_contentSelected) {
            sel = &e;
            break;
        }
    ImGui::Separator();
    if (sel) {
        ImGui::Text("%s", sel->name.c_str());
        ImGui::TextDisabled("assets/%s", sel->path.c_str());
        ImGui::TextDisabled("%s  |  %s", kAssetKindNames[static_cast<int>(sel->kind)],
                            humanSize(sel->size).c_str());
        // A model can be dropped into the world: it spawns an EditorProp where the
        // camera is looking (raycast pick), then gets a transform gizmo + Outliner
        // entry. Meshes only — textures/scripts/shaders aren't placeable.
        if (sel->kind == AssetKind::Model) {
            if (ImGui::Button("Place in world")) placeSelectedProp(ctx);
            ImGui::SameLine();
            ImGui::TextDisabled("drops where the camera looks");
        }
    } else {
        ImGui::TextDisabled("(no selection)");
    }
    ImGui::End();
}

void RoomEditor::placeSelectedProp(EditorContext& ctx) {
    if (m_contentSelected.empty()) return;
    // Content paths are relative to the assets/ scan root; props store a
    // project-relative path, which is what the engine's loader expects.
    const std::string assetPath = "assets/" + m_contentSelected;

    // Raycast the camera forward ray against the voxel world and seat the prop on
    // the hit surface; with nothing in view, drop it a few metres ahead so the
    // action never silently no-ops.
    const glm::vec3 origin = ctx.camera.pos;
    const glm::vec3 dir = ctx.camera.forward();
    const auto hit = ctx.voxels.raycast(origin, dir, kPickDistance);
    const glm::vec3 point = hit ? origin + dir * hit->t : origin + dir * 5.0f;

    // Server intent: the prop becomes a shared, collidable, saved world object and
    // comes back via PropAddedMsg into ctx.props (with a real id + collider) — no
    // local push. Selection follows once it lands (the Outliner lists synced props).
    if (ctx.requestPlaceProp)
        ctx.requestPlaceProp(assetPath, glm::translate(glm::mat4(1.0f), point));
    setStatus("placed " + assetPath);
}

} // namespace meat
