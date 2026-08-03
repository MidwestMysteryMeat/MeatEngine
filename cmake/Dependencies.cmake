# All third-party dependencies, pinned. First configure downloads and builds
# everything (Assimp + Jolt dominate; expect 10-20 minutes once).
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- GLFW -------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw
    GIT_TAG 3.4 GIT_SHALLOW ON)

# ---- glad (GL 4.5 core loader, generated at configure; needs python+jinja2) --
FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad
    GIT_TAG v2.0.8 GIT_SHALLOW ON)

# ---- GLM --------------------------------------------------------------------
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm
    GIT_TAG 1.0.1 GIT_SHALLOW ON)

# ---- Assimp (FBX/OBJ/glTF import only, no export, no tools) -----------------
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp
    GIT_TAG v5.4.3 GIT_SHALLOW ON)

# ---- Jolt Physics -----------------------------------------------------------
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)  # match /MD used project-wide
FetchContent_Declare(joltphysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics
    GIT_TAG v5.2.0 GIT_SHALLOW ON
    SOURCE_SUBDIR Build)

# ---- Lua 5.4 (built from source as a static lib) ----------------------------
FetchContent_Declare(lua_src
    GIT_REPOSITORY https://github.com/lua/lua
    GIT_TAG v5.4.7 GIT_SHALLOW ON)

# ---- sol2 -------------------------------------------------------------------
set(SOL2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2
    GIT_TAG v3.3.0 GIT_SHALLOW ON)

# ---- Dear ImGui (docking) + ImGuizmo ----------------------------------------
FetchContent_Declare(imgui_src
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG docking GIT_SHALLOW ON)
# SOURCE_SUBDIR points at a nonexistent dir so MakeAvailable skips ImGuizmo's own
# CMakeLists (it builds a duplicate target without imgui includes); we compile
# ImGuizmo.cpp into our imgui target below instead.
FetchContent_Declare(imguizmo_src
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo
    GIT_TAG master GIT_SHALLOW ON
    SOURCE_SUBDIR _skip_own_cmake)
# imnodes (MIT): node-graph editor for C6 visual scripting / blueprints.
# No CMakeLists of its own we care about — compile into the imgui static lib.
FetchContent_Declare(imnodes_src
    GIT_REPOSITORY https://github.com/Nelarius/imnodes
    GIT_TAG master GIT_SHALLOW ON
    SOURCE_SUBDIR _skip_own_cmake)

# ---- ENet (UDP transport) ---------------------------------------------------
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet
    GIT_TAG v1.3.18 GIT_SHALLOW ON)

# ---- Recast/Detour (navmesh: rasterize collision meshes → Detour) -----------
# zlib-licensed. We link only Recast (rasterize/build) + Detour (navmesh query);
# the demo/tests/examples pull SDL/extra deps we don't want, so force them OFF.
# Static libs follow the parent's default (BUILD_SHARED_LIBS unset = static).
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation
    GIT_TAG v1.6.0 GIT_SHALLOW ON)

# ---- ozz-animation (MIT): runtime skeletal animation core -------------------
# Sampling/blending/IK jobs + offline RawSkeleton/RawAnimation builders (we convert
# Assimp-loaded rigs to ozz in-memory at load). Tools/samples/tests/fbx pull deps we
# don't want (fbx SDK, jpeg, ...) — force them all OFF; build just the libraries.
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_simd_ref OFF CACHE BOOL "" FORCE)
set(ozz_build_msvc_rt_dll ON CACHE BOOL "" FORCE) # match /MD project-wide
FetchContent_Declare(ozz
    GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation
    GIT_TAG 0.16.0 GIT_SHALLOW ON)

# ---- enkiTS (zlib): task scheduler for parallel chunk meshing / worldgen ----
set(ENKITS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(enkits
    GIT_REPOSITORY https://github.com/dougbinks/enkiTS
    GIT_TAG v1.11 GIT_SHALLOW ON)

# ---- EnTT (MIT): sparse-set ECS, adopted incrementally behind the registry --
FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt
    GIT_TAG v3.13.2 GIT_SHALLOW ON)

# ---- bitsery (MIT): bit-packing/quantization for delta snapshots ------------
set(BITSERY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BITSERY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(bitsery
    GIT_REPOSITORY https://github.com/fraillt/bitsery
    GIT_TAG v5.2.4 GIT_SHALLOW ON)

# ---- FastNoiseLite (MIT): single-header noise for worldgen ------------------
FetchContent_Declare(fastnoiselite
    GIT_REPOSITORY https://github.com/Auburn/FastNoiseLite
    GIT_TAG v1.1.1 GIT_SHALLOW ON)

# ---- Header-only: stb, miniaudio, json --------------------------------------
FetchContent_Declare(stb_src
    GIT_REPOSITORY https://github.com/nothings/stb
    GIT_TAG master GIT_SHALLOW ON)
FetchContent_Declare(miniaudio_src
    GIT_REPOSITORY https://github.com/mackron/miniaudio
    GIT_TAG 0.11.21 GIT_SHALLOW ON)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.11.3 GIT_SHALLOW ON)

FetchContent_MakeAvailable(glfw glad glm assimp joltphysics lua_src sol2
                           imgui_src imguizmo_src imnodes_src enet stb_src miniaudio_src nlohmann_json
                           recastnavigation ozz enkits entt bitsery)

# FastNoiseLite is a single header; skip its root CMake (non-lib targets) and expose the dir.
FetchContent_GetProperties(fastnoiselite)
if(NOT fastnoiselite_POPULATED)
    FetchContent_Populate(fastnoiselite)
endif()
add_library(fastnoiselite INTERFACE)
target_include_directories(fastnoiselite INTERFACE ${fastnoiselite_SOURCE_DIR}/Cpp)

# ENet's CMakeLists predates usage-requirement style; export its include dir.
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()
# Silence ENet's own compiler warnings (e.g. MSVC C5287 enum-mismatch) — it is
# vendored code we don't edit, and its noise buries real warnings from our /W4
# targets. Our own MeatEngine/MeatTests keep full warnings.
if(MSVC)
    target_compile_options(enet PRIVATE /w)
else()
    target_compile_options(enet PRIVATE -w)
endif()

# glad: generate a GL 4.5 core loader target
add_subdirectory(${glad_SOURCE_DIR}/cmake ${glad_BINARY_DIR}/cmake)
glad_add_library(glad_gl_core_45 REPRODUCIBLE API gl:core=4.5)

# Lua static lib from upstream sources (lua.c = CLI, onelua.c = amalgam; excluded)
file(GLOB LUA_SOURCES ${lua_src_SOURCE_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac|onelua)\\.c$")
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC ${lua_src_SOURCE_DIR})

# ImGui + ImGuizmo + imnodes as one static lib (GLFW + GL3 backends)
add_library(imgui STATIC
    ${imgui_src_SOURCE_DIR}/imgui.cpp
    ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_src_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imguizmo_src_SOURCE_DIR}/src/ImGuizmo.cpp
    ${imnodes_src_SOURCE_DIR}/imnodes.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_src_SOURCE_DIR} ${imgui_src_SOURCE_DIR}/backends ${imguizmo_src_SOURCE_DIR}/src
    ${imnodes_src_SOURCE_DIR})
target_link_libraries(imgui PUBLIC glfw)
# imnodes_internal.h defines IMGUI_DEFINE_MATH_OPERATORS itself when compiling imnodes.cpp.

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_src_SOURCE_DIR})
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE ${miniaudio_src_SOURCE_DIR})
