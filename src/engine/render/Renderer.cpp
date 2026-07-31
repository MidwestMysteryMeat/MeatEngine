#include "engine/render/Renderer.h"

#include "engine/core/Log.h"
#include "engine/platform/Window.h"

// glad must precede GLFW; Renderer.h pulls <glad/gl.h> in via GlObjects.h.
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace meat {
namespace {

// Guard the C++ mirror against drifting from the std140 block in the shaders.
static_assert(sizeof(glm::vec4) == 16 && sizeof(glm::mat4) == 64);

constexpr GLuint kFrameUboBinding = 0;
constexpr const char* kShaderDir = "assets/shaders";

// Crosshair is two fixed lines; not worth disk files or hot reload.
constexpr const char* kCrosshairVert = R"(#version 450 core
layout(location = 0) in vec2 aOffsetPx;
uniform vec2 uViewport;
void main() { gl_Position = vec4(2.0 * aOffsetPx / uViewport, 0.0, 1.0); }
)";
constexpr const char* kCrosshairFrag = R"(#version 450 core
out vec4 oColor;
void main() { oColor = vec4(1.0); }
)";

} // namespace

bool Renderer::init(Window& window) {
    // 1664 = previous 1648 + one vec4 (hemiGround) after ambientColor (A3).
    // every GLSL FrameData copy must gain the same member in the same slot.
    static_assert(sizeof(FrameUbo) == 1664, "FrameUbo must match the std140 FrameData block");
    static_assert(sizeof(GpuPointLight) == 32 && sizeof(GpuSpotLight) == 48);

    m_window = &window;
    (void)window.handle(); // context creation/current-ness is Window's job

    if (gladLoadGL(glfwGetProcAddress) == 0) {
        log::error("renderer: gladLoadGL failed — no GL 4.5 context?");
        return false;
    }
    log::info("renderer: GL {} on {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    glEnable(GL_DEPTH_TEST);
    // Face culling stays off until the mesher's winding is locked down;
    // flipping it on is a one-line perf win later, not a correctness need.

    m_frameUbo.create();
    glNamedBufferStorage(m_frameUbo.id(), sizeof(FrameUbo), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, kFrameUboBinding, m_frameUbo.id());

    if (!m_chunkShader.load(kShaderDir, "chunk") || !m_meshShader.load(kShaderDir, "mesh") ||
        !m_skinnedShader.load(kShaderDir, "skinned") ||
        !m_spriteShader.load(kShaderDir, "sprite") || !m_resolveShader.load(kShaderDir, "resolve") ||
        !m_shadowShader.load(kShaderDir, "shadow") ||
        !m_shadowSkinnedShader.load(kShaderDir, "shadow_skinned") ||
        !m_skyShader.load(kShaderDir, "sky") || !m_waterShader.load(kShaderDir, "water")) {
        return false;
    }
    if (!m_crosshairProgram.compile(kCrosshairVert, kCrosshairFrag, "crosshair")) {
        return false;
    }

    m_fullscreenVao.create(); // attribute-less; resolve.vert synthesizes the triangle
    m_spriteVao.create();     // attribute-less; sprite.vert synthesizes the quad

    // Crosshair: two lines crossing at screen center, offsets in target pixels.
    const glm::vec2 crosshair[4] = {{-6.0f, 0.0f}, {6.0f, 0.0f}, {0.0f, -6.0f}, {0.0f, 6.0f}};
    m_crosshairVbo.create();
    glNamedBufferStorage(m_crosshairVbo.id(), sizeof(crosshair), crosshair, 0);
    m_crosshairVao.create();
    glVertexArrayVertexBuffer(m_crosshairVao.id(), 0, m_crosshairVbo.id(), 0, sizeof(glm::vec2));
    glEnableVertexArrayAttrib(m_crosshairVao.id(), 0);
    glVertexArrayAttribFormat(m_crosshairVao.id(), 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_crosshairVao.id(), 0, 0);

    // Sane default light so a scene renders before gameplay sets one.
    m_frame.dirLightDir = glm::vec4(glm::normalize(glm::vec3(-0.35f, -0.85f, -0.40f)), 0.0f);
    m_frame.dirLightColor = glm::vec4(1.0f, 0.97f, 0.92f, 0.0f);
    setAmbientLight(glm::vec3(0.25f, 0.27f, 0.32f));
    return true;
}

void Renderer::reloadShaders() {
    // Each Shader keeps its previous program if the recompile fails.
    m_chunkShader.reload();
    m_meshShader.reload();
    m_skinnedShader.reload();
    m_spriteShader.reload();
    m_resolveShader.reload();
    m_shadowShader.reload();
    m_shadowSkinnedShader.reload();
    m_skyShader.reload();
    m_waterShader.reload();
}

bool Renderer::captureScreenshot(const std::filesystem::path& path) {
    const glm::ivec2 fb = m_window ? m_window->framebufferSize() : glm::ivec2(0);
    if (fb.x <= 0 || fb.y <= 0) return false;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(fb.x) * fb.y * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, fb.x, fb.y, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1); // GL origin is bottom-left; PNG is top-left
    const bool ok = stbi_write_png(path.string().c_str(), fb.x, fb.y, 3, pixels.data(),
                                   fb.x * 3) != 0;
    if (ok)
        log::info("screenshot → {}", path.string());
    else
        log::error("screenshot write failed: {}", path.string());
    return ok;
}

