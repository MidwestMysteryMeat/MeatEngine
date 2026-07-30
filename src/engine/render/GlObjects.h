#pragma once
#include "engine/core/Log.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <string_view>
#include <utility>

// Move-only RAII wrappers over raw GL names. All of these require a current GL
// context (main thread only). Objects are created empty; call create() once the
// context exists — Renderer members are constructed before Renderer::init().
namespace meat {

class GlBuffer {
public:
    GlBuffer() = default;
    ~GlBuffer() { reset(); }
    GlBuffer(const GlBuffer&) = delete;
    GlBuffer& operator=(const GlBuffer&) = delete;
    GlBuffer(GlBuffer&& other) noexcept : m_id(std::exchange(other.m_id, 0u)) {}
    GlBuffer& operator=(GlBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            m_id = std::exchange(other.m_id, 0u);
        }
        return *this;
    }
    void create() {
        reset();
        glCreateBuffers(1, &m_id);
    }
    void reset() {
        if (m_id != 0u) {
            glDeleteBuffers(1, &m_id);
            m_id = 0u;
        }
    }
    GLuint id() const { return m_id; }
    explicit operator bool() const { return m_id != 0u; }

private:
    GLuint m_id = 0u;
};

class GlVertexArray {
public:
    GlVertexArray() = default;
    ~GlVertexArray() { reset(); }
    GlVertexArray(const GlVertexArray&) = delete;
    GlVertexArray& operator=(const GlVertexArray&) = delete;
    GlVertexArray(GlVertexArray&& other) noexcept : m_id(std::exchange(other.m_id, 0u)) {}
    GlVertexArray& operator=(GlVertexArray&& other) noexcept {
        if (this != &other) {
            reset();
            m_id = std::exchange(other.m_id, 0u);
        }
        return *this;
    }
    void create() {
        reset();
        glCreateVertexArrays(1, &m_id);
    }
    void reset() {
        if (m_id != 0u) {
            glDeleteVertexArrays(1, &m_id);
            m_id = 0u;
        }
    }
    GLuint id() const { return m_id; }
    explicit operator bool() const { return m_id != 0u; }

private:
    GLuint m_id = 0u;
};

class GlTexture {
public:
    GlTexture() = default;
    ~GlTexture() { reset(); }
    GlTexture(const GlTexture&) = delete;
    GlTexture& operator=(const GlTexture&) = delete;
    GlTexture(GlTexture&& other) noexcept : m_id(std::exchange(other.m_id, 0u)) {}
    GlTexture& operator=(GlTexture&& other) noexcept {
        if (this != &other) {
            reset();
            m_id = std::exchange(other.m_id, 0u);
        }
        return *this;
    }
    void create(GLenum target) {
        reset();
        glCreateTextures(target, 1, &m_id);
    }
    void reset() {
        if (m_id != 0u) {
            glDeleteTextures(1, &m_id);
            m_id = 0u;
        }
    }
    GLuint id() const { return m_id; }
    explicit operator bool() const { return m_id != 0u; }

private:
    GLuint m_id = 0u;
};

class GlFramebuffer {
public:
    GlFramebuffer() = default;
    ~GlFramebuffer() { reset(); }
    GlFramebuffer(const GlFramebuffer&) = delete;
    GlFramebuffer& operator=(const GlFramebuffer&) = delete;
    GlFramebuffer(GlFramebuffer&& other) noexcept : m_id(std::exchange(other.m_id, 0u)) {}
    GlFramebuffer& operator=(GlFramebuffer&& other) noexcept {
        if (this != &other) {
            reset();
            m_id = std::exchange(other.m_id, 0u);
        }
        return *this;
    }
    void create() {
        reset();
        glCreateFramebuffers(1, &m_id);
    }
    void reset() {
        if (m_id != 0u) {
            glDeleteFramebuffers(1, &m_id);
            m_id = 0u;
        }
    }
    GLuint id() const { return m_id; }
    explicit operator bool() const { return m_id != 0u; }

private:
    GLuint m_id = 0u;
};

