#pragma once
#include "engine/core/EditorHost.h"

#include <glm/glm.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

struct ImGuiInputTextCallbackData; // fwd: code-editor resize callback signature

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

    // IDE panels — just more ImGui windows drawn inside update(); all file I/O
    // goes through the ctx callbacks (never std::filesystem in the editor).
    void drawAssetBrowser(EditorContext& ctx);
    void drawDirTree(EditorContext& ctx, const std::string& dir);
    void drawCodeEditor(EditorContext& ctx);
    void openLuaFile(EditorContext& ctx, const std::string& path);
    const std::vector<std::string>& listDir(EditorContext& ctx, const std::string& dir);
    void setCodeStatus(std::string text);
    // std::string-backed InputTextMultiline resize handler (imgui_stdlib pattern).
    static int codeResizeCb(ImGuiInputTextCallbackData* data);

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

    // --- Asset browser -----------------------------------------------------
    // Directory listings are cached (keyed by project-relative dir) and only
    // fetched from ctx.listFiles on first expand or after a Refresh — never per
    // frame. A trailing "/" in a listing entry marks a subdirectory.
    std::map<std::string, std::vector<std::string>> m_dirCache;
    std::string m_selectedAsset; // currently highlighted file in the tree

    // --- Code editor -------------------------------------------------------
    // The open script's project-relative path (empty = nothing open) and its
    // editable text. The buffer is a std::string grown by codeResizeCb via
    // ImGuiInputTextFlags_CallbackResize, so there is no fixed length cap.
    std::string m_codePath;
    std::string m_codeText;
    bool m_codeDirty = false;
    std::string m_codeStatus;
    float m_codeStatusTtl = 0.0f;
};

} // namespace meat