void Renderer::ensurePsxTarget(glm::ivec2 framebufferSize) {
    const glm::ivec2 want =
        glm::max(glm::ivec2(1, 1),
                 glm::ivec2(glm::vec2(framebufferSize) * std::max(psx.internalScale, 0.05f)));
    if (want == m_psxSize && m_psxFbo) {
        return;
    }
    m_psxSize = want;

    m_psxColor.create(GL_TEXTURE_2D);
    glTextureStorage2D(m_psxColor.id(), 1, GL_RGBA8, m_psxSize.x, m_psxSize.y);
    glTextureParameteri(m_psxColor.id(), GL_TEXTURE_MIN_FILTER, GL_NEAREST); // nearest upscale
    glTextureParameteri(m_psxColor.id(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_psxColor.id(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_psxColor.id(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_psxDepth.create(GL_TEXTURE_2D);
    glTextureStorage2D(m_psxDepth.id(), 1, GL_DEPTH_COMPONENT24, m_psxSize.x, m_psxSize.y);

    m_psxFbo.create();
    glNamedFramebufferTexture(m_psxFbo.id(), GL_COLOR_ATTACHMENT0, m_psxColor.id(), 0);
    glNamedFramebufferTexture(m_psxFbo.id(), GL_DEPTH_ATTACHMENT, m_psxDepth.id(), 0);
    if (glCheckNamedFramebufferStatus(m_psxFbo.id(), GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log::error("renderer: PSX target incomplete at {}x{}", m_psxSize.x, m_psxSize.y);
    } else {
        log::info("renderer: PSX target {}x{}", m_psxSize.x, m_psxSize.y);
    }
}

MeshHandle Renderer::uploadChunkMesh(const ChunkMeshData& data) {
    if (data.vertices.empty() || data.indices.empty()) {
        return 0;
    }
    GpuMesh mesh;
    mesh.vbo.create();
    glNamedBufferStorage(mesh.vbo.id(),
                         static_cast<GLsizeiptr>(data.vertices.size() * sizeof(VoxelVertex)),
                         data.vertices.data(), 0);
    mesh.ibo.create();
    glNamedBufferStorage(mesh.ibo.id(),
                         static_cast<GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
                         data.indices.data(), 0);
    mesh.vao.create();
    const GLuint vao = mesh.vao.id();
    glVertexArrayVertexBuffer(vao, 0, mesh.vbo.id(), 0, sizeof(VoxelVertex));
    glVertexArrayElementBuffer(vao, mesh.ibo.id());
    for (GLuint attrib = 0; attrib < 6; ++attrib) {
        glEnableVertexArrayAttrib(vao, attrib);
        glVertexArrayAttribBinding(vao, attrib, 0);
    }
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(offsetof(VoxelVertex, pos)));
    glVertexArrayAttribFormat(vao, 1, 3, GL_BYTE, GL_TRUE, // i8 -> normalized float
                              static_cast<GLuint>(offsetof(VoxelVertex, normal)));
    glVertexArrayAttribFormat(vao, 2, 2, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(offsetof(VoxelVertex, uv)));
    glVertexArrayAttribIFormat(vao, 3, 1, GL_UNSIGNED_SHORT, // integer attrib, no conversion
                               static_cast<GLuint>(offsetof(VoxelVertex, tex)));
    // Block-light 0..15 delivered as an unnormalized ubyte -> float; the shader
    // divides by 15 to a 0..1 brightness. Not GL_TRUE (that would map 255->1).
    glVertexArrayAttribFormat(vao, 4, 1, GL_UNSIGNED_BYTE, GL_FALSE,
                              static_cast<GLuint>(offsetof(VoxelVertex, light)));
    // Per-vertex ambient occlusion 0..3, unnormalized ubyte -> float; the shader
    // divides by 3 to a 0..1 openness factor. Like light, NOT GL_TRUE.
    glVertexArrayAttribFormat(vao, 5, 1, GL_UNSIGNED_BYTE, GL_FALSE,
                              static_cast<GLuint>(offsetof(VoxelVertex, ao)));
    mesh.indexCount = static_cast<GLsizei>(data.indices.size());

    const MeshHandle handle = m_nextMesh++;
    m_meshes.emplace(handle, std::move(mesh));
    return handle;
}

void Renderer::destroyMesh(MeshHandle mesh) {
    m_meshes.erase(mesh); // GlObjects RAII releases the GPU side
}

SkinnedMeshHandle Renderer::uploadSkinnedMesh(const std::vector<SkinnedVertex>& vertices,
                                              const std::vector<std::uint32_t>& indices) {
    if (vertices.empty() || indices.empty()) {
        return 0;
    }
    GpuMesh mesh;
    mesh.vbo.create();
    glNamedBufferStorage(mesh.vbo.id(),
                         static_cast<GLsizeiptr>(vertices.size() * sizeof(SkinnedVertex)),
                         vertices.data(), 0);
    mesh.ibo.create();
    glNamedBufferStorage(mesh.ibo.id(),
                         static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
                         indices.data(), 0);
    mesh.vao.create();
    const GLuint vao = mesh.vao.id();
    glVertexArrayVertexBuffer(vao, 0, mesh.vbo.id(), 0, sizeof(SkinnedVertex));
    glVertexArrayElementBuffer(vao, mesh.ibo.id());
    for (GLuint attrib = 0; attrib < 5; ++attrib) {
        glEnableVertexArrayAttrib(vao, attrib);
        glVertexArrayAttribBinding(vao, attrib, 0);
    }
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(offsetof(SkinnedVertex, pos)));
    glVertexArrayAttribFormat(vao, 1, 3, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(offsetof(SkinnedVertex, normal)));
    glVertexArrayAttribFormat(vao, 2, 2, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(offsetof(SkinnedVertex, uv)));
    glVertexArrayAttribIFormat(vao, 3, 4, GL_INT, // integer attrib: bone indices stay ints
                               static_cast<GLuint>(offsetof(SkinnedVertex, bones)));
    glVertexArrayAttribFormat(vao, 4, 4, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(offsetof(SkinnedVertex, weights)));
    mesh.indexCount = static_cast<GLsizei>(indices.size());

    const SkinnedMeshHandle handle = m_nextSkinnedMesh++;
    m_skinnedMeshes.emplace(handle, std::move(mesh));
    return handle;
}

void Renderer::destroySkinnedMesh(SkinnedMeshHandle mesh) {
    m_skinnedMeshes.erase(mesh);
}

// Upload already-decoded RGBA8 pixels to a GL texture and register a handle. Frees `pixels`
// (stbi allocation). Shared by the file and in-memory decode paths so the mip/filter/wrap
// setup lives in one place. Returns 0 on a null upload.
TextureHandle Renderer::uploadRgba(unsigned char* pixels, int width, int height,
                                   const std::string& label) {
    if (pixels == nullptr) return 0;
    const int levels =
        psx.nearestFiltering
            ? 1
            : 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(width, height)))));
    GlTexture tex;
    tex.create(GL_TEXTURE_2D);
    glTextureStorage2D(tex.id(), levels, GL_RGBA8, width, height);
    glTextureSubImage2D(tex.id(), 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    if (psx.nearestFiltering) {
        glTextureParameteri(tex.id(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(tex.id(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glGenerateTextureMipmap(tex.id());
        glTextureParameteri(tex.id(), GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(tex.id(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glTextureParameteri(tex.id(), GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(tex.id(), GL_TEXTURE_WRAP_T, GL_REPEAT);

    const TextureHandle handle = m_nextTexture++;
    m_textures.emplace(handle, std::move(tex));
    log::info("uploadTexture '{}' {}x{} ({} mips)", label, width, height, levels);
    return handle;
}

TextureHandle Renderer::loadTexture(const std::filesystem::path& path) {
    const std::string pathUtf8 = path.string();
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(1); // GL's uv origin is bottom-left
    stbi_uc* pixels = stbi_load(pathUtf8.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        log::error("loadTexture '{}': {}", pathUtf8, stbi_failure_reason());
        return 0;
    }
    return uploadRgba(pixels, width, height, pathUtf8);
}

// Decode a compressed image (PNG/JPG/TGA bytes, e.g. an FBX-embedded texture) from memory and
// upload it. Lets skinned characters carry their own texture inside the .fbx with no sidecar.
TextureHandle Renderer::loadTextureFromMemory(const unsigned char* data, std::size_t size,
                                              const std::string& label) {
    if (data == nullptr || size == 0) return 0;
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(1);
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height,
                                            &channels, 4);
    if (pixels == nullptr) {
        log::error("loadTextureFromMemory '{}': {}", label, stbi_failure_reason());
        return 0;
    }
    return uploadRgba(pixels, width, height, label);
}

void Renderer::setAtlas(TextureHandle atlas) {
    m_atlas = atlas;
}

MaterialHandle Renderer::createMaterial(const MaterialDesc& desc) {
    const MaterialHandle handle{m_nextMaterial++};
    m_materials.emplace(handle, desc);
    return handle;
}

void Renderer::beginFrame(const Camera& camera, float alpha) {
    (void)alpha; // callers pass interpolated transforms; kept for contract parity

    m_fbSize = m_window->framebufferSize();
    m_fbSize = glm::max(m_fbSize, glm::ivec2(1, 1));
    m_usePsxTarget = psx.internalScale != 1.0f;

    if (m_usePsxTarget) {
        ensurePsxTarget(m_fbSize);
        glBindFramebuffer(GL_FRAMEBUFFER, m_psxFbo.id());
        glViewport(0, 0, m_psxSize.x, m_psxSize.y);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_fbSize.x, m_fbSize.y);
    }
    // PSX vertex-snap grid (0 disables): the low-res target resolution (now current), so verts
    // quantize to its pixel grid. Consumed by the chunk/mesh/skinned draw passes.
    m_psxJitter = (m_usePsxTarget && psx.vertexJitter) ? glm::vec2(m_psxSize) : glm::vec2(0.0f);
    glClearColor(psx.fogColor.r, psx.fogColor.g, psx.fogColor.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = static_cast<float>(m_fbSize.x) / static_cast<float>(m_fbSize.y);
    m_frame.view = camera.view();
    m_frame.proj = camera.proj(aspect);
    m_frame.camPos = glm::vec4(camera.pos, 1.0f);
    m_frame.fogParams = glm::vec4(psx.fogStart, psx.fogEnd, psx.fog ? 1.0f : 0.0f, 0.0f);
    m_frame.fogColor = glm::vec4(psx.fogColor, 1.0f);
    // B3-sky camera basis.
    m_camForward = camera.forward();
    m_camRight = glm::normalize(glm::cross(m_camForward, glm::vec3(0.0f, 1.0f, 0.0f)));
    if (glm::length(m_camRight) < 1e-4f) m_camRight = glm::vec3(1.0f, 0.0f, 0.0f);
    m_camUp = glm::normalize(glm::cross(m_camRight, m_camForward));
    m_camFovY = camera.fovY;
    m_pointCount = 0;
    m_spotCount = 0;
    m_frame.lightCounts = glm::ivec4(0);
    glNamedBufferSubData(m_frameUbo.id(), 0, sizeof(FrameUbo), &m_frame);

    m_chunkDraws.clear();
    m_meshDraws.clear();
    m_skinnedDraws.clear();
    m_spriteDraws.clear();
    m_crosshairRequested = false;
}

void Renderer::submitChunk(MeshHandle mesh, glm::vec3 originWorld) {
    if (mesh != 0) {
        m_chunkDraws.push_back({mesh, originWorld});
    }
}

void Renderer::submitMesh(MeshHandle mesh, const glm::mat4& transform, TextureHandle albedo) {
    // Bare-texture path: implicit default material (tint 1, shininess 32, no emissive).
    MaterialDesc desc;
    desc.albedo = albedo;
    if (mesh != 0) {
        m_meshDraws.push_back({mesh, transform, desc});
    }
}

void Renderer::submitMesh(MeshHandle mesh, const glm::mat4& transform, MaterialHandle material) {
    const auto it = m_materials.find(material);
    if (mesh == 0 || it == m_materials.end()) {
        return;
    }
    m_meshDraws.push_back({mesh, transform, it->second});
}

void Renderer::submitSkinned(SkinnedMeshHandle mesh, const glm::mat4& transform, const Pose& pose,
                             MaterialHandle material) {
    const auto it = m_materials.find(material);
    if (mesh == 0 || it == m_materials.end() || pose.skinningMatrices.empty()) {
        return;
    }
    if (pose.skinningMatrices.size() > static_cast<std::size_t>(kMaxBones)) {
        static bool warned = false;
        if (!warned) {
            log::warn("renderer: pose has {} bones, uBones holds {} — truncating",
                      pose.skinningMatrices.size(), kMaxBones);
            warned = true;
        }
    }
    const std::size_t count =
        std::min(pose.skinningMatrices.size(), static_cast<std::size_t>(kMaxBones));
    SkinnedDraw draw{mesh, transform, it->second,
                     {pose.skinningMatrices.begin(),
                      pose.skinningMatrices.begin() + static_cast<std::ptrdiff_t>(count)}};
    m_skinnedDraws.push_back(std::move(draw));
}

void Renderer::submitSprite(glm::vec3 center, glm::vec2 size, TextureHandle tex, glm::vec4 uvRect,
                            glm::vec3 tint, bool fullbright) {
    if (tex != 0) {
        m_spriteDraws.push_back({center, size, tex, uvRect, tint, fullbright});
    }
}

void Renderer::setAmbientLight(glm::vec3 color) {
    // Keep existing hemi strength in .w when only the colour changes.
    m_frame.ambientColor = glm::vec4(color, m_frame.ambientColor.w);
}

void Renderer::setHemisphereAmbient(glm::vec3 groundColor, float strength) {
    m_frame.hemiGround = glm::vec4(groundColor, 0.0f);
    m_frame.ambientColor.w = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
}

void Renderer::setDirectionalLight(glm::vec3 dir, glm::vec3 color) {
    m_frame.dirLightDir = glm::vec4(glm::normalize(dir), 0.0f);
    m_frame.dirLightColor = glm::vec4(color, 0.0f);
}

void Renderer::submitPointLight(glm::vec3 pos, glm::vec3 color, float radius) {
    if (m_pointCount >= kMaxPointLights) {
        return; // extras beyond the UBO budget are dropped
    }
    m_frame.pointLights[m_pointCount].posRadius = glm::vec4(pos, radius);
    m_frame.pointLights[m_pointCount].color = glm::vec4(color, 0.0f);
    ++m_pointCount;
}

void Renderer::submitSpotLight(glm::vec3 pos, glm::vec3 dir, glm::vec3 color, float radius,
                               float angle) {
    if (m_spotCount >= kMaxSpotLights) {
        return;
    }
    m_frame.spotLights[m_spotCount].posRadius = glm::vec4(pos, radius);
    // angle = half-angle of the cone in radians; shader compares against its cosine
    m_frame.spotLights[m_spotCount].dirCosAngle =
        glm::vec4(glm::normalize(dir), std::cos(angle));
    m_frame.spotLights[m_spotCount].color = glm::vec4(color, 0.0f);
    ++m_spotCount;
}

void Renderer::drawCrosshair() {
    m_crosshairRequested = true; // drawn on top of the scene passes in endFrame
}

void Renderer::endFrame() {
    if (m_window == nullptr) {
        return;
    }
    // Light lists are complete now; push the final UBO before the scene passes.
    m_frame.lightCounts = glm::ivec4(m_pointCount, m_spotCount, 0, 0);
    glNamedBufferSubData(m_frameUbo.id(), 0, sizeof(FrameUbo), &m_frame);

    // A2: depth pass from the sun before the lit scene so chunks cast hard shadows.
    if (psx.sunShadows) renderShadowMap();
    else m_lightVP = glm::mat4(1.0f);

    // Restore the colour target the scene draws into (shadow pass rebound the FBO).
    if (m_usePsxTarget) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_psxFbo.id());
        glViewport(0, 0, m_psxSize.x, m_psxSize.y);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_fbSize.x, m_fbSize.y);
    }

    flushScenePasses();
    if (m_crosshairRequested) {
        drawCrosshairPass(m_usePsxTarget ? m_psxSize : m_fbSize);
    }
    if (m_usePsxTarget) {
        resolveToBackbuffer();
    }
}

void Renderer::ensureShadowMap() {
    const int want = std::clamp(psx.shadowMapSize, 256, 4096);
    if (m_shadowSize == want && m_shadowFbo && m_shadowDepth) return;
    m_shadowSize = want;
    m_shadowDepth.create(GL_TEXTURE_2D);
    glTextureStorage2D(m_shadowDepth.id(), 1, GL_DEPTH_COMPONENT24, want, want);
    glTextureParameteri(m_shadowDepth.id(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_shadowDepth.id(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_shadowDepth.id(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(m_shadowDepth.id(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTextureParameterfv(m_shadowDepth.id(), GL_TEXTURE_BORDER_COLOR, border);

    m_shadowFbo.create();
    glNamedFramebufferTexture(m_shadowFbo.id(), GL_DEPTH_ATTACHMENT, m_shadowDepth.id(), 0);
    glNamedFramebufferDrawBuffer(m_shadowFbo.id(), GL_NONE);
    glNamedFramebufferReadBuffer(m_shadowFbo.id(), GL_NONE);
    const GLenum status = glCheckNamedFramebufferStatus(m_shadowFbo.id(), GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        log::error("renderer: shadow FBO incomplete (0x{:x})", static_cast<unsigned>(status));
    else
        log::info("renderer: A2 sun shadow map {}x{}", want, want);
}

void Renderer::renderShadowMap() {
    ensureShadowMap();
    const glm::vec3 lightDir = glm::normalize(glm::vec3(m_frame.dirLightDir));
    const glm::vec3 center = glm::vec3(m_frame.camPos);
    // Light travels along lightDir; place the virtual sun opposite and look at the camera.
    const float pull = psx.shadowExtent * 2.5f;
    glm::vec3 lightPos = center - lightDir * pull;
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(lightDir, up)) > 0.95f) up = glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::mat4 lightView = glm::lookAt(lightPos, center, up);
    const float half = std::max(8.0f, psx.shadowExtent);
    const glm::mat4 lightProj =
        glm::ortho(-half, half, -half, half, 1.0f, pull + half * 2.0f);
    m_lightVP = lightProj * lightView;

    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo.id());
    glViewport(0, 0, m_shadowSize, m_shadowSize);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    // Front-face cull reduces shadow acne on voxel faces.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);

    glUseProgram(m_shadowShader.id());
    const GlShaderProgram& sh = m_shadowShader.program();
    sh.setUniform("uLightVP", m_lightVP);

    for (const ChunkDraw& draw : m_chunkDraws) {
        const auto meshIt = m_meshes.find(draw.mesh);
        if (meshIt == m_meshes.end()) continue;
        sh.setUniform("uModel", glm::translate(glm::mat4(1.0f), draw.origin));
        glBindVertexArray(meshIt->second.vao.id());
        glDrawElements(GL_TRIANGLES, meshIt->second.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    for (const MeshDraw& draw : m_meshDraws) {
        const auto meshIt = m_meshes.find(draw.mesh);
        if (meshIt == m_meshes.end()) continue;
        sh.setUniform("uModel", draw.transform);
        glBindVertexArray(meshIt->second.vao.id());
        glDrawElements(GL_TRIANGLES, meshIt->second.indexCount, GL_UNSIGNED_INT, nullptr);
    }

    // A2-s: skinned casters — same bone palette path as the lit skinned pass.
    if (!m_skinnedDraws.empty()) {
        glUseProgram(m_shadowSkinnedShader.id());
        const GlShaderProgram& skSh = m_shadowSkinnedShader.program();
        skSh.setUniform("uLightVP", m_lightVP);
        const GLint bonesLoc = glGetUniformLocation(m_shadowSkinnedShader.id(), "uBones");
        for (const SkinnedDraw& draw : m_skinnedDraws) {
            const auto meshIt = m_skinnedMeshes.find(draw.mesh);
            if (meshIt == m_skinnedMeshes.end() || draw.bones.empty()) continue;
            skSh.setUniform("uModel", draw.transform);
            glProgramUniformMatrix4fv(m_shadowSkinnedShader.id(), bonesLoc,
                                      static_cast<GLsizei>(draw.bones.size()), GL_FALSE,
                                      glm::value_ptr(draw.bones[0]));
            glBindVertexArray(meshIt->second.vao.id());
            glDrawElements(GL_TRIANGLES, meshIt->second.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(0);
}

void Renderer::bindShadowUniforms(const GlShaderProgram& prog) const {
    prog.setUniform("uLightVP", m_lightVP);
    prog.setUniform("uShadows", (psx.sunShadows && m_shadowDepth) ? 1 : 0);
    if (m_shadowDepth) glBindTextureUnit(1, m_shadowDepth.id());
}

void Renderer::drawSkyPass() {
    if (!psx.sky) return;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(m_skyShader.id());
    const GlShaderProgram& sky = m_skyShader.program();
    const float aspect = static_cast<float>(m_fbSize.x) / static_cast<float>(std::max(1, m_fbSize.y));
    sky.setUniform("uCamForward", m_camForward);
    sky.setUniform("uCamRight", m_camRight);
    sky.setUniform("uCamUp", m_camUp);
    sky.setUniform("uTanHalfFov", std::tan(m_camFovY * 0.5f));
    sky.setUniform("uAspect", aspect);
    sky.setUniform("uZenith", psx.skyZenith);
    sky.setUniform("uHorizon", psx.skyHorizon);
    sky.setUniform("uGround", psx.skyGround);
    sky.setUniform("uStars", psx.skyStars ? 1 : 0);
    glBindVertexArray(m_fullscreenVao.id());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::drawWaterPass() {
    if (!psx.waterPlane || psx.waterExtent <= 0.0f || psx.waterAlpha <= 0.0f) return;
    // After opaque geometry: depth-test against terrain, no depth write, alpha blend.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE); // visible from above and below the surface
    glUseProgram(m_waterShader.id());
    const GlShaderProgram& water = m_waterShader.program();
    water.setUniform("uWaterY", psx.waterY);
    water.setUniform("uExtent", psx.waterExtent);
    water.setUniform("uTime", static_cast<float>(glfwGetTime()));
    water.setUniform("uWaterColor", psx.waterColor);
    water.setUniform("uWaterAlpha", psx.waterAlpha);
    // Reuse attribute-less sprite VAO (triangle strip corners from gl_VertexID).
    glBindVertexArray(m_spriteVao.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void Renderer::flushScenePasses() {
    // B3-sky first (depth off) so fog/clear still tint empty space under geometry.
    drawSkyPass();

    // Chunk pass: one shader, one atlas, per-draw translation.
    if (!m_chunkDraws.empty()) {
        const auto atlasIt = m_textures.find(m_atlas);
        if (atlasIt == m_textures.end()) {
            static bool warned = false;
            if (!warned) {
                log::warn("renderer: chunk draws submitted with no atlas set — skipping");
                warned = true;
            }
        } else {
            glUseProgram(m_chunkShader.id());
            m_chunkShader.program().setUniform("uPsxJitter", m_psxJitter);
            bindShadowUniforms(m_chunkShader.program());
            glBindTextureUnit(0, atlasIt->second.id());
            for (const ChunkDraw& draw : m_chunkDraws) {
                const auto meshIt = m_meshes.find(draw.mesh);
                if (meshIt == m_meshes.end()) {
                    continue;
                }
                m_chunkShader.program().setUniform(
                    "uModel", glm::translate(glm::mat4(1.0f), draw.origin));
                glBindVertexArray(meshIt->second.vao.id());
                glDrawElements(GL_TRIANGLES, meshIt->second.indexCount, GL_UNSIGNED_INT, nullptr);
            }
        }
    }

    // Mesh pass: per-draw transform + material params as plain uniforms.
    if (!m_meshDraws.empty()) {
        glUseProgram(m_meshShader.id());
        const GlShaderProgram& meshProg = m_meshShader.program();
        meshProg.setUniform("uPsxJitter", m_psxJitter);
        bindShadowUniforms(meshProg);
        for (const MeshDraw& draw : m_meshDraws) {
            const auto meshIt = m_meshes.find(draw.mesh);
            const auto texIt = m_textures.find(draw.material.albedo);
            if (meshIt == m_meshes.end() || texIt == m_textures.end()) {
                continue;
            }
            meshProg.setUniform("uModel", draw.transform);
            meshProg.setUniform("uTint", draw.material.tint);
            meshProg.setUniform("uShininess", draw.material.shininess);
            meshProg.setUniform("uEmissive", draw.material.emissive);
            glBindTextureUnit(0, texIt->second.id());
            glBindVertexArray(meshIt->second.vao.id());
            glDrawElements(GL_TRIANGLES, meshIt->second.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // Skinned pass: opaque like the mesh pass, depth on, inside the PSX target.
    // Bone palette rides a plain uniform array (uBones[128]) uploaded per draw
    // via glProgramUniformMatrix4fv — a handful of actors, not worth UBO ranges.
    if (!m_skinnedDraws.empty()) {
        glUseProgram(m_skinnedShader.id());
        const GlShaderProgram& skinnedProg = m_skinnedShader.program();
        skinnedProg.setUniform("uPsxJitter", m_psxJitter);
        bindShadowUniforms(skinnedProg);
        const GLint bonesLoc = glGetUniformLocation(m_skinnedShader.id(), "uBones");
        for (const SkinnedDraw& draw : m_skinnedDraws) {
            const auto meshIt = m_skinnedMeshes.find(draw.mesh);
            const auto texIt = m_textures.find(draw.material.albedo);
            if (meshIt == m_skinnedMeshes.end() || texIt == m_textures.end()) {
                continue;
            }
            skinnedProg.setUniform("uModel", draw.transform);
            skinnedProg.setUniform("uTint", draw.material.tint);
            skinnedProg.setUniform("uShininess", draw.material.shininess);
            skinnedProg.setUniform("uEmissive", draw.material.emissive);
            glProgramUniformMatrix4fv(m_skinnedShader.id(), bonesLoc,
                                      static_cast<GLsizei>(draw.bones.size()), GL_FALSE,
                                      glm::value_ptr(draw.bones[0]));
            glBindTextureUnit(0, texIt->second.id());
            glBindVertexArray(meshIt->second.vao.id());
            glDrawElements(GL_TRIANGLES, meshIt->second.indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    // B3 water plane: after opaque, before sprites (markers/UI-ish stay on top).
    drawWaterPass();

    // Sprite pass: after opaque geometry, still inside the PSX target so sprites
    // dither and fog like everything else. Alpha-test (discard < 0.5) with depth
    // writes ON instead of blend+sort — hard PSX cutouts make order irrelevant.
    if (!m_spriteDraws.empty()) {
        std::sort(m_spriteDraws.begin(), m_spriteDraws.end(),
                  [](const SpriteDraw& a, const SpriteDraw& b) { return a.tex < b.tex; });
        glUseProgram(m_spriteShader.id());
        const GlShaderProgram& spriteProg = m_spriteShader.program();
        glBindVertexArray(m_spriteVao.id());
        TextureHandle bound = 0;
        for (const SpriteDraw& draw : m_spriteDraws) {
            const auto texIt = m_textures.find(draw.tex);
            if (texIt == m_textures.end()) {
                continue;
            }
            if (draw.tex != bound) {
                glBindTextureUnit(0, texIt->second.id());
                bound = draw.tex;
            }
            spriteProg.setUniform("uCenter", draw.center);
            spriteProg.setUniform("uSize", draw.size);
            spriteProg.setUniform("uUvRect", draw.uvRect);
            spriteProg.setUniform("uTint", draw.tint);
            spriteProg.setUniform("uFullbright", draw.fullbright ? 1 : 0);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
    glBindVertexArray(0);
}

void Renderer::drawCrosshairPass(glm::ivec2 targetSize) {
    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_crosshairProgram.id());
    m_crosshairProgram.setUniform("uViewport", glm::vec2(targetSize));
    glBindVertexArray(m_crosshairVao.id());
    glDrawArrays(GL_LINES, 0, 4);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::resolveToBackbuffer() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_fbSize.x, m_fbSize.y);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_resolveShader.id());
    glBindTextureUnit(0, m_psxColor.id()); // sampler is NEAREST — the chunky upscale
    m_resolveShader.program().setUniform("uDither", psx.dither ? 1 : 0);
    m_resolveShader.program().setUniform("uSourceSize", glm::vec2(m_psxSize));
    glBindVertexArray(m_fullscreenVao.id());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

} // namespace meat
