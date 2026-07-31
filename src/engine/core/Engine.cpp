#include "engine/core/Engine.h"
#include "engine/core/Log.h"
#include "engine/core/ViewMath.h"
#include "engine/anim/Animator.h"
#include "engine/asset/ModelLoader.h"
#include "engine/net/HttpTiny.h"
#include "game/Environment.h"

#include <GLFW/glfw3.h>
#include <stb_image.h> // stbi_info: cheap header-probe validation for texture imports
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp> // value_ptr/make_mat4: serialize editor-prop transforms
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <ImGuizmo.h>
#include <nlohmann/json.hpp>

#include <fstream>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
#include <thread>

namespace meat {
namespace {
constexpr float kFallResetY = -30.0f; // below any terrain: teleport back to spawn
// Same voxel cell as historical (8,8,8)@0.5 m — scales with kVoxelSize so a
// large-block host never spawns the client under the surface pad.
inline glm::vec3 clientSpawnPos() {
    return glm::vec3(16.0f, 16.0f, 16.0f) * kVoxelSize;
}
constexpr float kFireInterval = 0.15f; // cosmetic mirror; the server enforces its own

// Box proxy meshes (feet/base at local origin) rendered through the chunk
// pipeline: atlas tile per face, so no separate shader needed.
ChunkMeshData makeBoxMesh(float hw, float h, std::uint16_t tile) {
    ChunkMeshData data;
    const glm::vec3 lo{-hw, 0.0f, -hw}, hi{hw, h, hw};
    const struct {
        glm::i8vec3 n;
        glm::vec3 corners[4]; // CCW from outside
    } faces[] = {
        {{1, 0, 0}, {{hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}}},
        {{-1, 0, 0}, {{lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}}},
        {{0, 1, 0}, {{lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}}},
        {{0, -1, 0}, {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}}},
        {{0, 0, 1}, {{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}}},
        {{0, 0, -1}, {{hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}}},
    };
    const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const auto& f : faces) {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        for (int i = 0; i < 4; ++i) data.vertices.push_back({f.corners[i], f.n, uv[i], tile});
        for (std::uint32_t idx : {0u, 1u, 2u, 0u, 2u, 3u}) data.indices.push_back(base + idx);
    }
    return data;
}
} // namespace

bool Engine::initClientSystems() {
    if (!m_window.init({})) return false;
    m_input.attach(m_window);
    m_window.setRelativeMouse(true);
    if (!m_renderer.init(m_window)) return false;
    if (!m_physics.init()) return false;
    m_audio.init(); // best-effort: a failed init just runs silent
    m_jobs.start(std::thread::hardware_concurrency());

    m_voxels.setMeshReadyCallback([this](ChunkPos pos, ChunkMeshData data) {
        if (auto it = m_chunkMeshes.find(pos); it != m_chunkMeshes.end()) {
            m_renderer.destroyMesh(it->second);
            m_chunkMeshes.erase(it);
        }
        if (!data.indices.empty()) {
            m_chunkMeshes[pos] = m_renderer.uploadChunkMesh(data);
            m_physics.syncChunkCollider(pos, data);
        } else {
            m_physics.removeChunkCollider(pos);
        }
    });
    m_voxels.setChunkUnloadedCallback([this](ChunkPos pos) {
        if (auto it = m_chunkMeshes.find(pos); it != m_chunkMeshes.end()) {
            m_renderer.destroyMesh(it->second);
            m_chunkMeshes.erase(it);
        }
        m_physics.removeChunkCollider(pos);
    });

    const TextureHandle atlas = m_renderer.loadTexture("assets/textures/atlas.png");
    if (atlas == 0) return false;
    m_renderer.setAtlas(atlas);
    m_atlasTexture = atlas;
    m_renderer.setDirectionalLight(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)),
                                   glm::vec3(1.0f, 0.96f, 0.88f));
    m_remotePlayerMesh = m_renderer.uploadChunkMesh(makeBoxMesh(0.35f, 1.8f, 2));
    m_pickupMesh = m_renderer.uploadChunkMesh(makeBoxMesh(0.15f, 0.3f, 3));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    // After Input::attach: ImGui chains and forwards the existing callbacks.
    ImGui_ImplGlfw_InitForOpenGL(m_window.handle(), true);
    ImGui_ImplOpenGL3_Init("#version 450");
    m_imguiReady = true;
    return true;
}

bool Engine::initNetwork(const EngineConfig& config) {
    using Mode = EngineConfig::Mode;
    // A game project supplies rules + a scripts dir; game.json is parsed in main().
    const auto bootServer = [&](std::unique_ptr<ServerSim>& server) {
        server = std::make_unique<ServerSim>(config.rules);
        if (!config.projectDir.empty())
            server->setScriptDir(config.projectDir + "/scripts");
        return config.loadPath.empty() ? server->init(config.seed)
                                       : server->initFromSave(config.loadPath);
    };
    if (config.mode == Mode::Game) {
        m_loopback = std::make_unique<LoopbackPair>();
        if (!bootServer(m_server)) return false;
        m_serverTransport = &m_loopback->serverEnd();
        m_clientTransport = &m_loopback->clientEnd();
    } else if (config.mode == Mode::Host) {
        // The host's own client joins over real UDP to localhost: one code
        // path for every player, and the net layer gets exercised constantly.
        m_enetHost = std::make_unique<EnetServerTransport>();
        if (!m_enetHost->listen(config.port)) return false;
        if (!bootServer(m_server)) return false;
        m_serverTransport = m_enetHost.get();
        m_enetJoin = std::make_unique<EnetClientTransport>();
        if (!m_enetJoin->connect("127.0.0.1", config.port)) return false;
        m_clientTransport = m_enetJoin.get();
    } else if (config.mode == Mode::Join) {
        m_enetJoin = std::make_unique<EnetClientTransport>();
        if (!m_enetJoin->connect(config.address, config.port)) return false;
        m_clientTransport = m_enetJoin.get();
    }
    m_client.attach(*m_clientTransport, "player");
    return true;
}

void Engine::applyEnvironment(const GameRules& rules) {
    const EnvSettings env = envSettings(rules.environment);
    // Client mirror physics + the local prediction body match the server's authoritative gravity so
    // predicted falls line up with reconciled ones. setGravity on the controller only writes tuning,
    // so it holds through the later m_player.init() in setupClientWorld().
    m_physics.setGravity(env.gravity);
    m_player.setGravity(env.gravity);
    // Fog + ambient are purely visual (client-only): the void of space, the murk of deep water.
    m_renderer.psx.fogColor = env.fogColor;
    m_renderer.psx.fogStart = env.fogStart;
    m_renderer.psx.fogEnd = env.fogEnd;
    m_renderer.setAmbientLight(env.ambient);
    // A3: hemisphere ambient — form-defining fill not gated by torch light. Toggleable so the
    // dark PSX-night look stays available (game.json / F7 / New Map "hemisphere ambient").
    const float hemi = rules.hemisphereAmbient ? env.hemiStrength : 0.0f;
    m_renderer.setHemisphereAmbient(env.hemiGround, hemi);
    m_hemisphereAmbient = rules.hemisphereAmbient;
}

void Engine::setupClientWorld() {
    const BlockPalette palette = registerDefaultBlocks(m_voxels.blockRegistry());
    registerDefaultItems(m_items, palette.stone); // ids mirror the server's registry
    // Terrain mode came from the server in Welcome (packed in the rules flags), so the client
    // builds the identical world locally.
    m_voxels.setGenerator(makeTerrainGenerator(m_client.worldSeed(), palette, m_client.rules().terrain));
    if (!m_player.init(m_physics, clientSpawnPos())) {
        log::error("client character init failed");
        return;
    }
    m_prevPlayerPos = m_currPlayerPos = clientSpawnPos();
    m_clientWorldReady = true;
    loadWorldProps();
    loadAnimTestActor();
    loadNpcActor();
    log::info("client world ready (seed {})", m_client.worldSeed());
}

bool Engine::rebuildWorld(GameRules::Terrain terrain, GameRules::Environment environment,
                          std::uint32_t seed) {
    if (!m_server || !m_clientWorldReady) {
        log::warn("rebuildWorld: host/SP only, and only after the world is ready");
        return false;
    }

    // Drop client GPU chunk meshes + prop colliders before the server clears
    // world state (same unload contract as streaming).
    for (auto& [pos, mesh] : m_chunkMeshes) m_renderer.destroyMesh(mesh);
    m_chunkMeshes.clear();
    for (auto& [id, body] : m_propBodies) m_physics.removeStaticBox(body);
    m_propBodies.clear();
    m_editorProps.clear();
    m_entityPrevPos.clear();
    m_remoteAudio.clear();

    m_voxels.clearWorld();

    GameRules rules = m_server->rules();
    rules.terrain = terrain;
    rules.environment = environment;
    rules.hemisphereAmbient = m_hemisphereAmbient;
    m_server->reseedWorld(seed, terrain, environment);
    applyEnvironment(rules);

    // Client mirror generator must match the host's new seed/terrain.
    // Block ids already registered at setupClientWorld — do NOT re-register.
    const BlockPalette palette{/*stone*/ 1, /*dirt*/ 2, /*grass*/ 3, /*lamp*/ 4};
    m_voxels.setGenerator(makeTerrainGenerator(seed, palette, terrain));

    const glm::vec3 spawn = clientSpawnPos();
    m_player.setState(spawn, glm::vec3(0));
    m_player.setGravity(envSettings(environment).gravity);
    m_prevPlayerPos = m_currPlayerPos = spawn;
    m_editorCamera.pos = spawn + glm::vec3(0.0f, m_player.eyeHeight(), 0.0f);

    log::info("engine: New Map applied (seed {}, terrain {}, env {})", seed,
              static_cast<int>(terrain), static_cast<int>(environment));
    return true;
}

