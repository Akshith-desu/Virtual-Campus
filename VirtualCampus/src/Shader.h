// Shader.h - loads, compiles and links a GLSL vertex + fragment shader pair.
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>

class Shader {
public:
    GLuint id = 0;

    Shader() = default;
    Shader(const std::string& vertPath, const std::string& fragPath) {
        std::string vs = readFile(vertPath), fs = readFile(fragPath);
        GLuint v = compile(GL_VERTEX_SHADER, vs, vertPath);
        GLuint f = compile(GL_FRAGMENT_SHADER, fs, fragPath);
        id = glCreateProgram();
        glAttachShader(id, v);
        glAttachShader(id, f);
        glLinkProgram(id);
        GLint ok; glGetProgramiv(id, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024]; glGetProgramInfoLog(id, 1024, nullptr, log);
            std::cerr << "Shader link error:\n" << log << "\n";
        }
        glDeleteShader(v);
        glDeleteShader(f);
    }

    void use() const { glUseProgram(id); }

    void set(const char* n, int v)              const { glUniform1i(loc(n), v); }
    void set(const char* n, float v)            const { glUniform1f(loc(n), v); }
    void set(const char* n, const glm::vec2& v) const { glUniform2fv(loc(n), 1, glm::value_ptr(v)); }
    void set(const char* n, const glm::vec3& v) const { glUniform3fv(loc(n), 1, glm::value_ptr(v)); }
    void set(const char* n, const glm::mat4& m) const { glUniformMatrix4fv(loc(n), 1, GL_FALSE, glm::value_ptr(m)); }

private:
    GLint loc(const char* n) const { return glGetUniformLocation(id, n); }

    static std::string readFile(const std::string& path) {
        std::ifstream f(path);
        if (!f) { std::cerr << "Cannot open " << path << "\n"; return ""; }
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    }

    static GLuint compile(GLenum type, const std::string& src, const std::string& name) {
        GLuint s = glCreateShader(type);
        const char* c = src.c_str();
        glShaderSource(s, 1, &c, nullptr);
        glCompileShader(s);
        GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log);
            std::cerr << "Shader compile error in " << name << ":\n" << log << "\n";
        }
        return s;
    }
};
