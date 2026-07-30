#include "engine/render/Shader.h"

#include "engine/core/Log.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace meat {

bool Shader::load(std::filesystem::path directory, std::string name) {
    m_directory = std::move(directory);
    m_name = std::move(name);
    return reload();
}

bool Shader::reload() {
    const std::filesystem::path vertPath = m_directory / (m_name + ".vert");
    const std::filesystem::path fragPath = m_directory / (m_name + ".frag");

    std::string vertSrc;
    std::string fragSrc;
    if (!readFile(vertPath, vertSrc)) {
        log::error("shader '{}': cannot read {}", m_name, vertPath.string());
        return false;
    }
    if (!readFile(fragPath, fragSrc)) {
        log::error("shader '{}': cannot read {}", m_name, fragPath.string());
        return false;
    }

    // GlShaderProgram::compile keeps the previous program on failure, which is
    // exactly the hot-reload fallback we want; compile errors are logged there.
    if (!m_program.compile(vertSrc, fragSrc, m_name)) {
        log::error("shader '{}': reload failed, keeping previous program", m_name);
        return false;
    }
    log::info("shader '{}' compiled", m_name);
    return true;
}

bool Shader::readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    out = stream.str();
    return true;
}

} // namespace meat