void Engine::loadWorldProps() {
    // Committed OBJ smoke-test prop, plus any locally-staged model (gitignored,
    // license-pending). Each is a static Assimp import → the existing mesh path.
    struct Candidate {
        const char* path;
        ModelImportOptions opts;
        glm::vec3 place;
    };
    const glm::vec3 near = clientSpawnPos() + glm::vec3(3.0f, 0.0f, 0.0f);
    const Candidate candidates[] = {
        {"assets/models/prop_crate.obj", {.scale = 1.0f, .center = true}, near},
        {"assets/models/test_male.fbx", {.scale = 0.01f, .center = true}, near + glm::vec3(2, 0, 0)},
    };
    for (const Candidate& c : candidates) {
        if (!std::filesystem::exists(c.path)) continue;
        const auto model = loadStaticModel(c.path, c.opts);
        if (!model) continue;
        MaterialDesc mat;
        mat.tint = glm::vec3(0.9f);
        if (!model->albedo.empty()) mat.albedo = m_renderer.loadTexture(model->albedo);
        m_props.push_back({m_renderer.uploadChunkMesh(model->mesh), m_renderer.createMaterial(mat),
                           glm::translate(glm::mat4(1.0f), c.place)});
    }
    log::info("loaded {} world props", m_props.size());
}

// Drain the server's prop reports (PropAddedMsg from a live place / join replay /
// move, RemovePropMsg) into the rendered+collidable synced list. An already-known
// id on PropAdded means the transform changed (gizmo move) — rebuild the collider.
void Engine::syncWorldProps() {
    for (const PropAddedMsg& msg : m_client.takeNewProps()) {
        auto existing = std::find_if(m_editorProps.begin(), m_editorProps.end(),
                                     [&](const EditorProp& p) { return p.id == msg.id; });
        const EditorPropMesh& pm = editorPropMesh(msg.asset);
        glm::vec3 center, half;
        transformedAabb(msg.transform, pm.boundsMin, pm.boundsMax, center, half);
        if (existing != m_editorProps.end()) {
            existing->transform = msg.transform;
            existing->assetPath = msg.asset;
            if (const auto bit = m_propBodies.find(msg.id); bit != m_propBodies.end()) {
                m_physics.removeStaticBox(bit->second);
                bit->second = m_physics.addStaticBox(center, half);
            } else {
                m_propBodies[msg.id] = m_physics.addStaticBox(center, half);
            }
            continue;
        }
        EditorProp prop;
        prop.id = msg.id;
        prop.assetPath = msg.asset;
        prop.transform = msg.transform;
        m_editorProps.push_back(prop);
        // Client-mirror box collider (same bounds+transform math as the server) so
        // local prediction stops at the prop where the authoritative sim does.
        m_propBodies[msg.id] = m_physics.addStaticBox(center, half);
        const glm::vec3 at = glm::vec3(msg.transform[3]);
        log::info("client: world prop {} '{}' synced at ({:.1f},{:.1f},{:.1f}) (+collider)", msg.id,
                  msg.asset, at.x, at.y, at.z);
    }
    for (const std::uint32_t id : m_client.takeRemovedProps()) {
        std::erase_if(m_editorProps, [id](const EditorProp& p) { return p.id == id; });
        if (const auto it = m_propBodies.find(id); it != m_propBodies.end()) {
            m_physics.removeStaticBox(it->second);
            m_propBodies.erase(it);
        }
    }
}

// Lazily load (and cache) the mesh + material for an editor-placed prop. Props
// are staged through the same static-Assimp path the world props use; loading
// with center=true drops the model's base to y=0 and recenters it in XZ, so an
// EditorProp's transform translation seats the base on the picked surface. A
// missing/failed asset caches a mesh==0 entry so we neither reload nor spam.
const Engine::EditorPropMesh& Engine::editorPropMesh(const std::string& assetPath) {
    auto it = m_editorPropCache.find(assetPath);
    if (it != m_editorPropCache.end()) return it->second;
    EditorPropMesh entry;
    if (std::filesystem::exists(assetPath)) {
        if (const auto model = loadStaticModel(assetPath, {.center = true})) {
            MaterialDesc mat;
            mat.tint = glm::vec3(0.9f);
            if (!model->albedo.empty()) mat.albedo = m_renderer.loadTexture(model->albedo);
            entry.mesh = m_renderer.uploadChunkMesh(model->mesh);
            entry.material = m_renderer.createMaterial(mat);
            entry.boundsMin = model->boundsMin; // same center=true bounds the server uses
            entry.boundsMax = model->boundsMax;
        } else {
            log::warn("editor prop '{}' failed to load", assetPath);
        }
    } else {
        log::warn("editor prop '{}' not found", assetPath);
    }
    return m_editorPropCache.emplace(assetPath, entry).first->second;
}

void Engine::loadAnimTestActor() {
    // Optional (gitignored) proof asset. Load at native scale (the trivially
    // correct path — no scale conjugation) and normalize display height via the
    // actor transform, so ANY model shows upright at ~1.8 m regardless of units.
    // --animmodel wins if given (booth-check any file); else the default proof asset.
    std::vector<std::string> paths;
    if (!m_animModel.empty()) paths.push_back(m_animModel);
    paths.push_back("assets/models/anim_test.fbx");
    paths.push_back("assets/models/anim_test.glb");
    for (const std::string& path : paths) {
        if (!std::filesystem::exists(path)) continue;
        auto model = loadSkeletalModel(path, {.scale = 1.0f});
        if (!model) continue;

        auto actor = std::make_unique<AnimActor>();
        actor->mesh = m_renderer.uploadSkinnedMesh(model->vertices, model->indices);

        MaterialDesc mat;
        mat.tint = glm::vec3(1.0f);
        if (!model->albedoEmbedded.empty()) // the model's own texture, embedded in the FBX
            mat.albedo = m_renderer.loadTextureFromMemory(model->albedoEmbedded.data(),
                                                          model->albedoEmbedded.size(), path);
        if (mat.albedo == 0 && !model->albedo.empty())
            mat.albedo = m_renderer.loadTexture(model->albedo);
        if (mat.albedo == 0 && std::filesystem::exists("assets/models/anim_test_body.png"))
            mat.albedo = m_renderer.loadTexture("assets/models/anim_test_body.png");
        const bool textured = mat.albedo != 0;
        if (!textured) mat.tint = glm::vec3(0.7f, 0.72f, 0.78f); // flat grey, not atlas magenta
        actor->material = m_renderer.createMaterial(mat);

        // Robust upright fit: rotate the longest bounds axis (head-to-toe) to +Y —
        // self-corrects Z-up FBX and Y-up GLB alike — then scale to 1.8 m and drop
        // the base to the floor. finalPos = place * scale * orient * skin * v.
        const glm::vec3 ext = model->boundsMax - model->boundsMin;
        glm::mat4 orient(1.0f);
        float upExtent = ext.y;
        if (ext.z >= ext.x && ext.z >= ext.y) { // Z-up: rotate -90° about X
            orient = glm::rotate(glm::mat4(1.0f), -glm::half_pi<float>(), glm::vec3(1, 0, 0));
            upExtent = ext.z;
        } else if (ext.x > ext.y && ext.x > ext.z) { // X-up (on its side): about Z
            orient = glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0, 0, 1));
            upExtent = ext.x;
        }
        const float norm = 1.8f / std::max(0.01f, upExtent);
        // Stand it on the terrain surface a few m ahead, so the whole body frames
        // in the spawn camera rather than floating at spawn height (y 8).
        const glm::vec3 place{clientSpawnPos().x, 5.0f, clientSpawnPos().z - 4.0f};
        // rootInverse now lives in the skinning matrices (Animator), so the model
        // transform is just display placement.
        actor->transform = glm::translate(glm::mat4(1.0f), place) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(norm)) * orient;

        // Many imported rigs ship no usable clip (Fab reference poses are ~1 frame).
        // With a real clip, sample it; otherwise drive a procedural idle computed
        // by EXACT bind-local matrix multiply (Animator::idlePose — no decompose,
        // so deep chains like arms/fingers don't warp).
        actor->model = std::move(*model);
        // Merge external animation clips (Mixamo/MoCap) onto this skeleton by bone name —
        // a same-hierarchy clip (mixamorig, or MoCap Online minus the prefix) needs no
        // retarget and plays through samplePose's delta path unchanged.
        if (!m_animClip.empty()) {
            appendClipsFromFile(actor->model, m_animClip, {.scale = 1.0f});
        }
        // --animretarget: foreign-skeleton clip (UE5 mannequin, MoCap Online) — rest-pose
        // compensated so a different bind pose doesn't distort the mesh.
        if (!m_animRetarget.empty()) {
            retargetClipsFromFile(actor->model, m_animRetarget, {.scale = 1.0f});
        }
        // Play the LONGEST clip: a merged multi-second locomotion clip beats a bundled
        // 1-frame reference pose. Below 0.02s there's nothing to sample → bind idle.
        actor->clipIndex = 0;
        for (int i = 1; i < static_cast<int>(actor->model.clips.size()); ++i) {
            const AnimClip& c = actor->model.clips[i];
            const AnimClip& best = actor->model.clips[actor->clipIndex];
            if (c.duration / c.ticksPerSec > best.duration / best.ticksPerSec) {
                actor->clipIndex = i;
            }
        }
        actor->hasRealClip =
            !actor->model.clips.empty() &&
            actor->model.clips[actor->clipIndex].duration /
                    actor->model.clips[actor->clipIndex].ticksPerSec >=
                0.02f;
        m_animActor = std::move(actor);
        log::info("anim actor '{}' up: {} clips, {}, textured={} (up-extent {:.2f} m -> x{:.3f})",
                  path, m_animActor->model.clips.size(),
                  m_animActor->hasRealClip
                      ? std::format("clip {}", m_animActor->clipIndex).c_str()
                      : "procedural idle",
                  textured, upExtent, norm);
        return;
    }
    log::info("no assets/models/anim_test.{{fbx,glb}} staged — skeletal proof skipped");
}

