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
FetchContent_Declare(imguizmo_src
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo
    GIT_TAG master GIT_SHALLOW ON)

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
                           imgui_src imguizmo_src stb_src miniaudio_src nlohmann_json)

# glad: generate a GL 4.5 core loader target
add_subdirectory(${glad_SOURCE_DIR}/cmake ${glad_BINARY_DIR}/cmake)
glad_add_library(glad_gl_core_45 REPRODUCIBLE API gl:core=4.5)

# Lua static lib from upstream sources (lua.c = CLI, onelua.c = amalgam; excluded)
file(GLOB LUA_SOURCES ${lua_src_SOURCE_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac|onelua)\\.c$")
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC ${lua_src_SOURCE_DIR})

# ImGui + ImGuizmo as one static lib (GLFW + GL3 backends)
add_library(imgui STATIC
    ${imgui_src_SOURCE_DIR}/imgui.cpp
    ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_src_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imguizmo_src_SOURCE_DIR}/ImGuizmo.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_src_SOURCE_DIR} ${imgui_src_SOURCE_DIR}/backends ${imguizmo_src_SOURCE_DIR})
target_link_libraries(imgui PUBLIC glfw)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_src_SOURCE_DIR})
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE ${miniaudio_src_SOURCE_DIR})
