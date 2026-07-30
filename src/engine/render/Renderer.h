#pragma once
#include "engine/render/Camera.h"
#include "engine/render/GlObjects.h"
#include "engine/render/Shader.h"
#include "engine/voxel/ChunkMesher.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace meat {

class Window;

struct PsxOptions {
    bool nearestFiltering = true;
    float internalScale = 0.5f; // fraction of framebuffer size; 0.5 => 800x450 at 1600x900
    bool dither = true;
    bool fog = true;
    glm::vec3 fogColor{0.10f, 0.11f, 0.13f};
    float fogStart = 30.0f;
    float fogEnd = 120.0f;
};

using MeshHandle = std::uint32_t;    // 0 = invalid
using TextureHandle = std::uint32_t; // 0 = invalid

inline constexpr int kMaxPointLights = 32;
inline constexpr int kMaxSpotLights = 8;

// Forward Blinn-Phong renderer with the PSX post pipeline (half-res target,
// nearest upscale, ordered dither, vertex fog). All methods main thread only.
class Renderer {
public:
    bool init(Window& window);
    void reloadShaders(); // F6

    MeshHandle uploadChunkMesh(const ChunkMeshData& data);
    void destroyMesh(MeshHandle mesh);
    TextureHandle loadTexture(const std::filesystem::path& path);
    void setAtlas(TextureHandle atlas); // block atlas sampled by every chunk draw

    void beginFrame(const Camera& camera, float alpha);
    void submitChunk(MeshHandle mesh, glm::vec3 originWorld);
    void submitMesh(MeshHandle mesh, const glm::mat4& transform, TextureHandle albedo);
    void setDirectionalLight(glm::vec3 dir, glm::vec3 color);
    void submitPointLight(glm::vec3 pos, glm::vec3 color, float radius);
    void submitSpotLight(glm::vec3 pos, glm::vec3 dir, glm::vec3 color, float radius, float angle);
    void drawCrosshair();
    void endFrame();

    PsxOptions psx;

private:
    // std140 mirror of the FrameData block (binding 0). vec4/mat4 members only,
    // so the C++ layout matches std140 exactly; static_asserts in Renderer.cpp.
    struct GpuPointLight {
        glm::vec4 posRadius; // xyz world pos, w radius (m)
        glm::vec4 color;
    };
    struct GpuSpotLight {
        glm::vec4 posRadius;   // xyz world pos, w radius (m)
        glm::vec4 dirCosAngle; // xyz normalized direction, w cos(half-angle)
        glm::vec4 color;
    };
    struct FrameUbo {
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec4 camPos;       // xyz
        glm::vec4 fogParams;    // x fogStart, y fogEnd, z fog enabled (0/1)
        glm::vec4 fogColor;     // rgb
        glm::vec4 dirLightDir;  // xyz normalized, direction the light travels
        glm::vec4 dirLightColor;
        glm::ivec4 lightCounts; // x point count, y spot count
        GpuPointLight pointLights[kMaxPointLights];
        GpuSpotLight spotLights[kMaxSpotLights];
    };

    struct GpuMesh {
        GlVertexArray vao;
        GlBuffer vbo;
        GlBuffer ibo;
        GLsizei indexCount = 0;
    };
    struct ChunkDraw {
        MeshHandle mesh;
        glm::vec3 origin;
    };
    struct MeshDraw {
        MeshHandle mesh;
        glm::mat4 transform;
        TextureHandle albedo;
    };

    void ensurePsxTarget(glm::ivec2 framebufferSize);
    void flushScenePasses();
    void drawCrosshairPass(glm::ivec2 targetSize);
    void resolveToBackbuffer();

    Window* m_window = nullptr;

    Shader m_chunkShader;
    Shader m_meshShader;
    Shader m_resolveShader;
    GlShaderProgram m_crosshairProgram; // trivial, source embedded in Renderer.cpp

    GlBuffer m_frameUbo;
    FrameUbo m_frame{};

    GlFramebuffer m_psxFbo;
    GlTexture m_psxColor;
    GlTexture m_psxDepth;
    glm::ivec2 m_psxSize{0, 0};

    GlVertexArray m_fullscreenVao; // empty; resolve.vert builds the triangle from gl_VertexID
    GlVertexArray m_crosshairVao;
    GlBuffer m_crosshairVbo;

    std::unordered_map<MeshHandle, GpuMesh> m_meshes;
    std::unordered_map<TextureHandle, GlTexture> m_textures;
    MeshHandle m_nextMesh = 1;
    TextureHandle m_nextTexture = 1;
    TextureHandle m_atlas = 0;

    std::vector<ChunkDraw> m_chunkDraws;
    std::vector<MeshDraw> m_meshDraws;
    int m_pointCount = 0;
    int m_spotCount = 0;
    bool m_crosshairRequested = false;

    glm::ivec2 m_fbSize{0, 0};
    bool m_usePsxTarget = false;
};

} // namespace meat