void Engine::loadNpcActor() {
    // Optional (gitignored) skinned character for in-game NPCs/companions. When present it
    // replaces the box proxies with an animated humanoid; absent → NPCs stay boxes (no
    // regression). Same load path as the anim actor, but the transform stores only the
    // upright-fit (normalize + orient) — the render adds each entity's world pos + yaw.
    const char* charPath = "assets/models/npc_char.fbx";
    if (!std::filesystem::exists(charPath)) {
        log::info("no assets/models/npc_char.fbx staged — NPCs render as box proxies");
        return;
    }
    auto model = loadSkeletalModel(charPath, {.scale = 1.0f});
    if (!model) return;

    auto actor = std::make_unique<AnimActor>();
    actor->mesh = m_renderer.uploadSkinnedMesh(model->vertices, model->indices);
    MaterialDesc mat;
    if (!model->albedoEmbedded.empty()) // the character's own texture, embedded in the FBX
        mat.albedo = m_renderer.loadTextureFromMemory(model->albedoEmbedded.data(),
                                                      model->albedoEmbedded.size(), charPath);
    if (mat.albedo == 0 && !model->albedo.empty()) mat.albedo = m_renderer.loadTexture(model->albedo);
    // The skinned draw path REQUIRES a texture (untextured draws are skipped → invisible),
    // so fall back to the neutral body texture, then the atlas, exactly like the anim actor.
    if (mat.albedo == 0 && std::filesystem::exists("assets/models/anim_test_body.png"))
        mat.albedo = m_renderer.loadTexture("assets/models/anim_test_body.png");
    if (mat.albedo == 0) mat.albedo = m_atlasTexture;
    mat.tint = mat.albedo == m_atlasTexture ? glm::vec3(0.55f, 0.57f, 0.63f) : glm::vec3(1.0f);
    mat.emissive = glm::vec3(0.05f, 0.05f, 0.06f); // lift NPCs off pure-black in the night scene
    actor->material = m_renderer.createMaterial(mat);
    // Zombie variant: same albedo/mesh, sickly-green tint + stronger green emissive so a zombie
    // reads as a zombie and stays visible in the dark.
    MaterialDesc zmat = mat;
    zmat.tint = glm::vec3(0.45f, 0.62f, 0.38f);
    zmat.emissive = glm::vec3(0.05f, 0.14f, 0.05f);
    m_zombieMaterial = m_renderer.createMaterial(zmat);

    const glm::vec3 ext = model->boundsMax - model->boundsMin;
    glm::mat4 orient(1.0f);
    float upExtent = ext.y;
    if (ext.z >= ext.x && ext.z >= ext.y) {
        orient = glm::rotate(glm::mat4(1.0f), -glm::half_pi<float>(), glm::vec3(1, 0, 0));
        upExtent = ext.z;
    } else if (ext.x > ext.y && ext.x > ext.z) {
        orient = glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0, 0, 1));
        upExtent = ext.x;
    }
    const float norm = 1.8f / std::max(0.01f, upExtent); // ~human height; base at model origin
    actor->transform = glm::scale(glm::mat4(1.0f), glm::vec3(norm)) * orient; // NO world placement

    actor->model = std::move(*model);
    // Load walk + idle clips for the speed-driven idle↔walk blend. Retarget handles BOTH
    // exact- and variant-bind characters (PSX, etc.); each clip lands at a known index.
    const auto loadClip = [&](const char* file) -> int {
        if (!std::filesystem::exists(file)) return -1;
        const int before = static_cast<int>(actor->model.clips.size());
        if (retargetClipsFromFile(actor->model, file, {.scale = 1.0f}) == 0)
            appendClipsFromFile(actor->model, file, {.scale = 1.0f});
        return static_cast<int>(actor->model.clips.size()) > before ? before : -1;
    };
    m_npcWalkClip = loadClip("assets/models/npc_walk.fbx");
    m_npcIdleClip = loadClip("assets/models/npc_idle.fbx");
    // Zombie shamble clips retargeted onto the SAME model — NpcZombie entities blend
    // these (see submitHumanoid); absent → zombies fall back to the regular locomotion.
    m_zombieWalkClip = loadClip("assets/models/zombie_walk.fbx");
    m_zombieIdleClip = loadClip("assets/models/zombie_idle.fbx");
    m_pistolWalkClip = loadClip("assets/models/pistol_walk.fbx"); // armed shooter locomotion
    m_pistolIdleClip = loadClip("assets/models/pistol_idle.fbx");
    actor->clipIndex = m_npcWalkClip >= 0 ? m_npcWalkClip : 0; // single-clip path plays walk
    actor->hasRealClip = !actor->model.clips.empty() &&
                         actor->model.clips[actor->clipIndex].duration /
                                 actor->model.clips[actor->clipIndex].ticksPerSec >=
                             0.02f;
    m_npcActor = std::move(actor);

    // Ground the model: the render places the model ORIGIN at the entity's feet (pos.y), but the
    // rig's origin isn't at the soles — so without this the character floats ("mid air") or sinks.
    // Skin the bind pose, find its lowest vertex under scale+orient, and bake a vertical shift so
    // the soles sit exactly at the origin. Constant (bind-pose), no per-frame cost.
    {
        const glm::mat4 fit = glm::scale(glm::mat4(1.0f), glm::vec3(norm)) * orient;
        // Lowest skinned-vertex Y of a pose under a given placement matrix.
        const auto minSkinnedY = [&](const Pose& p, const glm::mat4& place) {
            float mn = 1e9f;
            for (const SkinnedVertex& v : m_npcActor->model.vertices) {
                glm::vec3 sp(0.0f);
                for (int i = 0; i < 4; ++i) {
                    const float w = v.weights[i];
                    if (w <= 0.0f) continue;
                    const int b = v.bones[i];
                    if (b < 0 || b >= static_cast<int>(p.skinningMatrices.size())) continue;
                    sp += w * glm::vec3(p.skinningMatrices[b] * glm::vec4(v.pos, 1.0f));
                }
                mn = std::min(mn, (place * glm::vec4(sp, 1.0f)).y);
            }
            return mn;
        };
        // Ground the rest pose: shift so the bind soles sit at the origin (= entity feet).
        const float minY = minSkinnedY(bindPose(m_npcActor->model), fit);
        m_npcActor->transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -minY, 0.0f)) * fit;
        // Precompute each clip's lowest-point curve (in the grounded transform space) so the render
        // can keep the planted foot on the floor through the whole cycle (kills the walk-float).
        for (int c = 0; c < static_cast<int>(m_npcActor->model.clips.size()); ++c) {
            const AnimClip& clip = m_npcActor->model.clips[c];
            const float durSec = clip.duration / (clip.ticksPerSec > 0.0f ? clip.ticksPerSec : 25.0f);
            std::array<float, kFootCurveSamples> curve{};
            for (int s = 0; s < kFootCurveSamples; ++s) {
                const float ts = durSec * static_cast<float>(s) / kFootCurveSamples;
                curve[s] = minSkinnedY(samplePose(m_npcActor->model, clip, ts),
                                       m_npcActor->transform);
            }
            m_clipFootCurve[c] = curve;
        }
        log::info("NPC actor up: {} verts, {} clips (walk={}, idle={}, zwalk={}, zidle={}) "
                  "(norm x{:.3f}, grounded {:.3f})",
                  m_npcActor->model.vertices.size(), m_npcActor->model.clips.size(), m_npcWalkClip,
                  m_npcIdleClip, m_zombieWalkClip, m_zombieIdleClip, norm, -minY);

        // Facing: derive the rig's forward axis from the bind foot->toe direction (feet point
        // forward), in the render's post-transform/pre-yaw space, then the offset that rotates it
        // to the -Z the server yaw convention assumes. Deterministic — no screenshot guessing.
        const SkeletalModel& mdl = m_npcActor->model;
        const auto boneBySuffix = [&](const char* suffix) -> int {
            for (int b = 0; b < static_cast<int>(mdl.bones.size()); ++b) {
                const std::string& n = mdl.bones[b].name;
                const std::size_t colon = n.find_last_of(':');
                if ((colon == std::string::npos ? n : n.substr(colon + 1)) == suffix) return b;
            }
            return -1;
        };
        const auto jointPos = [&](int b) { // bind position in render space (post-transform, pre-yaw)
            return glm::vec3(m_npcActor->transform * mdl.rootInverse *
                             glm::inverse(mdl.bones[b].offset) * glm::vec4(0, 0, 0, 1));
        };
        glm::vec3 fwd(0.0f);
        int contrib = 0;
        for (const auto& [foot, toe] : {std::pair{"LeftFoot", "LeftToeBase"},
                                        std::pair{"RightFoot", "RightToeBase"}}) {
            const int fb = boneBySuffix(foot), tb = boneBySuffix(toe);
            if (fb < 0 || tb < 0) continue;
            const glm::vec3 d = jointPos(tb) - jointPos(fb);
            fwd += glm::vec3(d.x, 0.0f, d.z); // horizontal foot->toe
            ++contrib;
        }
        if (contrib > 0 && glm::length(fwd) > 1e-4f) {
            fwd = glm::normalize(fwd);
            m_humanoidYawOffset = std::atan2(fwd.x, -fwd.z); // rotates fwd onto -Z
            log::info("humanoid facing: forward=({:.2f},{:.2f}) yawOffset={:.1f} deg (from {} feet)",
                      fwd.x, fwd.z, glm::degrees(m_humanoidYawOffset), contrib);
        } else {
            log::warn("humanoid facing: no foot/toe bones found — yaw offset stays 0");
        }

        // Two-bone foot-IK leg chains (hip/knee/ankle + knee hinge axis + sole offset).
        m_footIkRig = buildFootIkRig(mdl, m_npcActor->transform);
        log::info("foot IK: legs {} (soleOffset {:.3f})",
                  m_footIkRig.valid() ? "found" : "MISSING", m_footIkRig.soleOffset);
    }
}

