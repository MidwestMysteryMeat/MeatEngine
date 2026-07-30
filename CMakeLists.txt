cmake_minimum_required(VERSION 3.28)
project(MeatEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

include(cmake/Dependencies.cmake)

# CONFIGURE_DEPENDS keeps the file list current without hand-editing while the
# codebase is growing fast; revisit once the module set stabilizes.
file(GLOB_RECURSE MEAT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_executable(MeatEngine ${MEAT_SOURCES})

target_include_directories(MeatEngine PRIVATE src)
target_compile_definitions(MeatEngine PRIVATE GLFW_INCLUDE_NONE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(MeatEngine PRIVATE
    glfw
    glad_gl_core_45
    glm::glm
    assimp::assimp
    Jolt
    lua_static
    sol2::sol2
    imgui
    stb
    miniaudio
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(MeatEngine PRIVATE /W4 /permissive- /MP /utf-8)
else()
    target_compile_options(MeatEngine PRIVATE -Wall -Wextra)
endif()
