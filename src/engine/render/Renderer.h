#pragma once
#include "engine/anim/Animator.h" // Pose (+ SkinnedVertex/kMaxBones via SkeletalModel.h)
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
    bool vertexJitter = true;   // snap projected verts to the low-res pixel grid (PSX wobble)
    bool fog = true;
    glm::vec3 fogColor{0.10f, 0.11f, 0.13f};
    float fogStart = 30.0f;
    float fogEnd = 120.0f;
    // A2: directional sun shadow map (hard/PCF; low-res edges are on-aesthetic for PSX).
    bool sunShadows = true;
    int shadowMapSize = 2048; // square depth map resolution
    float shadowExtent = 48.0f; // ortho half-size around camera (metres)
    // B3-sky: procedural gradient backdrop (no cubemap). Off keeps clear = fogColor only.
    bool sky = true;
    glm::vec3 skyZenith{0.28f, 0.42f, 0.68f};
    glm::vec3 skyHorizon{0.55f, 0.62f, 0.72f};
    glm::vec3 skyGround{0.12f, 0.11f, 0.10f};
    bool skyStars = false; // sparse procedural stars (Space)
};

using MeshHandle = std::uint32_t;        // 0 = invalid
using TextureHandle = std::uint32_t;     // 0 = invalid
using SkinnedMeshHandle = std::uint32_t; // 0 = invalid; separate space from MeshHandle

// Distinct type rather than a uint32_t alias: submitMesh overloads on
// MaterialHandle vs TextureHandle, which requires them to differ. 0 = invalid.
enum class MaterialHandle : std::uint32_t { Invalid = 0 };

// Albedo + Blinn-Phong params + emissive. Meshes drawn with a bare
// TextureHandle get an implicit default material (tint 1, shininess 32).
struct MaterialDesc {
    TextureHandle albedo = 0;
    glm::vec3 tint{1.0f};
    float shininess = 32.0f;
    glm::vec3 emissive{0.0f};
};

inline constexpr int kMaxPointLights = 32;
inline constexpr int kMaxSpotLights = 8;

// Forward Blinn-Phong renderer with the PSX post pipeline (half-res target,
// nearest upscale, ordered dither, vertex fog). All methods main thread only.
class Renderer {
public:
    bool init(Window& window);
    void reloadShaders(); // F6
    bool captureScreenshot(const std::filesystem::path& path); // F12: backbuffer → PNG

    MeshHandle uploadChunkMesh(const ChunkMeshData& data);
    void destroyMesh(MeshHandle mesh);
    SkinnedMeshHandle uploadSkinnedMesh(const std::vector<SkinnedVertex>& vertices,
                                        const std::vector<std::uint32_t>& indices);
    void destroySkinnedMesh(SkinnedMeshHandle mesh);
    TextureHandle loadTexture(const std::filesystem::path& path);
    // Decode + upload a compressed image (PNG/JPG/TGA) from memory — e.g. an FBX-embedded
    // character texture. `label` is for logging only. Returns 0 on decode failure.
    TextureHandle loadTextureFromMemory(const unsigned char* data, std::size_t size,
                                        const std::string& label);
    void setAtlas(TextureHandle atlas); // block atlas sampled by every chunk draw
    MaterialHandle createMaterial(const MaterialDesc& desc);

    void beginFrame(const Camera& camera, float alpha);
    void submitChunk(MeshHandle mesh, glm::vec3 originWorld);
    void submitMesh(MeshHandle mesh, const glm::mat4& transform, TextureHandle albedo);
    void submitMesh(MeshHandle mesh, const glm::mat4& transform, MaterialHandle material);
    // Draws in the mesh pass (opaque, depth on, inside the PSX target). The
    // pose's skinning matrices are copied at submit time — callers may reuse
    // or discard the Pose immediately. Poses beyond kMaxBones are truncated.
    void submitSkinned(SkinnedMeshHandle mesh, const glm::mat4& transform, const Pose& pose,
                       MaterialHandle material);
    // Camera-facing billboard, alpha-tested, drawn after opaque geometry inside
    // the PSX target. uvRect = {u, v, width, height} within the texture.
    void submitSprite(glm::vec3 center, glm::vec2 size, TextureHandle tex,
                      glm::vec4 uvRect = {0.0f, 0.0f, 1.0f, 1.0f},
                      glm::vec3 tint = {1.0f, 1.0f, 1.0f}, bool fullbright = false);
    // Flat / sky ambient (premultiplied rgb). When hemisphere strength > 0 this is the
    // sky lobe; when strength == 0 it is the classic isotropic ambient.
    void setAmbientLight(glm::vec3 color);
    // A3: ground lobe + blend strength [0,1]. Strength 0 disables hemi (flat ambient only).
    void setHemisphereAmbient(glm::vec3 groundColor, float strength);
    void setDirectionalLight(glm::vec3 dir, glm::vec3 color);
    void submitPointLight(glm::vec3 pos, glm::vec3 color, float radius);
    void submitSpotLight(glm::vec3 pos, glm::vec3 dir, glm::vec3 color, float radius, float angle);
    void drawCrosshair();
    void endFrame();