void Engine::simulateClientTick(const PlayerCommand& frameCmd) {
    PlayerCommand cmd = frameCmd;
    cmd.tick = m_tick; // unique per tick even when one frame spans several ticks
    cmd.selectedSlot = m_selectedSlot;
    m_client.sendCommand(cmd);

    m_localFireCooldown -= kFixedDt;
    m_muzzleFlash -= kFixedDt;
    if (cmd.fire && m_localFireCooldown <= 0.0f) {
        m_localFireCooldown = kFireInterval;
        m_muzzleFlash = 0.08f;
        m_audio.play(Sound::Gunshot, 0.6f);
    }

    m_player.update(cmd, kFixedDt, m_physics);

    // Footsteps: paced by horizontal speed while grounded.
    const glm::vec3 vel = m_player.velocity();
    const float speed = glm::length(glm::vec2(vel.x, vel.z));
    if (m_player.onGround() && speed > 1.0f) {
        m_footstepTimer -= kFixedDt;
        if (m_footstepTimer <= 0.0f) {
            m_footstepTimer = cmd.sprint ? 0.30f : 0.45f;
            m_audio.play(Sound::Footstep, 0.5f);
        }
    } else {
        m_footstepTimer = 0.0f;
    }
    m_physics.step(kFixedDt);
    if (m_player.position().y < kFallResetY)
        m_player.setState(clientSpawnPos(), glm::vec3(0)); // fell out (colliders pending)
    m_prevPlayerPos = m_currPlayerPos;
    m_currPlayerPos = m_player.position();
}