class GlShaderProgram {
public:
    GlShaderProgram() = default;
    ~GlShaderProgram() { reset(); }
    GlShaderProgram(const GlShaderProgram&) = delete;
    GlShaderProgram& operator=(const GlShaderProgram&) = delete;
    GlShaderProgram(GlShaderProgram&& other) noexcept : m_id(std::exchange(other.m_id, 0u)) {}
    GlShaderProgram& operator=(GlShaderProgram&& other) noexcept {
        if (this != &other) {
            reset();
            m_id = std::exchange(other.m_id, 0u);
        }
        return *this;
    }

    // Compiles and links a vert+frag pair. On failure the previous program (if
    // any) is kept intact so hot reload can fall back to the last good build.
    bool compile(std::string_view vertSrc, std::string_view fragSrc, std::string_view debugName) {
        const GLuint vert = compileStage(GL_VERTEX_SHADER, vertSrc, debugName);
        if (vert == 0u) {
            return false;
        }
        const GLuint frag = compileStage(GL_FRAGMENT_SHADER, fragSrc, debugName);
        if (frag == 0u) {
            glDeleteShader(vert);
            return false;
        }
        const GLuint program = glCreateProgram();
        glAttachShader(program, vert);
        glAttachShader(program, frag);
        glLinkProgram(program);
        glDeleteShader(vert);
        glDeleteShader(frag);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            char buf[2048];
            GLsizei len = 0;
            glGetProgramInfoLog(program, static_cast<GLsizei>(sizeof(buf)), &len, buf);
            log::error("shader link failed ({}): {}", debugName,
                       std::string_view(buf, static_cast<std::size_t>(len)));
            glDeleteProgram(program);
            return false;
        }
        reset();
        m_id = program;
        return true;
    }

    void reset() {
        if (m_id != 0u) {
            glDeleteProgram(m_id);
            m_id = 0u;
        }
    }
    GLuint id() const { return m_id; }
    explicit operator bool() const { return m_id != 0u; }

    // glProgramUniform* so no glUseProgram is needed; unknown names resolve to
    // location -1, which GL silently ignores.
    void setUniform(const char* name, int v) const { glProgramUniform1i(m_id, loc(name), v); }
    void setUniform(const char* name, float v) const { glProgramUniform1f(m_id, loc(name), v); }
    void setUniform(const char* name, const glm::vec2& v) const {
        glProgramUniform2fv(m_id, loc(name), 1, glm::value_ptr(v));
    }
    void setUniform(const char* name, const glm::vec3& v) const {
        glProgramUniform3fv(m_id, loc(name), 1, glm::value_ptr(v));
    }
    void setUniform(const char* name, const glm::vec4& v) const {
        glProgramUniform4fv(m_id, loc(name), 1, glm::value_ptr(v));
    }
    void setUniform(const char* name, const glm::mat4& v) const {
        glProgramUniformMatrix4fv(m_id, loc(name), 1, GL_FALSE, glm::value_ptr(v));
    }

private:
    GLint loc(const char* name) const { return glGetUniformLocation(m_id, name); }

    static GLuint compileStage(GLenum type, std::string_view src, std::string_view debugName) {
        const GLuint shader = glCreateShader(type);
        const GLchar* text = src.data();
        const GLint length = static_cast<GLint>(src.size());
        glShaderSource(shader, 1, &text, &length);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char buf[2048];
            GLsizei len = 0;
            glGetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(buf)), &len, buf);
            log::error("{} compile failed ({}): {}",
                       type == GL_VERTEX_SHADER ? "vertex shader" : "fragment shader", debugName,
                       std::string_view(buf, static_cast<std::size_t>(len)));
            glDeleteShader(shader);
            return 0u;
        }
        return shader;
    }

    GLuint m_id = 0u;
};

} // namespace meat