    PsxOptions psx;

private:
    // Upload already-decoded RGBA8 pixels (takes ownership; frees via stbi). Shared by the
    // file and in-memory texture decode paths.
    TextureHandle uploadRgba(unsigned char* pixels, int width, int height,
                             const std::string& label);
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
        glm::vec4 ambientColor; // rgb = sky/flat ambient; w = hemi strength [0,1]
        glm::vec4 hemiGround;   // rgb = ground lobe (A3); w unused
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
        MaterialDesc material; // resolved at submit time (copy, handles stay light)
    };
    struct SkinnedDraw {
        SkinnedMeshHandle mesh;
        glm::mat4 transform;
        MaterialDesc material;
        std::vector<glm::mat4> bones; // ≤ kMaxBones skinning matrices, copied at submit
    };
    struct SpriteDraw {
        glm::vec3 center;
        glm::vec2 size;
        TextureHandle tex;
        glm::vec4 uvRect;
        glm::vec3 tint;
        bool fullbright;
    };

    void ensurePsxTarget(glm::ivec2 framebufferSize);
    void ensureShadowMap();
    void renderShadowMap();
    void bindShadowUniforms(const GlShaderProgram& prog) const;
    void drawSkyPass();
    void flushScenePasses();
    void drawCrosshairPass(glm::ivec2 targetSize);
    void resolveToBackbuffer();

    Window* m_window = nullptr;

    Shader m_chunkShader;
    Shader m_meshShader;
    Shader m_skinnedShader;
    Shader m_spriteShader;
    Shader m_resolveShader;
    Shader m_shadowShader; // A2 depth-only sun pass
    Shader m_skyShader;    // B3-sky gradient backdrop
    GlShaderProgram m_crosshairProgram; // trivial, source embedded in Renderer.cpp
    // Camera basis for sky (set in beginFrame).
    glm::vec3 m_camForward{0, 0, -1};
    glm::vec3 m_camRight{1, 0, 0};
    glm::vec3 m_camUp{0, 1, 0};
    float m_camFovY = glm::radians(70.0f);

    GlBuffer m_frameUbo;
    FrameUbo m_frame{};

    GlFramebuffer m_psxFbo;
    GlTexture m_psxColor;
    GlTexture m_psxDepth;
    glm::ivec2 m_psxSize{0, 0};
    glm::vec2 m_psxJitter{0.0f}; // vertex-snap grid for the chunk/mesh/skinned passes (0 = off)

    // A2 sun shadow map (depth-only FBO).
    GlFramebuffer m_shadowFbo;
    GlTexture m_shadowDepth;
    int m_shadowSize = 0;
    glm::mat4 m_lightVP{1.0f};

    GlVertexArray m_fullscreenVao; // empty; resolve.vert builds the triangle from gl_VertexID
    GlVertexArray m_spriteVao;     // empty; sprite.vert builds the quad from gl_VertexID
    GlVertexArray m_crosshairVao;
    GlBuffer m_crosshairVbo;

    std::unordered_map<MeshHandle, GpuMesh> m_meshes;
    std::unordered_map<SkinnedMeshHandle, GpuMesh> m_skinnedMeshes;
    std::unordered_map<TextureHandle, GlTexture> m_textures;
    std::unordered_map<MaterialHandle, MaterialDesc> m_materials;
    MeshHandle m_nextMesh = 1;
    SkinnedMeshHandle m_nextSkinnedMesh = 1;
    TextureHandle m_nextTexture = 1;
    std::uint32_t m_nextMaterial = 1;
    TextureHandle m_atlas = 0;

    std::vector<ChunkDraw> m_chunkDraws;
    std::vector<MeshDraw> m_meshDraws;
    std::vector<SkinnedDraw> m_skinnedDraws;
    std::vector<SpriteDraw> m_spriteDraws;
    int m_pointCount = 0;
    int m_spotCount = 0;
    bool m_crosshairRequested = false;

    glm::ivec2 m_fbSize{0, 0};
    bool m_usePsxTarget = false;
};

} // namespace meat
