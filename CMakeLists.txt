cmake_minimum_required(VERSION 3.28)
project(MeatEngine VERSION 0.1.0 LANGUAGES C CXX)

# Several vendored deps (RecastNavigation, glm, enkiTS) declare an ancient
# cmake_minimum_required(<3.5), which CMake 4.x rejects as a hard error rather
# than a warning. Floor those old policy requests at 3.5 so the tree configures
# on modern CMake (the CI runners and any evaluator's toolchain) as well as older
# ones. Introduced in CMake 3.31; ignored (harmless) on earlier versions.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

include(cmake/Dependencies.cmake)

# CONFIGURE_DEPENDS keeps the file list current without hand-editing while the
# codebase is growing fast; revisit once the module set stabilizes.
file(GLOB_RECURSE MEAT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_executable(MeatEngine ${MEAT_SOURCES})

# Shared by the app and the test binary, so a link change cannot apply to one
# and not the other.
set(MEAT_LIBS
    glfw
    glad_gl_core_45
    glm::glm
    assimp::assimp
    Jolt
    lua_static
    sol2::sol2
    imgui
    enet
    stb
    miniaudio
    nlohmann_json::nlohmann_json
    Recast
    Detour
    ozz_base
    ozz_animation
    ozz_animation_offline
    ozz_geometry
    enkiTS
    EnTT::EnTT
    Bitsery::bitsery
    fastnoiselite
    monocypher
)

target_include_directories(MeatEngine PRIVATE src)
target_compile_definitions(MeatEngine PRIVATE GLFW_INCLUDE_NONE)
if(WIN32)
    # Win32-only header hygiene; harmless-but-meaningless elsewhere.
    target_compile_definitions(MeatEngine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_link_libraries(MeatEngine PRIVATE ${MEAT_LIBS})

if(MSVC)
    target_compile_options(MeatEngine PRIVATE /W4 /permissive- /MP /utf-8)
else()
    target_compile_options(MeatEngine PRIVATE -Wall -Wextra)
endif()

# On Linux the app needs pthread (std::thread / GLFW / Jolt) and libdl (glad's
# runtime GL loader via glfwGetProcAddress). GL, X11/Wayland and libm come in
# transitively through the GLFW static-lib target's link interface.
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(MeatEngine PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Headless tests
#
# Same translation units as the app minus its main(), so the tests exercise the
# shipping code rather than a copy of it. Nothing here opens a window or a
# socket: ServerSim is driven over LoopbackPair, which is why these can run on a
# CI box with no GPU.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MEAT_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
if(MEAT_TEST_SOURCES)
    set(MEAT_ENGINE_SOURCES ${MEAT_SOURCES})
    list(REMOVE_ITEM MEAT_ENGINE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

    add_executable(MeatTests ${MEAT_ENGINE_SOURCES} ${MEAT_TEST_SOURCES})
    target_include_directories(MeatTests PRIVATE src)
    target_compile_definitions(MeatTests PRIVATE GLFW_INCLUDE_NONE)
    if(WIN32)
        target_compile_definitions(MeatTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    target_link_libraries(MeatTests PRIVATE ${MEAT_LIBS})
    if(MSVC)
        target_compile_options(MeatTests PRIVATE /W4 /permissive- /MP /utf-8)
    else()
        target_compile_options(MeatTests PRIVATE -Wall -Wextra)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(MeatTests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    enable_testing()
    # Run from the repository root: the server loads assets by relative path.
    add_test(NAME headless COMMAND MeatTests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# AddressSanitizer + UndefinedBehaviorSanitizer on OUR targets only (deps stay
# uninstrumented — ASan's malloc interceptor still catches heap errors anywhere,
# and UBSan covers our code). CI runs the test suite under this. Would have
# caught the abs(INT_MIN) out-of-bounds write immediately. Clang/GCC only.
option(MEAT_SANITIZE "Build engine + tests with ASan+UBSan" OFF)
if(MEAT_SANITIZE AND NOT MSVC)
    # -fno-sanitize=vptr: UBSan's vtable-pointer check needs RTTI/typeinfo for
    # polymorphic types, but Jolt is built without RTTI, so instrumenting our
    # code that touches JPH::Shape/CharacterVirtual emits undefined references to
    # their typeinfo. Drop just that check; ASan + the rest of UBSan stay on.
    set(_meat_san -fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    foreach(_t MeatEngine MeatTests)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE ${_meat_san})
            target_link_options(${_t} PRIVATE ${_meat_san})
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Install + package. Everything ships under a single `runtime` COMPONENT so the
# ~20 FetchContent deps' own install rules (headers/libs) never leak into the
# game package. `cmake --install build --component runtime --prefix <dir>` lays
# out a runnable tree (exe + the assets it loads by relative path + the Apache
# LICENSE/NOTICE); `cpack` zips exactly that.
# ---------------------------------------------------------------------------
install(TARGETS MeatEngine RUNTIME DESTINATION . COMPONENT runtime)
install(DIRECTORY assets DESTINATION . COMPONENT runtime)
install(FILES LICENSE NOTICE README.md DESTINATION . COMPONENT runtime)

set(CPACK_PACKAGE_NAME MeatEngine)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "MeatEngine-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PSX-styled multiplayer voxel FPS engine")
set(CPACK_GENERATOR "ZIP")               # cross-platform; one artifact shape everywhere
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)  # package by component...
set(CPACK_COMPONENTS_ALL runtime)        # ...and only the game runtime, not deps
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_ARCHIVE_RUNTIME_FILE_NAME "MeatEngine-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_VERBATIM_VARIABLES ON)
include(CPack)
