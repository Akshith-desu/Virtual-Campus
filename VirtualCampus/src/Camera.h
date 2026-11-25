// Camera.h - two camera modes:
//   ORBIT : bird's-eye view that rotates around a target point (yaw/pitch/distance)
//   WALK  : first-person, eyes at 1.7 m, WASD + mouse look  (also the VR / phone headset view)
//   TOUR  : the guided fly-through sets the eye and look-at point each frame
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/common.hpp>
#include <algorithm>
#include <cmath>

class Camera {
public:
    enum Mode { ORBIT, WALK, TOUR };
    Mode mode = ORBIT;

    // orbit state
    glm::vec3 target{0.0f, 0.0f, 0.0f};
    float orbitYaw = -45.0f, orbitPitch = 40.0f, distance = 160.0f;

    // walk state
    glm::vec3 walkPos{0.0f, 1.7f, 90.0f};
    float walkYaw = -90.0f, walkPitch = 0.0f;
    float eyeHeight = 1.7f;

    // head tracking (phone VR): when set, the head decides where the camera looks
    bool headOverride = false;
    glm::vec3 headF{0, 0, -1}, headU{0, 1, 0};

    float fov = 60.0f;
    float nearPlane = 0.5f, farPlane = 6000.0f;

    // tour state (set by Tour each frame)
    glm::vec3 tourEye{0, 50, 0}, tourLook{0};

    // walk-mode head bob (a small up/down motion while walking adds a lot of immersion)
    float bobPhase = 0, bobAmount = 0;
    bool headBob = true;

    glm::vec3 position() const {
        if (mode == TOUR) return tourEye;
        if (mode == WALK) return walkPos + glm::vec3(0, headBob ? std::sin(bobPhase) * 0.05f * bobAmount : 0.0f, 0);
        float y = glm::radians(orbitYaw), p = glm::radians(orbitPitch);
        glm::vec3 offset(std::cos(p) * std::cos(y), std::sin(p), std::cos(p) * std::sin(y));
        return target + offset * distance;
    }

    glm::vec3 forward() const {
        if (headOverride && mode != ORBIT) return headF;
        if (mode == TOUR) return glm::normalize(tourLook - tourEye);
        if (mode == WALK) {
            float y = glm::radians(walkYaw), p = glm::radians(walkPitch);
            return glm::normalize(glm::vec3(std::cos(p) * std::cos(y), std::sin(p), std::cos(p) * std::sin(y)));
        }
        return glm::normalize(target - position());
    }

    // View matrix = world -> camera transform (the "viewing transformation")
    glm::mat4 view() const {
        glm::vec3 eye = position();
        return glm::lookAt(eye, eye + forward(), upVector());
    }

    glm::vec3 upVector() const { return headOverride && mode != ORBIT ? headU : glm::vec3(0, 1, 0); }

    // Perspective projection matrix
    glm::mat4 projection(float aspect) const {
        return glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
    }

    void onMouseMove(float dx, float dy) {
        const float sens = 0.15f;
        if (mode == TOUR) return;
        if (mode == WALK) {
            walkYaw += dx * sens;
            walkPitch = glm::clamp(walkPitch - dy * sens, -89.0f, 89.0f);
        } else {
            orbitYaw += dx * sens;
            orbitPitch = glm::clamp(orbitPitch + dy * sens, 5.0f, 89.0f);
        }
    }

    void onScroll(float amount) {
        if (mode == ORBIT) distance = glm::clamp(distance * (1.0f - amount * 0.1f), 10.0f, 2500.0f);
        else fov = glm::clamp(fov - amount * 2.0f, 30.0f, 90.0f);
    }

    // move: x = strafe right, z = forward. In orbit mode it pans the target.
    void move(float strafe, float fwd, float dt, bool run) {
        if (mode == WALK) {
            float speed = (run ? 12.0f : 4.5f) * dt;           // metres per second
            glm::vec3 f = forward(); f.y = 0;                         // stay on the ground
            if (glm::length(f) < 1e-3f) f = glm::vec3(std::cos(glm::radians(walkYaw)), 0, std::sin(glm::radians(walkYaw)));
            f = glm::normalize(f);
            glm::vec3 r = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0)));
            walkPos += (f * fwd + r * strafe) * speed;
            walkPos.y = eyeHeight;
            bobPhase += dt * (run ? 14.0f : 9.0f);
            bobAmount = 1.0f;
        } else if (mode == ORBIT) {
            float speed = distance * 0.8f * dt;
            float y = glm::radians(orbitYaw);
            glm::vec3 f(-std::cos(y), 0, -std::sin(y));
            glm::vec3 r(-std::sin(y), 0, std::cos(y));
            target += (f * fwd + r * -strafe) * speed;
        }
    }
};
