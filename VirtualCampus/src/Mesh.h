// Mesh.h - VAO/VBO wrapper, a vertex builder, and primitive shape generators.
// Vertex layout (12 floats):
//   location 0: position (3)   location 1: normal (3)   location 2: uv (2)
//   location 3: colour rgb + id (4)   - id = building index + 1 (0 = not a building),
//                                       used to highlight the building under the cursor
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <cstddef>
#include <utility>
#include <vector>

struct Vertex {
    glm::vec3 pos, normal;
    glm::vec2 uv;
    glm::vec4 color;   // rgb + id
};

struct Mesh {
    GLuint vao = 0, vbo = 0;
    GLsizei count = 0;

    void upload(const std::vector<Vertex>& v) {
        count = (GLsizei)v.size();
        if (!vao) { glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); }
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(Vertex), v.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
        for (int i = 0; i < 4; ++i) glEnableVertexAttribArray(i);
        glBindVertexArray(0);
    }

    void draw() const {
        if (!count) return;
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, count);
    }
};

// Collects triangles; makes sure each triangle's winding matches its normal
// (so back-face culling keeps the visible side).
struct MeshBuilder {
    std::vector<Vertex> v;

    void tri(Vertex a, Vertex b, Vertex c) {
        glm::vec3 n = glm::cross(b.pos - a.pos, c.pos - a.pos);
        if (glm::dot(n, a.normal) < 0) std::swap(b, c);
        v.push_back(a); v.push_back(b); v.push_back(c);
    }
    // quad a-b-c-d (any winding)
    void quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
        tri(a, b, c); tri(a, c, d);
    }
};

// Unit cube centred on x/z, sitting on y = 0 (so scaling y = building height).
inline Mesh makeCube() {
    struct Face { float n[3]; float c[4][3]; };
    const Face faces[6] = {
        {{ 0, 0, 1}, {{-.5f,0, .5f},{ .5f,0, .5f},{ .5f,1, .5f},{-.5f,1, .5f}}},
        {{ 0, 0,-1}, {{ .5f,0,-.5f},{-.5f,0,-.5f},{-.5f,1,-.5f},{ .5f,1,-.5f}}},
        {{ 1, 0, 0}, {{ .5f,0, .5f},{ .5f,0,-.5f},{ .5f,1,-.5f},{ .5f,1, .5f}}},
        {{-1, 0, 0}, {{-.5f,0,-.5f},{-.5f,0, .5f},{-.5f,1, .5f},{-.5f,1,-.5f}}},
        {{ 0, 1, 0}, {{-.5f,1, .5f},{ .5f,1, .5f},{ .5f,1,-.5f},{-.5f,1,-.5f}}},
        {{ 0,-1, 0}, {{-.5f,0,-.5f},{ .5f,0,-.5f},{ .5f,0, .5f},{-.5f,0, .5f}}},
    };
    const glm::vec2 uv[4] = {{0,0},{1,0},{1,1},{0,1}};
    MeshBuilder mb;
    for (const Face& f : faces) {
        Vertex q[4];
        for (int i = 0; i < 4; ++i)
            q[i] = {{f.c[i][0], f.c[i][1], f.c[i][2]}, {f.n[0], f.n[1], f.n[2]}, uv[i], {1, 1, 1, 0}};
        mb.quad(q[0], q[1], q[2], q[3]);
    }
    Mesh m; m.upload(mb.v); return m;
}

// Unit square on the XZ plane (y = 0), facing up.
inline Mesh makePlane() {
    MeshBuilder mb;
    glm::vec3 n(0, 1, 0); glm::vec4 c(1, 1, 1, 0);
    mb.quad({{-.5f,0, .5f}, n, {0,0}, c}, {{.5f,0, .5f}, n, {1,0}, c},
            {{ .5f,0,-.5f}, n, {1,1}, c}, {{-.5f,0,-.5f}, n, {0,1}, c});
    Mesh m; m.upload(mb.v); return m;
}
