// Textures.h - texture creation.
// Every texture is generated procedurally on the CPU (value noise + fractal Brownian motion),
// uploaded with glTexImage2D, and mip-mapped.  If a PNG with the same name exists in
// assets/textures/ it is loaded instead - so you can drop in real photos of your campus.
//
//   facade.png  one window cell of a building wall   (alpha: 0 = plaster, ~0.6 = frame, 1 = glass)
//   grass.png   ground        asphalt.png   roads      paving.png   footpaths      roof.png   roofs
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "stb_image.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE          // core in GL 4.6, extension before that
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif

namespace tex {

// ---------------------------------------------------------------- noise
inline float hash(int x, int y, int seed) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffff) / float(0xffffff);
}
// smooth value noise that tiles with the given period
inline float noise(float x, float y, int period, int seed) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    auto h = [&](int a, int b) { return hash(((a % period) + period) % period, ((b % period) + period) % period, seed); };
    float a = h(xi, yi), b = h(xi + 1, yi), c = h(xi, yi + 1), d = h(xi + 1, yi + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
// fractal Brownian motion: several octaves of noise added together
inline float fbm(float u, float v, int baseFreq, int octaves, int seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    int f = baseFreq;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * noise(u * f, v * f, f, seed + o);
        norm += amp; amp *= 0.5f; f *= 2;
    }
    return sum / norm;
}

struct Image {
    int w, h;
    std::vector<unsigned char> px;   // RGBA8
    Image(int W, int H) : w(W), h(H), px(W * H * 4) {}
    void set(int x, int y, glm::vec4 c) {
        c = glm::clamp(c, 0.0f, 1.0f);
        unsigned char* p = &px[(y * w + x) * 4];
        p[0] = (unsigned char)(c.r * 255); p[1] = (unsigned char)(c.g * 255);
        p[2] = (unsigned char)(c.b * 255); p[3] = (unsigned char)(c.a * 255);
    }
};

// ---------------------------------------------------------------- generators
inline Image facade() {
    const int N = 256;
    Image im(N, N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N;       // v = 0 at the floor
            float n = fbm(u, v, 8, 4, 11);
            glm::vec4 c(glm::vec3(0.88f + 0.12f * n), 0.0f);       // plaster (tinted by the building colour)
            if (v < 0.05f) c = glm::vec4(glm::vec3(0.70f + 0.1f * n), 0);            // floor slab band
            bool inWin = u > 0.20f && u < 0.80f && v > 0.30f && v < 0.835f;
            bool inGlass = u > 0.23f && u < 0.77f && v > 0.33f && v < 0.805f && std::abs(u - 0.5f) > 0.012f;
            if (v > 0.25f && v < 0.30f && u > 0.17f && u < 0.83f)               // window sill
                c = glm::vec4(glm::vec3(0.95f), 0.0f);
            // concrete sunshade ("chajja") over each window: a light ledge with its shadow under it
            if (v > 0.86f && v < 0.90f && u > 0.14f && u < 0.86f) c = glm::vec4(glm::vec3(0.98f), 0.0f);
            if (v > 0.84f && v <= 0.86f && u > 0.16f && u < 0.84f) c = glm::vec4(glm::vec3(0.55f + 0.05f * n), 0.0f);
            if (inWin) c = glm::vec4(0.22f, 0.22f, 0.24f, 0.6f);                 // dark frame
            if (inGlass) {
                float g = 0.25f + 0.35f * (v - 0.33f) / 0.48f;                     // sky reflected brighter at top
                float streak = 0.08f * std::max(0.0f, 1.0f - std::abs((u - v * 0.6f) - 0.05f) * 12.0f);
                c = glm::vec4(0.30f * g + streak, 0.40f * g + streak, 0.52f * g + streak, 1.0f);
            }
            im.set(x, y, c);
        }
    return im;
}

inline Image grass() {
    const int N = 512;
    Image im(N, N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (float)x / N, v = (float)y / N;
            float big = fbm(u, v, 4, 4, 21), blade = hash(x, y, 5);
            glm::vec3 dry(0.50f, 0.50f, 0.30f), lush(0.30f, 0.48f, 0.22f);
            glm::vec3 c = glm::mix(lush, dry, glm::smoothstep(0.45f, 0.75f, big));
            c *= 0.88f + 0.24f * blade;
            im.set(x, y, glm::vec4(c, 1));
        }
    return im;
}

inline Image asphalt() {
    const int N = 256;
    Image im(N, N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (float)x / N, v = (float)y / N;
            float n = fbm(u, v, 8, 4, 31), grain = hash(x, y, 7);
            float g = 0.85f + 0.2f * n + (grain > 0.93f ? 0.25f : 0.0f) - (grain < 0.05f ? 0.2f : 0.0f);
            im.set(x, y, glm::vec4(glm::vec3(g), 1));
        }
    return im;
}

inline Image paving() {
    const int N = 256;
    Image im(N, N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (float)x / N * 4, v = (float)y / N * 4;      // 4 x 4 tiles
            int row = (int)v;
            float uu = u + (row % 2) * 0.5f;                       // running bond pattern
            float fu = uu - std::floor(uu), fv = v - std::floor(v);
            float grout = (fu < 0.04f || fv < 0.06f) ? 0.65f : 1.0f;
            float tileShade = 0.9f + 0.2f * hash((int)std::floor(uu), row, 3);
            float n = fbm((float)x / N, (float)y / N, 16, 3, 41);
            im.set(x, y, glm::vec4(glm::vec3(grout * tileShade * (0.9f + 0.2f * n)), 1));
        }
    return im;
}

inline Image roof() {
    const int N = 256;
    Image im(N, N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (float)x / N, v = (float)y / N;
            float stain = fbm(u, v, 3, 5, 51), n = hash(x, y, 9);
            float g = 0.95f - 0.35f * glm::smoothstep(0.55f, 0.85f, stain) + 0.08f * n;
            im.set(x, y, glm::vec4(glm::vec3(g), 1));
        }
    return im;
}

// ---------------------------------------------------------------- upload
inline GLuint upload(const unsigned char* px, int w, int h) {
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);                                    // mip-mapping
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);   // trilinear
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLfloat maxAniso = 0;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    if (glGetError() == GL_NO_ERROR && maxAniso > 1)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, std::min(8.0f, maxAniso));
    glGetError();
    return id;
}

// Load assets/textures/<name>.png if present, otherwise use the generated image.
inline GLuint loadOrGenerate(const std::string& dir, const std::string& name, Image (*gen)()) {
    std::string path = dir + name + ".png";
    int w, h, n;
    stbi_set_flip_vertically_on_load(1);
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (data) {
        std::printf("Texture %s loaded from %s\n", name.c_str(), path.c_str());
        GLuint id = upload(data, w, h);
        stbi_image_free(data);
        return id;
    }
    Image im = gen();
    return upload(im.px.data(), im.w, im.h);
}

struct Set {
    GLuint facade = 0, grass = 0, asphalt = 0, paving = 0, roof = 0;
    void create(const std::string& dir) {
        facade = loadOrGenerate(dir, "facade", tex::facade);
        grass = loadOrGenerate(dir, "grass", tex::grass);
        asphalt = loadOrGenerate(dir, "asphalt", tex::asphalt);
        paving = loadOrGenerate(dir, "paving", tex::paving);
        roof = loadOrGenerate(dir, "roof", tex::roof);
    }
};

} // namespace tex
