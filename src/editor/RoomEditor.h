#pragma once
#include "engine/core/EditorHost.h"
#include "engine/script/NodeGraph.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct ImGuiInputTextCallbackData; // fwd: code-editor resize callback signature
struct ImNodesContext;             // fwd: C6 imnodes context (created lazily)

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
    enum class Selection { None, Light, Volume, Prop };

    // One-time Dear ImGui style: UE5-Slate-inspired dark charcoal + blue select.
    // Editor windows only — the in-game HUD is not drawn while this is active.
    void applyEditorTheme();
    void applyNodeGraphStyle(); // imnodes palette (visual script node colors)

    void updateFlyCamera(EditorContext& ctx, float dt);
    void drawTopBar(EditorContext& ctx);
    void drawNewMapDialog(EditorContext& ctx);
    void drawToolbar();
    void drawOutliner(EditorContext& ctx);
    void drawGizmo(EditorContext& ctx, const glm::mat4& view, const glm::mat4& proj);
    void handleTool(EditorContext& ctx, glm::vec3 rayOrigin, glm::vec3 rayDir);
    void drawNodeGraphDetails(EditorContext& ctx);
    void drawNodeGraphContextMenu(EditorContext& ctx);
    void openGraphNode(int nodeId);
    void focusGraphNode(int nodeId);
    void createObjectNodeFromSelection(EditorContext& ctx);
    void updateObjectHighlight(EditorContext& ctx);

    // IDE panels — just more ImGui windows drawn inside update(); all file I/O
    // goes through the ctx callbacks (never std::filesystem in the editor).
    void drawAssetBrowser(EditorContext& ctx);
    void drawDirTree(EditorContext& ctx, const std::string& dir);
    void doImport(EditorContext& ctx, const std::string& sourcePath);
    void setImportStatus(std::string text);
    void drawCodeEditor(EditorContext& ctx);
    void openLuaFile(EditorContext& ctx, const std::string& path);

    // C9 Output Log — UE-style severity-filtered message browser.
    void drawOutputLog();

    // C6 Node Graph (visual scripting) — imnodes graph that compiles to sandboxed Lua.
    void drawNodeGraph(EditorContext& ctx);
    void ensureNodeGraphContext();
    void loadOrSeedNodeGraph(EditorContext& ctx);
    bool saveAndCompileNodeGraph(EditorContext& ctx);

    // Content browser: a flat, cached, type-grouped listing of the project's
    // assets with per-file sizes. Unlike the Assets tree (which lazily lists via
    // ctx.listFiles), this panel scans the assets/ dir directly with
    // std::filesystem so it can report sizes, and rescans only on first open or
    // when the dev clicks Refresh — never per frame.
    enum class AssetKind { Model, Texture, Script, Shader, Other, Count };
    struct ContentEntry {
        std::string name;        // file name (leaf)
        std::string path;        // path relative to the scan root
        std::uintmax_t size = 0; // bytes
        AssetKind kind = AssetKind::Other;
    };
    void drawContentBrowser(EditorContext& ctx);
    void rescanContent();
    // Spawn an EditorProp for the selected Content-Browser model, dropped where
    // the editor camera looks (raycast against the voxel world), and select it.
    void placeSelectedProp(EditorContext& ctx);
    static AssetKind classifyExt(const std::string& ext);

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
    int m_propGizmoOp = 0; // selected prop's ImGuizmo op: 0 move, 1 rotate, 2 scale
    bool m_propGizmoDirty = false; // true while a gizmo drag has uncommitted transform

    int m_previewLights = 0; // per-frame budget so previews can't eat the light UBO
    std::string m_status;
    float m_statusTtl = 0.0f;

    // --- Asset browser -----------------------------------------------------
    // Directory listings are cached (keyed by project-relative dir) and only
    // fetched from ctx.listFiles on first expand or after a Refresh — never per
    // frame. A trailing "/" in a listing entry marks a subdirectory.
    std::map<std::string, std::vector<std::string>> m_dirCache;
    std::string m_selectedAsset; // currently highlighted file in the tree

    // --- Asset import ------------------------------------------------------
    // No native file dialog is wired: the dev pastes/types a source path here and
    // clicks Import (or drags a file onto the window — drained from Input each
    // frame). The result string is shown transiently, like the code status line;
    // a successful import clears m_dirCache so the tree re-lists the new file.
    char m_importPath[512] = {};
    std::string m_importStatus;
    float m_importStatusTtl = 0.0f;

    // --- Code editor -------------------------------------------------------
    // The open script's project-relative path (empty = nothing open) and its
    // editable text. The buffer is a std::string grown by codeResizeCb via
    // ImGuiInputTextFlags_CallbackResize, so there is no fixed length cap.
    std::string m_codePath;
    std::string m_codeText;
    bool m_codeDirty = false;
    std::string m_codeStatus;
    float m_codeStatusTtl = 0.0f;

    // --- Content browser ---------------------------------------------------
    // m_content is the cached scan; m_contentScanned gates the one-time first
    // scan so the panel populates on open without touching the disk every frame.
    std::vector<ContentEntry> m_content;
    bool m_contentScanned = false;
    char m_contentFilter[128] = {}; // name substring filter (case-insensitive)
    std::string m_contentSelected;  // scan-relative path of the highlighted entry
    // Current sub-folder within assets/ shown by the browser grid ("" = root, else
    // a scan-relative dir like "anim_packs/Action_Adventure_Pack"). The scan stays
    // flat + recursive; this only filters which tiles are shown and drives the
    // breadcrumb. Folders are derived from each entry's path.
    std::string m_contentDir;

    // --- Editor theme ------------------------------------------------------
    // One-shot guard so applyEditorTheme() runs on the first update() only.
    bool m_themed = false;

    // --- New Map (B4) ------------------------------------------------------
    bool m_newMapOpen = false;
    int m_newMapGenre = 0;       // 0 FPS, 1 TPS, 2 Space ship, 3 Racer
    int m_newMapTerrain = 0;     // 0 Normal, 1 Superflat, 2 Void
    int m_newMapEnvironment = 0; // 0 Surface, 1 Underwater, 2 Space
    int m_newMapSeed = 1337;

    // --- Node Graph (C6 visual scripting; not "Blueprints" — UE trademark) ---
    bool m_nodeGraphOpen = true;
    bool m_nodeGraphLoaded = false;
    bool m_nodeGraphDirty = false;
    NodeGraph m_nodeGraph;
    ImNodesContext* m_imnodes = nullptr;
    std::string m_graphStatus;
    float m_graphStatusTtl = 0.0f;
    int m_graphAddKind = 0; // palette index for "Add node"
    // Node ids that already received SetNodeGridSpacePos (imnodes owns drag after).
    std::vector<int> m_graphPlacedIds;
    // Double-click / Open opens a Details panel for this node id (0 = none).
    int m_graphOpenNodeId = 0;
    // Right-click "place node here" spawn position in grid space.
    float m_graphContextGridX = 80.0f;
    float m_graphContextGridY = 80.0f;
    bool m_graphContextOpen = false;
    char m_graphSearch[64] = {};
    // World object highlight driven by selected GetWorldObject / Highlight nodes.
    std::uint32_t m_graphHighlightPropId = 0;
    float m_graphHighlightPulse = 0.0f;

    // --- Output Log (C9) ---------------------------------------------------
    bool m_outputLogOpen = true;
    int m_logFilter = 0; // 0 All, 1 Messages, 2 Warnings, 3 Errors
    char m_logSearch[128] = {};
    bool m_logAutoScroll = true;
    int m_logLastCount = 0; // for auto-scroll when new lines arrive
};

} // namespace meat
