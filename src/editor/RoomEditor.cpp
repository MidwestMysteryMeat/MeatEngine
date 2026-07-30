#include "editor/RoomEditor.h"

#include "engine/core/ViewMath.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <ImGuizmo.h> // must follow imgui.h — its header uses ImGui types

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

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
    m_previewLights = 0;
    ctx.buildBlock = m_buildBlock; // ctx is rebuilt per frame; the editor owns persistence
    if (m_statusTtl > 0.0f && (m_statusTtl -= dt) <= 0.0f) m_status.clear();

    updateFlyCamera(ctx, dt);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float aspect = vp->Size.y > 0.0f ? vp->Size.x / vp->Size.y : 16.0f / 9.0f;
    const glm::mat4 view = ctx.camera.view();
    const glm::mat4 proj = ctx.camera.proj(aspect);

    drawTopBar(ctx);
    drawToolbar();
    drawOutliner(ctx);
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
    if (ctx.lights.empty() && ctx.seedVolumes.empty())
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
    }
    ImGui::End();
}

void RoomEditor::drawGizmo(EditorContext& ctx, const glm::mat4& view, const glm::mat4& proj) {
    if (m_selKind != Selection::Light || m_selIndex < 0 ||
        m_selIndex >= static_cast<int>(ctx.lights.size()))
        return;
    EditorLight& light = ctx.lights[static_cast<std::size_t>(m_selIndex)];

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
    glm::mat4 model = glm::translate(glm::mat4(1.0f), light.pos);
    if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), ImGuizmo::TRANSLATE,
                             ImGuizmo::WORLD, glm::value_ptr(model)))
        light.pos = glm::vec3(model[3]);
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

} // namespace meat
