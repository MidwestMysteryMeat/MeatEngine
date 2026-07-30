#pragma once
#include "engine/core/EditorHost.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>

namespace meat {

// Room Designer: free-fly camera, voxel brush tools (place/erase/wall/floor/
// platform/doorway), light placement, and seed-volume authoring. All world
// writes go through ctx.requestVoxelOp so editing is server-validated and
// works over the network like any other client intent.
class RoomEditor final : public IEditor {
public:
    void update(EditorContext& ctx, float dt) override;

private:
    enum class Tool { Place, Erase, Wall, Floor, Platform, Doorway, Light, SeedVolume };
    enum class Selection { None, Light, Volume };

    void updateFlyCamera(EditorContext& ctx, float dt);
    void drawTopBar(EditorContext& ctx);
    void drawToolbar();
    void drawOutliner(EditorContext& ctx);
    void drawGizmo(EditorContext& ctx, const glm::mat4& view, const glm::mat4& proj);
    void handleTool(EditorContext& ctx, glm::vec3 rayOrigin, glm::vec3 rayDir);

    void fillBox(EditorContext& ctx, glm::ivec3 lo, glm::ivec3 hi, BlockId block);
    void carveDoorway(EditorContext& ctx, glm::ivec3 voxel, glm::ivec3 normal);
    bool toolBox(glm::ivec3 a, glm::ivec3 b, glm::ivec3& lo, glm::ivec3& hi) const;
    glm::ivec3 snapVoxel(glm::ivec3 v) const;

    void submitPreviewLight(EditorContext& ctx, glm::vec3 pos, glm::vec3 color, float radius);
    void previewBox(EditorContext& ctx, glm::ivec3 lo, glm::ivec3 hi, glm::vec3 color,
                    bool withCenter);
    void setStatus(std::string text);

    Tool m_tool = Tool::Place;
    BlockId m_buildBlock = 1; // stone in the default palette
    int m_wallHeight = 6;     // voxels (3 m)
    int m_platformOffset = 4; // voxels above the clicked anchor
    int m_snap = 1;           // 1 = off, else round anchors to multiples of 2/4
    std::uint32_t m_nextSeed = 42;

    bool m_flying = false;
    float m_flySpeed = 8.0f; // m/s, scroll-adjusted while flying

    std::optional<glm::ivec3> m_anchor; // first click of two-click tools

    Selection m_selKind = Selection::None;
    int m_selIndex = -1;

    int m_previewLights = 0; // per-frame budget so previews can't eat the light UBO
    std::string m_status;
    float m_statusTtl = 0.0f;
};

} // namespace meat