void Engine::render(float alpha) {
    if (m_imguiReady) { // frame opens before scene submits so the editor can use UI
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
    }

    Camera playerCamera;
    playerCamera.yaw = m_lastCmd.yaw; // freshest mouse sample, not the tick's
    playerCamera.pitch = m_lastCmd.pitch;
    {
        const glm::vec3 body = glm::mix(m_prevPlayerPos, m_currPlayerPos, alpha);
        const glm::vec3 eye = body + glm::vec3(0.0f, m_player.eyeHeight(), 0.0f);
        if (m_perspective == GameRules::Perspective::Third) {
            // Over-shoulder TPS: camera sits behind/above/right of the eye and
            // pulls in if a static (terrain/prop) collider blocks the line of sight.
            constexpr float kBehind = 2.8f;
            constexpr float kAbove = 0.45f;
            constexpr float kShoulder = 0.55f;
            constexpr float kPullEps = 0.12f;
            const glm::vec3 fwd = playerCamera.forward();
            const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
            glm::vec3 right = glm::cross(fwd, worldUp);
            const float rLen = glm::length(right);
            right = rLen > 1e-4f ? right / rLen : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 desired = eye - fwd * kBehind + worldUp * kAbove + right * kShoulder;
            const glm::vec3 toCam = desired - eye;
            const float dist = glm::length(toCam);
            glm::vec3 camPos = desired;
            if (dist > 1e-4f) {
                const glm::vec3 dir = toCam / dist;
                const auto hit = m_physics.raycast(eye, dir, dist);
                if (hit.hit) {
                    const float pulled = std::max(0.0f, glm::length(hit.pos - eye) - kPullEps);
                    camPos = eye + dir * pulled;
                }
            }
            playerCamera.pos = camPos;
        } else {
            playerCamera.pos = eye;
        }
    }

    // Animation booth: a fixed, close, deterministic framing of the anim actor so
    // VLM grading is consistent (the SIE-booth idea) instead of hostage to where
    // the auto-spawn camera happens to settle.
    Camera boothCamera;
    if (m_animBooth && m_animActor) {
        const glm::vec3 base(m_animActor->transform[3]);
        const glm::vec3 mid = base + glm::vec3(0.0f, 0.9f, 0.0f); // ~chest height
        // Orbit 45 deg off dead-front to a 3/4 view: a straight side/front profile lets the
        // arms hang inside the torso silhouette (the VLM then reports "no arms" even at bind);
        // a 3/4 angle separates both arms + hands from the body so extremities are gradeable.
        constexpr float kOrbit = 0.785398f; // 45 deg
        boothCamera.pos = mid + 2.6f * glm::vec3(std::sin(kOrbit), 0.0f, std::cos(kOrbit));
        boothCamera.yaw = kOrbit;  // look back toward the actor
        boothCamera.pitch = 0.0f;  // level
        boothCamera.fovY = glm::radians(45.0f); // tighter → the figure fills the frame
    }
    const Camera& camera =
        m_animBooth && m_animActor ? boothCamera : (m_editorActive ? m_editorCamera : playerCamera);

    // Audio listener follows the active camera so positional SFX attenuate/pan
    // relative to what the player is looking from. right = forward × world-up.
    {
        const glm::vec3 fwd = camera.forward();
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
        m_audio.setListener(camera.pos, fwd, right);
    }

    m_renderer.beginFrame(camera, alpha);
    for (const auto& [pos, mesh] : m_chunkMeshes)
        m_renderer.submitChunk(mesh, glm::vec3(pos.x, pos.y, pos.z) * (kChunkSize * kVoxelSize));

    const std::vector<PlayerState> remotes = m_client.remoteViewStates();
    for (const PlayerState& remote : remotes) {
        m_renderer.submitChunk(m_remotePlayerMesh, remote.pos);
        // Positional footsteps for remote players: speed is derived from the
        // interpolated position delta (no net change), paced like local footsteps,
        // and played through playAt so distant players are quieter / panned.
        RemoteAudioState& ra = m_remoteAudio[remote.playerId];
        float speed = 0.0f;
        if (ra.seen && m_frameDt > 0.0f)
            speed = glm::length(glm::vec2(remote.pos.x - ra.prevPos.x,
                                          remote.pos.z - ra.prevPos.z)) /
                    m_frameDt;
        ra.prevPos = remote.pos;
        ra.seen = true;
        if (remote.onGround && speed > 1.0f) {
            ra.stepTimer -= m_frameDt;
            if (ra.stepTimer <= 0.0f) {
                ra.stepTimer = 0.42f;
                m_audio.playAt(Sound::Footstep, remote.pos, 0.6f);
            }
        } else {
            ra.stepTimer = 0.0f;
        }
    }

    // Shared animated humanoid for NPCs/companions. One clip, sampled per-instance at a
    // per-id phase so a room of NPCs isn't lock-stepped; placed at the entity's pos + yaw.
    // Falls back to the box proxy when no npc_char.fbx is staged.
    if (m_npcActor && m_npcActor->mesh != 0) m_npcActor->time += m_frameDt;
    // Facing correction derived at load from the rig's bind foot->toe direction (see loadNpcActor).
    const float kHumanoidYawOffset = m_humanoidYawOffset;
    // Sample a clip's precomputed lowest-point curve at time t (linear-interp, wraps over the
    // clip). Used to drop each pose so its planted foot stays on the ground (no walk-float).
    const auto footYAt = [&](int clip, float t) -> float {
        const auto it = m_clipFootCurve.find(clip);
        if (it == m_clipFootCurve.end() || clip < 0) return 0.0f;
        const AnimClip& c = m_npcActor->model.clips[clip];
        const float durSec = c.duration / (c.ticksPerSec > 0.0f ? c.ticksPerSec : 25.0f);
        if (durSec <= 0.0f) return it->second[0];
        const float phase = std::fmod(std::fmod(t, durSec) + durSec, durSec) / durSec; // 0..1
        const float fs = phase * kFootCurveSamples;
        const int i0 = static_cast<int>(fs) % kFootCurveSamples;
        const int i1 = (i0 + 1) % kFootCurveSamples;
        return glm::mix(it->second[i0], it->second[i1], fs - std::floor(fs));
    };
    // idleClip/walkClip select which locomotion set this humanoid blends (regular NPC vs
    // zombie shamble) — both live on the one shared m_npcActor model.
    const auto submitHumanoid = [&](const glm::vec3& pos, float yaw, std::uint32_t id,
                                    int idleClip, int walkClip, float walkWeight,
                                    MaterialHandle material) {
        if (m_npcActor && m_npcActor->mesh != 0) {
            const float t = m_npcActor->time + 0.37f * static_cast<float>(id % 13);
            const float w = glm::clamp(walkWeight, 0.0f, 1.0f);
            // Whole-body drop so the planted foot stays on the floor through the walk cycle.
            float footY = 0.0f;
            if (idleClip >= 0 && walkClip >= 0)
                footY = glm::mix(footYAt(idleClip, t), footYAt(walkClip, t), w);
            else if (m_npcActor->hasRealClip)
                footY = footYAt(m_npcActor->clipIndex, t);
            const glm::mat4 xform = glm::translate(glm::mat4(1.0f), pos - glm::vec3(0, footY, 0)) *
                                    glm::rotate(glm::mat4(1.0f), yaw + kHumanoidYawOffset,
                                                glm::vec3(0, 1, 0)) *
                                    m_npcActor->transform;
            Pose pose;
            if (idleClip >= 0 && walkClip >= 0) {
                // Idle↔walk blend by the server's AUTHORITATIVE walk weight (EntityState.anim);
                // client-derived speed read ~0 (interp) and froze NPCs in idle.
                std::vector<glm::mat4> locals =
                    blendLocalMatrices(m_npcActor->model, m_npcActor->model.clips[idleClip], t,
                                       m_npcActor->model.clips[walkClip], t, w);
                // Two-bone foot IK plants each foot on the terrain under it (steps/slopes). No-op on
                // flat ground (each foot is already at/above its target), so no regression there.
                if (m_footIkRig.valid()) {
                    std::vector<glm::mat4> globals;
                    resolveLocals(m_npcActor->model, locals, &globals);
                    const glm::vec3 fwd(-std::sin(yaw), 0.0f, -std::cos(yaw)); // facing = knee pole
                    const auto groundY = [&](float wx, float wz) -> float {
                        const int vx = static_cast<int>(std::floor(wx / kVoxelSize));
                        const int vz = static_cast<int>(std::floor(wz / kVoxelSize));
                        const int vy0 = static_cast<int>(std::floor(pos.y / kVoxelSize));
                        for (int vy = vy0 + 1; vy >= vy0 - 4; --vy)
                            if (m_voxels.blockRegistry()
                                    .get(m_voxels.blockAt(glm::ivec3(vx, vy, vz)))
                                    .solid)
                                return static_cast<float>(vy + 1) * kVoxelSize;
                        return pos.y;
                    };
                    applyFootIk(m_npcActor->model, m_footIkRig, locals, globals, xform, groundY, fwd,
                                1.0f);
                }
                pose = resolveLocals(m_npcActor->model, locals, nullptr);
            } else if (m_npcActor->hasRealClip) {
                pose = samplePose(m_npcActor->model,
                                  m_npcActor->model.clips[m_npcActor->clipIndex], t);
            } else {
                pose = idlePose(m_npcActor->model, t);
            }
            m_renderer.submitSkinned(m_npcActor->mesh, xform, pose,
                                     material != MaterialHandle::Invalid ? material
                                                                         : m_npcActor->material);
        } else {
            m_renderer.submitChunk(m_remotePlayerMesh, pos);
        }
    };

    const float bobPhase = static_cast<float>(m_tick % 120) / 120.0f * glm::two_pi<float>();
    for (const EntityState& e : m_client.entities()) {
        switch (e.archetype) {
        case 1: // ItemPickup: gentle bob so loot reads as loot
            m_renderer.submitChunk(m_pickupMesh,
                                   e.pos + glm::vec3(0, 0.08f * std::sin(bobPhase), 0));
            break;
        case 2: // Projectile: glowing tracer
            m_renderer.submitChunk(m_pickupMesh, e.pos);
            m_renderer.submitPointLight(e.pos, glm::vec3(1.0f, 0.5f, 0.15f), 6.0f);
            break;
        case 3: // Deployable: armed trap, red pulse
            m_renderer.submitChunk(m_pickupMesh, e.pos);
            m_renderer.submitPointLight(e.pos, glm::vec3(1.0f, 0.1f, 0.1f), 4.0f);
            break;
        case 4: // NpcChaser: animated humanoid (or box if none staged)
            submitHumanoid(e.pos, e.yaw, e.id, m_npcIdleClip, m_npcWalkClip, e.anim / 255.0f,
                           MaterialHandle::Invalid);
            break;
        case 5: // NpcShooter: armed — uses the pistol locomotion set (falls back to plain)
            submitHumanoid(e.pos, e.yaw, e.id,
                           m_pistolIdleClip >= 0 ? m_pistolIdleClip : m_npcIdleClip,
                           m_pistolWalkClip >= 0 ? m_pistolWalkClip : m_npcWalkClip,
                           e.anim / 255.0f, MaterialHandle::Invalid);
            break;
        case 6: // Turret: small box + a blue status light
            m_renderer.submitChunk(m_pickupMesh, e.pos);
            m_renderer.submitPointLight(e.pos + glm::vec3(0, 0.5f, 0),
                                        glm::vec3(0.2f, 0.5f, 1.0f), 5.0f);
            break;
        case 7: // Companion: animated humanoid + a friendly green light
            submitHumanoid(e.pos, e.yaw, e.id, m_npcIdleClip, m_npcWalkClip, e.anim / 255.0f,
                           MaterialHandle::Invalid);
            m_renderer.submitPointLight(e.pos + glm::vec3(0, 1.6f, 0),
                                        glm::vec3(0.2f, 1.0f, 0.35f), 5.0f);
            break;
        case 8: // NpcZombie: same mesh, zombie shamble clips + green material (falls back if unstaged)
            submitHumanoid(e.pos, e.yaw, e.id,
                           m_zombieIdleClip >= 0 ? m_zombieIdleClip : m_npcIdleClip,
                           m_zombieWalkClip >= 0 ? m_zombieWalkClip : m_npcWalkClip,
                           e.anim / 255.0f, m_zombieMaterial);
            break;
        default:
            break;
        }
    }

    for (const PropInstance& prop : m_props)
        m_renderer.submitMesh(prop.mesh, prop.transform, prop.material);

    if (m_animBooth && m_animActor) { // fill light so the dark PSX figure reads clearly
        const glm::vec3 base(m_animActor->transform[3]);
        m_renderer.submitPointLight(base + glm::vec3(0.0f, 1.2f, 2.0f), glm::vec3(2.4f), 8.0f);
    }
    // The anim proof actor is a BOOTH asset (--animshot / --animmodel). Only render it in
    // booth mode so it doesn't stand in the middle of normal gameplay as a static T-pose.
    if (m_animBooth && m_animActor && m_animActor->mesh != 0) {
        m_animActor->time += m_frameDt;
        const Pose pose =
            m_animActor->hasRealClip
                ? samplePose(m_animActor->model,
                             m_animActor->model.clips[m_animActor->clipIndex], m_animActor->time)
                : idlePose(m_animActor->model, m_animActor->time);
        m_renderer.submitSkinned(m_animActor->mesh, m_animActor->transform, pose,
                                 m_animActor->material);
    }

    for (const EditorProp& prop : m_editorProps) { // editor-placed decoration meshes
        const EditorPropMesh& pm = editorPropMesh(prop.assetPath);
        if (pm.mesh != 0) m_renderer.submitMesh(pm.mesh, prop.transform, pm.material);
    }

    for (const EditorLight& light : m_editorLights) { // placed lights are world lights
        if (light.type == 0)
            m_renderer.submitPointLight(light.pos, light.color, light.radius);
        else
            m_renderer.submitSpotLight(light.pos, light.dir, light.color, light.radius,
                                       light.angle);
    }

    if (m_editorActive && m_editor && m_imguiReady) {
        EditorContext ctx{m_editorCamera,
                          m_voxels,
                          m_input,
                          m_renderer,
                          m_editorLights,
                          m_seedVolumes,
                          m_editorProps,
                          [this](glm::ivec3 v, BlockId b) { m_client.sendVoxelOp(v, b); },
                          [this](bool on) { m_window.setRelativeMouse(on); },
                          [this] {
                              if (m_server) m_server->saveTo("saves/quick.json");
                              saveEditorExtras();
                          }};
        // Prop placement is a server intent, exactly like a voxel edit: the prop
        // returns via PropAddedMsg and appears in m_editorProps with a collider.
        ctx.requestPlaceProp = [this](std::string asset, glm::mat4 transform) {
            m_client.sendPlaceProp(asset, transform);
        };
        ctx.requestMoveProp = [this](std::uint32_t id, glm::mat4 transform) {
            if (id != 0) m_client.sendMoveProp(id, transform);
        };
        ctx.requestRemoveProp = [this](std::uint32_t id) {
            if (id != 0) m_client.sendRemoveProp(id);
        };
        ctx.requestNewMap = [this](int terrain, int environment, std::uint32_t seed) {
            const auto t = static_cast<GameRules::Terrain>(terrain < 0 || terrain > 2 ? 0 : terrain);
            const auto e =
                static_cast<GameRules::Environment>(environment < 0 || environment > 2 ? 0
                                                                                       : environment);
            return rebuildWorld(t, e, seed);
        };
        if (m_server) {
            ctx.currentTerrain = static_cast<int>(m_server->rules().terrain);
            ctx.currentEnvironment = static_cast<int>(m_server->rules().environment);
            ctx.currentSeed = m_server->seed();
            ctx.hemisphereAmbient = m_hemisphereAmbient;
        }
        ctx.setHemisphereAmbient = [this](bool on) {
            m_hemisphereAmbient = on;
            GameRules r = m_server ? m_server->rules() : GameRules{};
            r.hemisphereAmbient = on;
            applyEnvironment(r);
        };
        ctx.listFiles = [](const std::string& dir) {
            std::vector<std::string> out;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                out.push_back(e.path().filename().string() +
                              (e.is_directory() ? "/" : ""));
            std::sort(out.begin(), out.end());
            return out;
        };
        ctx.readFile = [](const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return std::string{};
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        ctx.writeFile = [](const std::string& path, const std::string& text) {
            std::ofstream out(path, std::ios::binary);
            if (!out) return false;
            out << text;
            return out.good();
        };
        ctx.reloadScripts = [this] { return m_server && m_server->reloadScripts(); };
        // Import a dropped/typed asset: validate by type, then copy into the right
        // assets/ subdir. Models are validated by actually loading them (and probed
        // for a rig); textures by an stbi_info header decode. All filesystem work
        // is error_code-based — no exceptions cross this boundary.
        ctx.importAsset = [](const std::string& sourcePath) -> std::string {
            namespace fs = std::filesystem;
            if (sourcePath.empty()) return {}; // nothing typed/dropped: not attempted
            std::error_code ec;
            const fs::path src(sourcePath);
            if (!fs::exists(src, ec) || fs::is_directory(src, ec))
                return "rejected: " + sourcePath + " is not a file";

            std::string ext = src.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string filename = src.filename().string();

            const bool isModel =
                ext == ".fbx" || ext == ".obj" || ext == ".glb" || ext == ".gltf";
            const bool isTexture = ext == ".png" || ext == ".jpg" || ext == ".jpeg";

            const auto copyInto = [&](const char* subdir) -> std::string {
                const fs::path destDir = fs::path("assets") / subdir;
                fs::create_directories(destDir, ec);
                const fs::path dest = destDir / src.filename();
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                if (ec) return "rejected: " + sourcePath + " copy failed (" + ec.message() + ")";
                return {}; // empty = copy succeeded
            };

            if (isModel) {
                const auto model = loadStaticModel(src);
                if (!model) return "rejected: " + sourcePath + " failed to load";
                // OBJ never carries a skeleton; probe FBX/GLB for a rig so the
                // summary can report bone count like the anim pipeline expects.
                int bones = 0;
                if (ext == ".fbx" || ext == ".glb")
                    if (const auto skel = loadSkeletalModel(src))
                        bones = static_cast<int>(skel->bones.size());
                if (const std::string err = copyInto("models"); !err.empty()) return err;

                const float height = model->boundsMax.y - model->boundsMin.y;
                std::string summary = std::format("imported models/{} ({:.1f}m", filename, height);
                if (bones > 0) summary += std::format(", {} bones", bones);
                summary += ")";
                if (height > 20.0f) summary += " — check scale"; // imported, but units look off
                return summary;
            }

            if (isTexture) {
                int w = 0, h = 0, c = 0;
                if (!stbi_info(sourcePath.c_str(), &w, &h, &c))
                    return "rejected: " + sourcePath + " is not a decodable image";
                if (const std::string err = copyInto("textures"); !err.empty()) return err;
                return std::format("imported textures/{} ({}x{})", filename, w, h);
            }

            return "rejected: unsupported type (" + ext + ")";
        };
        m_editor->update(ctx, ImGui::GetIO().DeltaTime);
    }

    if (!m_editorActive) {
        if (m_muzzleFlash > 0.0f)
            m_renderer.submitPointLight(camera.pos + camera.forward() * 0.4f,
                                        glm::vec3(1.0f, 0.72f, 0.35f) * (m_muzzleFlash / 0.08f),
                                        8.0f);
        // Crosshair is an FPS aim cue; third-person uses the over-shoulder view instead.
        if (m_perspective == GameRules::Perspective::First) m_renderer.drawCrosshair();
    }
    m_renderer.endFrame();

    if (m_imguiReady) {
        ImGui::SetNextWindowPos({12, 12});
        ImGui::Begin("hud", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        ImGui::Text("HP %.0f", m_client.health());
        ImGui::Text("players %zu", remotes.size() + 1);
        ImGui::Text("pos %.1f %.1f %.1f", m_currPlayerPos.x, m_currPlayerPos.y,
                    m_currPlayerPos.z);
        ImGui::Text("%.0f fps", ImGui::GetIO().Framerate);
        ImGui::End();
        drawInventoryUi();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

void Engine::drawInventoryUi() {
    if (!m_clientWorldReady) return;
    const Inventory& inv = m_client.inventory();
    const GameRules& rules = m_client.rules();
    const ImGuiViewport* view = ImGui::GetMainViewport();
    const auto slotLabel = [&](int i) {
        const ItemStack& s = inv.slot(i);
        return s.id == 0 ? std::string("-")
                         : std::format("{} x{}", m_items.get(s.id).name, s.count);
    };

    using Model = GameRules::InventoryModel;
    const int hotbarSlots = rules.inventoryModel == Model::WeaponSlots ? 4
                            : rules.inventoryModel == Model::GridOnly  ? 0
                                                                      : Inventory::kHotbar;
    if (hotbarSlots > 0) {
        ImGui::SetNextWindowPos({view->Size.x * 0.5f, view->Size.y - 16.0f}, ImGuiCond_Always,
                                {0.5f, 1.0f});
        ImGui::Begin("hotbar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs);
        for (int i = 0; i < hotbarSlots; ++i) {
            if (i > 0) ImGui::SameLine();
            const bool selected = m_selectedSlot == i;
            ImGui::TextColored(selected ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                        : ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                               "[%d %s]", i + 1, slotLabel(i).c_str());
        }
        if (rules.inventoryModel == Model::WeaponSlots) {
            // Materials/consumables read as counters, not managed slots.
            ImGui::SameLine();
            ImGui::TextColored({0.6f, 0.8f, 1.0f, 1.0f}, "  blocks:%d  medkits:%d",
                               inv.countOf(4), inv.countOf(3));
        }
        // Ammo readout for the equipped magazine weapon: loaded mag / reserve (H3).
        const ItemStack& sel = inv.slot(m_selectedSlot);
        const ItemDef& selDef = m_items.get(sel.id);
        if (selDef.type == ItemType::Weapon && selDef.magSize > 0) {
            ImGui::SameLine();
            if (rules.finiteAmmo) {
                const int reserve = selDef.ammoItem != 0 ? inv.countOf(selDef.ammoItem) : 0;
                ImGui::TextColored({1.0f, 0.85f, 0.3f, 1.0f}, "  ammo %d / %d",
                                   inv.magOf(sel.id), reserve);
            } else {
                ImGui::TextColored({1.0f, 0.85f, 0.3f, 1.0f}, "  ammo %d / inf",
                                   selDef.magSize);
            }
        }
        ImGui::End();
    }

    if (m_showBackpack && rules.inventoryModel != Model::WeaponSlots) {
        ImGui::SetNextWindowPos({view->Size.x * 0.5f, view->Size.y * 0.5f}, ImGuiCond_Always,
                                {0.5f, 0.5f});
        ImGui::Begin("backpack", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
        for (int i = 0; i < Inventory::kSlots; ++i) {
            ImGui::Text("%2d %s", i + 1, slotLabel(i).c_str());
            if ((i + 1) % 6 != 0) ImGui::SameLine(static_cast<float>((i % 6 + 1) * 140));
        }
        ImGui::End();
    }
}

void Engine::saveEditorExtras() const {
    // Props are NO LONGER saved here: they are server-authoritative world objects
    // persisted in the world save (ServerSim::saveTo). Extras keep only the
    // client-side editor authoring aids (lights, seed volumes).
    nlohmann::json j = nlohmann::json::object();
    j["lights"] = nlohmann::json::array();
    j["seedVolumes"] = nlohmann::json::array();
    for (const EditorLight& l : m_editorLights)
        j["lights"].push_back({{"type", l.type},
                               {"pos", {l.pos.x, l.pos.y, l.pos.z}},
                               {"color", {l.color.x, l.color.y, l.color.z}},
                               {"radius", l.radius},
                               {"dir", {l.dir.x, l.dir.y, l.dir.z}},
                               {"angle", l.angle}});
    for (const SeedVolume& v : m_seedVolumes)
        j["seedVolumes"].push_back({{"min", {v.min.x, v.min.y, v.min.z}},
                                    {"max", {v.max.x, v.max.y, v.max.z}},
                                    {"seed", v.seed}});
    std::ofstream out("saves/editor_extras.json");
    if (out) out << j.dump();
}

void Engine::loadEditorExtras() {
    std::ifstream in("saves/editor_extras.json");
    if (!in) return;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    for (const auto& l : j.value("lights", nlohmann::json::array())) {
        EditorLight light;
        light.type = l.value("type", 0);
        light.pos = {l["pos"][0], l["pos"][1], l["pos"][2]};
        light.color = {l["color"][0], l["color"][1], l["color"][2]};
        light.radius = l.value("radius", 8.0f);
        light.dir = {l["dir"][0], l["dir"][1], l["dir"][2]};
        light.angle = l.value("angle", 0.6f);
        m_editorLights.push_back(light);
    }
    for (const auto& v : j.value("seedVolumes", nlohmann::json::array())) {
        SeedVolume vol;
        vol.min = {v["min"][0], v["min"][1], v["min"][2]};
        vol.max = {v["max"][0], v["max"][1], v["max"][2]};
        vol.seed = v.value("seed", 0u);
        m_seedVolumes.push_back(vol);
    }
    // Props are intentionally NOT loaded here anymore — they arrive from the
    // server (PropAddedMsg / join replay), driven by the world save. A legacy
    // extras file may still carry a "props" array; it is simply ignored.
    log::info("editor extras loaded ({} lights, {} volumes)", m_editorLights.size(),
              m_seedVolumes.size());
}

bool Engine::runMenu(EngineConfig& config) {
    LanDiscovery discovery;
    discovery.start();
    m_window.setRelativeMouse(false);
    std::snprintf(m_menuMaster, sizeof(m_menuMaster), "%s", config.master.c_str());

    bool chosen = false;
    while (!m_window.shouldClose() && !chosen) {
        m_input.beginFrame();
        m_window.pollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        Camera idle; // empty world render = fog-colored backdrop for the menu
        m_renderer.beginFrame(idle, 0.0f);
        m_renderer.endFrame();

        const ImGuiViewport* view = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos({view->Size.x * 0.5f, view->Size.y * 0.5f}, ImGuiCond_Always,
                                {0.5f, 0.5f});
        ImGui::SetNextWindowSize({560, 0});
        ImGui::Begin("MeatEngine", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

        if (ImGui::Button("Singleplayer", {160, 0})) {
            config.mode = EngineConfig::Mode::Game;
            chosen = true;
        }
        ImGui::Separator();

        ImGui::InputText("server name", m_menuName, sizeof(m_menuName));
        ImGui::SameLine();
        if (ImGui::Button("Host", {100, 0})) {
            config.mode = EngineConfig::Mode::Host;
            config.serverName = m_menuName;
            chosen = true;
        }
        ImGui::Separator();

        ImGui::TextUnformatted("LAN servers");
        const std::vector<ServerAd> lan = discovery.servers();
        if (lan.empty()) ImGui::TextDisabled("  (none found — beacons appear within ~1 s)");
        for (std::size_t i = 0; i < lan.size(); ++i) {
            const ServerAd& ad = lan[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s  %s:%u  %d/%d", ad.name.c_str(), ad.address.c_str(), ad.port,
                        ad.players, ad.maxPlayers);
            ImGui::SameLine();
            if (ImGui::SmallButton("Join")) {
                config.mode = EngineConfig::Mode::Join;
                config.address = ad.address;
                config.port = ad.port;
                chosen = true;
            }
            ImGui::PopID();
        }
        ImGui::Separator();

        ImGui::InputText("master", m_menuMaster, sizeof(m_menuMaster));
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) { // blocking by design: one click, 3 s cap
            std::string host = m_menuMaster;
            std::uint16_t port = 27000;
            if (const auto colon = host.find(':'); colon != std::string::npos) {
                port = static_cast<std::uint16_t>(std::atoi(host.c_str() + colon + 1));
                host.resize(colon);
            }
            m_internetServers.clear();
            if (const auto body = httpGet(host, port, "/servers"))
                m_internetServers = parseServerList(*body);
        }
        for (std::size_t i = 0; i < m_internetServers.size(); ++i) {
            const ServerAd& ad = m_internetServers[i];
            ImGui::PushID(1000 + static_cast<int>(i));
            ImGui::Text("%s  %s:%u  %d/%d", ad.name.c_str(), ad.address.c_str(), ad.port,
                        ad.players, ad.maxPlayers);
            ImGui::SameLine();
            if (ImGui::SmallButton("Join")) {
                config.mode = EngineConfig::Mode::Join;
                config.address = ad.address;
                config.port = ad.port;
                chosen = true;
            }
            ImGui::PopID();
        }
        ImGui::Separator();

        ImGui::InputText("ip:port", m_menuAddr, sizeof(m_menuAddr));
        ImGui::SameLine();
        if (ImGui::Button("Direct join")) {
            std::string addr = m_menuAddr;
            config.port = 26000;
            if (const auto colon = addr.find(':'); colon != std::string::npos) {
                config.port = static_cast<std::uint16_t>(std::atoi(addr.c_str() + colon + 1));
                addr.resize(colon);
            }
            config.address = addr;
            config.mode = EngineConfig::Mode::Join;
            chosen = true;
        }
        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        m_window.swap();
    }
    discovery.stop();
    if (chosen) {
        config.master = m_menuMaster;
        m_window.setRelativeMouse(true);
    }
    return chosen;
}

void Engine::startHosting(const EngineConfig& config) {
    m_beacon.start(config.port, config.serverName);
    if (config.master.empty()) return;
    std::string host = config.master;
    std::uint16_t port = 27000;
    if (const auto colon = host.find(':'); colon != std::string::npos) {
        port = static_cast<std::uint16_t>(std::atoi(host.c_str() + colon + 1));
        host.resize(colon);
    }
    m_stopHeartbeat = false;
    m_masterHeartbeat = std::thread([this, host, port, config] {
        while (!m_stopHeartbeat) {
            const int players = m_server ? m_server->playerCount() : 0;
            const std::string body =
                std::format(R"({{"name":"{}","port":{},"players":{},"maxPlayers":8}})",
                            config.serverName, config.port, players);
            httpPost(host, port, "/announce", body);
            for (int i = 0; i < 300 && !m_stopHeartbeat; ++i) // 30 s in stoppable slices
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void Engine::stopHosting() {
    m_beacon.stop();
    m_stopHeartbeat = true;
    if (m_masterHeartbeat.joinable()) m_masterHeartbeat.join();
}

namespace {
// Apply the dev-chosen metres-per-voxel to the global ONCE, before any world/mesh/physics
// exists. Clamp to a sane band: 0.1 m (fine detail) … 8 m (far chunkier than Minecraft's
// 1 m). Written back into config.rules so server + clients carry the same world scale.
void applyVoxelSize(EngineConfig& config) {
    const float requested = config.rules.voxelSize;
    kVoxelSize = requested < 0.1f ? 0.1f : requested > 8.0f ? 8.0f : requested;
    config.rules.voxelSize = kVoxelSize;
    if (kVoxelSize != requested) {
        log::warn("voxelSize {:.3f} m out of range [0.1, 8.0] — clamped to {:.3f} m", requested,
                  kVoxelSize);
    }
    if (kVoxelSize != kDefaultVoxelSize) {
        log::info("voxel size = {:.3f} m/voxel (chunk edge {:.1f} m; engine default {:.2f} m)",
                  kVoxelSize, chunkWorldSize(), kDefaultVoxelSize);
    }
}
} // namespace

int Engine::run(const EngineConfig& configIn) {
    EngineConfig config = configIn;
    applyVoxelSize(config); // set metres-per-voxel before ANY world/mesh/physics is built
    if (config.mode == EngineConfig::Mode::Dedicated) return runDedicated(config);

    if (!initClientSystems()) {
        log::error("engine init failed");
        return 1;
    }
    // World Environment preset (gravity + fog + ambient). For --play/--editor/--project the client
    // shares config.rules with the in-process server. A networked join re-applies from Welcome
    // (host-authoritative voxelSize + environment) before setupClientWorld.
    applyEnvironment(config.rules);
    m_perspective = config.rules.perspective;
    m_hemisphereAmbient = config.rules.hemisphereAmbient;
    m_animBooth = config.animBooth;
    m_animModel = config.animModel;
    m_animClip = config.animClip;
    m_animRetarget = config.animRetarget;
    if (config.mode == EngineConfig::Mode::Browse && !runMenu(config)) return 0;
    if (!initNetwork(config)) {
        log::error("network init failed");
        return 1;
    }
    if (m_server && m_enetHost) startHosting(config);
    loadEditorExtras();
    log::info("MeatEngine up — mode {}, 60 Hz tick, {}³ chunks @ {} m voxels",
              static_cast<int>(config.mode), kChunkSize, kVoxelSize);

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    double accumulator = 0.0;
    int frameCount = 0; // for --shot auto-capture

    while (!m_window.shouldClose()) {
        m_input.beginFrame(); // before pollEvents so pressed() edges last one frame
        m_window.pollEvents();
        m_jobs.drainMainThread();

        if (m_input.pressed(GLFW_KEY_ESCAPE)) break;
        if (m_input.pressed(GLFW_KEY_F6)) m_renderer.reloadShaders();
        if (m_input.pressed(GLFW_KEY_F7)) {
            // A3: toggle hemisphere ambient — dark PSX-night (off) vs form-readable fill (on).
            m_hemisphereAmbient = !m_hemisphereAmbient;
            GameRules envRules;
            if (m_client.welcomed()) envRules = m_client.rules();
            else if (m_server) envRules = m_server->rules();
            envRules.hemisphereAmbient = m_hemisphereAmbient;
            applyEnvironment(envRules);
            log::info("hemisphere ambient {}", m_hemisphereAmbient ? "ON" : "OFF");
        }
        if (m_input.pressed(GLFW_KEY_F12)) m_renderer.captureScreenshot("build/shot.png");
        if (m_input.pressed(GLFW_KEY_F5) && m_server) {
            m_server->saveTo("saves/quick.json");
            saveEditorExtras();
        }
        if (m_input.pressed(GLFW_KEY_F1) && m_editor && m_server && m_clientWorldReady) {
            m_editorActive = !m_editorActive;
            if (m_editorActive) { // start where the player is looking from
                m_editorCamera.pos = m_currPlayerPos + glm::vec3(0, m_player.eyeHeight(), 0);
                m_editorCamera.yaw = m_lastCmd.yaw;
                m_editorCamera.pitch = m_lastCmd.pitch;
                m_window.setRelativeMouse(false); // editor UI needs the cursor
            } else {
                m_window.setRelativeMouse(true);
            }
        }
        if (m_input.pressed(GLFW_KEY_TAB)) {
            m_showBackpack = !m_showBackpack;
            m_audio.play(Sound::UiClick, 0.5f);
        }
        for (int i = 0; i < Inventory::kHotbar; ++i)
            if (m_input.pressed(GLFW_KEY_1 + i)) {
                m_selectedSlot = static_cast<std::uint8_t>(i);
                m_audio.play(Sound::UiClick, 0.4f);
            }
        if (const int scroll = m_input.consumeScrollSteps(); scroll != 0)
            m_selectedSlot = static_cast<std::uint8_t>(
                (m_selectedSlot + Inventory::kHotbar - (scroll % Inventory::kHotbar)) %
                Inventory::kHotbar);

        if (m_server) {
            m_server->pump(*m_serverTransport);
            m_beacon.update(m_server->playerCount(), 8);
        }
        m_client.pump(m_voxels, m_physics, m_player);
        if (m_clientWorldReady) syncWorldProps(); // server prop adds/removes → render + colliders
        if (!m_clientWorldReady && m_client.welcomed()) {
            // Host wins on world scale + environment (Welcome). Apply before any
            // generator/mesh/character runs so the mirror matches the server.
            const GameRules& hostRules = m_client.rules();
            const float vs = hostRules.voxelSize;
            if (vs != kVoxelSize) {
                kVoxelSize = vs;
                log::info("client: host voxelSize = {:.3f} m/voxel (chunk edge {:.1f} m)",
                          kVoxelSize, chunkWorldSize());
            }
            applyEnvironment(hostRules);
            setupClientWorld();
        }

        if (m_clientWorldReady) { // audio cues from authoritative state deltas
            const float hp = m_client.health();
            if (hp < m_prevHealth - 0.5f) m_audio.play(Sound::Hit, 0.7f);
            m_prevHealth = hp;
            std::size_t invHash = 0;
            for (int i = 0; i < Inventory::kSlots; ++i) {
                const ItemStack& s = m_client.inventory().slot(i);
                invHash = invHash * 1099511628211u ^ (s.id * 65537u + s.count);
            }
            if (m_prevInvHash != 0 && invHash != m_prevInvHash) m_audio.play(Sound::Pickup, 0.6f);
            m_prevInvHash = invHash;
        }

        const auto now = Clock::now();
        const double frameDt =
            std::min(std::chrono::duration<double>(now - last).count(), 0.25);
        last = now;
        accumulator += frameDt;

        PlayerCommand frameCmd = m_input.sampleCommand(m_tick); // per-frame: look stays fresh
        if (m_editorActive) { // player idles in place while the editor has input
            const float yaw = m_lastCmd.yaw, pitch = m_lastCmd.pitch;
            frameCmd = PlayerCommand{};
            frameCmd.yaw = yaw;
            frameCmd.pitch = pitch;
        }
        m_lastCmd = frameCmd;
        while (accumulator >= kFixedDt) {
            if (m_clientWorldReady) simulateClientTick(m_lastCmd);
            if (m_server) m_server->tick(*m_serverTransport);
            ++m_tick;
            accumulator -= kFixedDt;
        }

        if (config.startEditor && m_clientWorldReady && !m_editorActive && m_editor && m_server) {
            m_editorActive = true; // --editor: open the Room Designer on spawn
            m_editorCamera.pos = m_currPlayerPos + glm::vec3(0, m_player.eyeHeight(), 0);
            m_window.setRelativeMouse(false);
        }
        if (m_clientWorldReady) m_voxels.update(m_currPlayerPos, m_jobs);
        m_frameDt = static_cast<float>(frameDt);
        render(static_cast<float>(accumulator / kFixedDt));
        m_window.swap();

        // --shot: capture a 3-frame sequence (spaced ~0.9 s apart) so motion is
        // visible across the PNGs, then quit. name.png → name_0/_1/_2.png.
        if (!config.autoShot.empty() && m_clientWorldReady) {
            ++frameCount;
            const int shots[] = {150, 215, 280};
            for (int i = 0; i < 3; ++i) {
                if (frameCount == shots[i]) {
                    std::string p = config.autoShot;
                    const auto dot = p.rfind('.');
                    p.insert(dot == std::string::npos ? p.size() : dot,
                             "_" + std::to_string(i));
                    m_renderer.captureScreenshot(p);
                }
            }
            if (frameCount >= 281) break;
        }
    }

    stopHosting();
    if (m_server) {
        m_server->saveTo("saves/autosave.json"); // graceful exit = autosave
        saveEditorExtras();
    }

    if (m_imguiReady) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    return 0;
}

int Engine::runDedicated(const EngineConfig& config) {
    m_enetHost = std::make_unique<EnetServerTransport>();
    if (!m_enetHost->listen(config.port)) return 1;
    m_server = std::make_unique<ServerSim>();
    const bool booted = config.loadPath.empty() ? m_server->init(config.seed)
                                                : m_server->initFromSave(config.loadPath);
    if (!booted) return 1;
    startHosting(config);
    log::info("dedicated server '{}' on port {} (seed {})", config.serverName, config.port,
              config.seed);

    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();
    for (;;) { // terminated externally; graceful shutdown comes with the save system
        m_server->pump(*m_enetHost);
        m_server->tick(*m_enetHost);
        m_beacon.update(m_server->playerCount(), 8);
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
}

} // namespace meat
