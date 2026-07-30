#pragma once
#include "engine/render/GlObjects.h"

#include <filesystem>
#include <string>

namespace meat {

// A {name}.vert / {name}.frag pair loaded from disk. reload() recompiles from
// the same paths (F6 hot reload); on failure the last good program stays live.
class Shader {
public:
    bool load(std::filesystem::path directory, std::string name);
    bool reload();

    GLuint id() const { return m_program.id(); }
    const GlShaderProgram& program() const { return m_program; }
    explicit operator bool() const { return static_cast<bool>(m_program); }

private:
    static bool readFile(const std::filesystem::path& path, std::string& out);

    std::filesystem::path m_directory;
    std::string m_name;
    GlShaderProgram m_program;
};

} // namespace meat
