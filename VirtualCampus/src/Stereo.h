// Stereo.h - stereoscopic (VR) rendering.
//
// Each eye gets its own camera, offset sideways by half the eye separation, with PARALLEL
// view directions and an asymmetric ("off-axis") frustum, so both eyes converge on the
// same screen plane (the convergence distance) without the vertical parallax that "toe-in"
// cameras cause.  Each eye is rendered into an off-screen framebuffer (4x MSAA, then
// resolved into a texture), and a final full-screen pass combines the two images:
//
//   ANAGLYPH      red/cyan glasses, Dubois least-squares colour matrices (less ghosting)
//   CARDBOARD     left eye | right eye + barrel lens distortion and a vignette, for a phone in a
//                 VR viewer (see PhoneVR.h, which streams this image to the phone)
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Shader.h"
#include <cmath>
#include <string>

class Stereo {
public:
    enum Mode { OFF, ANAGLYPH, CARDBOARD, MODE_COUNT };
    int mode = OFF;
    float tunnel = 0.0f;           // comfort vignette while moving (0 = off, 1 = full)
    float eyeSeparation = 0.065f;   // metres (human average) in walk mode
    float convergence = 12.0f;      // distance of the zero-parallax plane
    bool autoSettings = true;       // scale separation with the orbit distance
    bool swapEyes = false;

    struct Eye {
        GLuint msFBO = 0, msColor = 0, msDepth = 0;   // multisampled render target
        GLuint fbo = 0, tex = 0;                     // resolved texture
    } eyes[2];
    int w = 0, h = 0;

    bool sideBySide() const { return mode == CARDBOARD; }

    void init(const std::string& assetDir) {
        shader = Shader(assetDir + "shaders/sky.vert", assetDir + "shaders/stereo.frag");
        glGenVertexArrays(1, &vao);
    }

    // (re)create the eye buffers for this size
    void resize(int width, int height) {
        if (width == w && height == h) return;
        w = width; h = height;
        for (Eye& e : eyes) {
            if (e.fbo) {
                glDeleteFramebuffers(1, &e.fbo); glDeleteFramebuffers(1, &e.msFBO);
                glDeleteTextures(1, &e.tex); glDeleteRenderbuffers(1, &e.msColor); glDeleteRenderbuffers(1, &e.msDepth);
            }
            glGenFramebuffers(1, &e.msFBO);
            glGenRenderbuffers(1, &e.msColor);
            glGenRenderbuffers(1, &e.msDepth);
            glBindRenderbuffer(GL_RENDERBUFFER, e.msColor);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, w, h);
            glBindRenderbuffer(GL_RENDERBUFFER, e.msDepth);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24, w, h);
            glBindFramebuffer(GL_FRAMEBUFFER, e.msFBO);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, e.msColor);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, e.msDepth);

            glGenFramebuffers(1, &e.fbo);
            glGenTextures(1, &e.tex);
            glBindTexture(GL_TEXTURE_2D, e.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_FRAMEBUFFER, e.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.tex, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Size of one eye's image: half the window width when the eyes sit side by side.
    glm::ivec2 eyeSize(int winW, int winH) const { return sideBySide() ? glm::ivec2(winW / 2, winH) : glm::ivec2(winW, winH); }

    // Camera for one eye (eye = -1 left, +1 right).
    // upHint: normally straight up; with head tracking, the head's up (so tilting the head rolls the view)
    void eyeMatrices(int eye, const glm::vec3& camPos, const glm::vec3& forward, float fovDeg, float aspect,
                     float nearP, float farP, glm::mat4& view, glm::mat4& proj, glm::vec3& eyePos,
                     const glm::vec3& upHint = glm::vec3(0, 1, 0)) const {
        glm::vec3 right = glm::normalize(glm::cross(forward, upHint));
        glm::vec3 up = glm::cross(right, forward);
        float e = eye * eyeSeparation * 0.5f;
        eyePos = camPos + right * e;
        view = glm::lookAt(eyePos, eyePos + forward, up);                  // parallel axes
        float top = nearP * std::tan(glm::radians(fovDeg) * 0.5f);
        float halfW = aspect * top;
        float shift = e * nearP / convergence;                            // off-axis frustum shift
        proj = glm::frustum(-halfW - shift, halfW - shift, -top, top, nearP, farP);
    }

    void beginEye(int i, int ew, int eh) {
        glBindFramebuffer(GL_FRAMEBUFFER, eyes[i].msFBO);
        glViewport(0, 0, ew, eh);
    }

    // Resolve the multisampled image of eye i into its texture.
    void endEye(int i, int ew, int eh) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, eyes[i].msFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, eyes[i].fbo);
        glBlitFramebuffer(0, 0, ew, eh, 0, 0, ew, eh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Combine both eyes onto the screen.
    void composite(int winW, int winH) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, winW, winH);
        glDisable(GL_DEPTH_TEST);
        shader.use();
        bool swap = swapEyes;
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, eyes[swap ? 1 : 0].tex);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, eyes[swap ? 0 : 1].tex);
        glActiveTexture(GL_TEXTURE0);
        shader.set("uLeft", 0);
        shader.set("uRight", 1);
        shader.set("uMode", mode);
        glm::ivec2 es = eyeSize(winW, winH);
        shader.set("uEyeUV", glm::vec2(float(es.x) / w, float(es.y) / h));   // used part of the eye texture
        shader.set("uAspect", float(es.x) / float(es.y));
        shader.set("uTunnel", tunnel);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
    }

private:
    Shader shader;
    GLuint vao = 0;
};
