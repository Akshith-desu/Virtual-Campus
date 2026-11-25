// Virtual Campus - an interactive 3D model of R. V. College of Engineering with stereoscopic VR.
// CGVR project, OpenGL 3.3 core profile.
//
//   Step 1  window, cameras, Blinn-Phong lighting, day/night
//   Step 2  the real campus from OpenStreetMap (extrusion, ear clipping, picking, collision)
//   Step 3  textures + mip-mapping, shadow mapping (PCF), procedural sky, instanced trees,
//           Flat / Gouraud / Phong shading, rooftop details
//   Step 4  stereo VR: off-axis stereo cameras, anaglyph, Cardboard lens distortion
//   Step 5  phone VR headset: head tracking from the phone's sensors, video streamed to the phone
//   Step 6  guided Catmull-Rom fly-through tour, minimap viewport, help overlay
//
// Controls (also in the in-app help, F1)
//   C  orbit <-> walk camera        T  start/stop the guided tour     V  cycle VR / stereo mode
//   M  minimap on/off               H  hide/show the panel            F1 help
//   F11 fullscreen                  F12 screenshot                    Esc stop tour / release mouse / quit
//   Q/E snap turn in VR
//   Orbit: right-drag rotate, scroll zoom, WASD pan.   Walk: click to look, WASD, Shift run, Tab free mouse
//   Left-click: select a building
//
// Command line (for testing / screenshots):
//   --campus file.json  --mode walk|orbit  --time H  --cam yaw,pitch,dist  --target x,z  --stereo 0-2
//   --tour S  --style 0|1  --surroundings 0-2  --minimap  --nominimap  --phonevr  --size W H
//   --screenshot out.png  --ui (screenshot with the panel)

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "PhoneVR.h"                // (includes stb_image_write.h declarations only)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "Shader.h"
#include "Mesh.h"
#include "Camera.h"
#define STB_IMAGE_IMPLEMENTATION     // stb_image is compiled here (included via Textures.h)
#include "Campus.h"
#include "Stereo.h"
#include "Tour.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ------------------------------------------------------------ app state
static Camera camera;
static int   winW = 1280, winH = 720;           // framebuffer size (pixels)
static bool  mouseCaptured = false;
static bool  rightDragging = false;
static double lastX = 0, lastY = 0;
static float timeOfDay = 10.0f;                 // hours, 0-24
static bool  animateTime = false;
static float timeSpeed = 1.0f;                  // in-game hours per real second
static int   screenshotCounter = 0;

static Campus campus;
static int hovered = -1, selected = -1;         // building indices
static float fogDensity = 0.0006f;

// graphics options (all switchable in the panel - handy for showing each technique in the viva)
static bool useTextures = true, useShadows = true, softShadows = true, showTrees = true, showClouds = true;
static int  shadingMode = 2;                    // 0 = flat, 1 = Gouraud, 2 = Phong
static int  surroundings = 1;                   // buildings outside the campus: 0 show, 1 faded, 2 hidden
static bool showLabels = true;                  // name pins over the departments, like the venue map
static int  colourStyle = 0;                    // 0 natural, 1 venue map
static std::vector<int> labelBuildings;         // one building per name, the largest
static const int SHADOW_SIZE = 4096;

// short message at the top of the screen
static std::string statusText;
static float statusTimer = 0;

// the part of the window the 3D view occupies (window coordinates)
static float viewX0 = 0, viewW = 1;
// ---- phone VR headset (see PhoneVR.h)
static PhoneVR phone;
static bool  phoneHeadset = false;               // phone page connected -> it steers the view
static float phoneYaw0 = 0, refYaw = 0;          // phone heading at re-centre / camera heading it maps to
static bool  phoneRecenter = true;
static bool  comfortVignette = true;
static float vignetteNow = 0;
static float yawOf(const glm::vec3& v) { return glm::degrees(std::atan2(v.z, v.x)); }
static glm::vec3 rotY(const glm::vec3& v, float deg) {     // same sense as increasing walkYaw
    float c = std::cos(glm::radians(deg)), s = std::sin(glm::radians(deg));
    return {v.x * c - v.z * s, v.y, v.x * s + v.z * c};
}


// VR / stereo (steps 4-5) and tour, minimap, UI (step 6)
static Stereo stereo;
static Tour tour;
static bool showMinimap = true, showUI = true, showHelp = false, showTourPath = false;
static float mapX = 0, mapY = 0, mapSize = 220;   // minimap rectangle in window coordinates
static float mapRadius = 500;                     // world metres from centre to edge
static std::string tourCaption;
static float tourCaptionAlpha = 0;
static bool fullscreen = false;
static int savedX = 100, savedY = 100, savedW = 1280, savedH = 720;

// ------------------------------------------------------------ helpers
static void setMouseCapture(GLFWwindow* w, bool on) {
    mouseCaptured = on;
    glfwSetInputMode(w, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    glfwGetCursorPos(w, &lastX, &lastY);
}

static void flash(const std::string& msg) { statusText = msg; statusTimer = 4.0f; }

static bool saveScreenshot(const std::string& path) {
    int w, h;
    glfwGetFramebufferSize(glfwGetCurrentContext(), &w, &h);
    std::vector<unsigned char> px(w * h * 3);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    bool ok = stbi_write_png(path.c_str(), w, h, 3, px.data(), w * 3) != 0;
    std::printf(ok ? "Saved %s\n" : "Could not save %s\n", path.c_str());
    return ok;
}

static bool collides(const glm::vec3& p) {
    return campus.collides({p.x, p.z}) || (showTrees && campus.hitsTree({p.x, p.z}, 0.35f));
}

static float aspectRatio() {
    if (winH <= 0) return 1.0f;
    return float(winW) / float(winH);
}

// Mouse position -> world-space ray (inverse of the projection * view transform).
static void mouseRay(GLFWwindow* w, glm::vec3& origin, glm::vec3& dir) {
    double mx, my; glfwGetCursorPos(w, &mx, &my);
    int ww, wh; glfwGetWindowSize(w, &ww, &wh);
    float x = 2.0f * float(mx - viewX0) / float(viewW) - 1.0f, y = 1.0f - 2.0f * float(my) / float(wh);
    glm::mat4 inv = glm::inverse(camera.projection(aspectRatio()) * camera.view());
    glm::vec4 a = inv * glm::vec4(x, y, -1, 1), b = inv * glm::vec4(x, y, 1, 1);
    origin = glm::vec3(a) / a.w;
    dir = glm::normalize(glm::vec3(b) / b.w - origin);
}

// World point -> window coordinates (for drawing labels/lines with ImGui). false if behind the camera.
static bool worldToScreen(const glm::vec3& p, ImVec2& out) {
    glm::vec4 c = camera.projection(aspectRatio()) * camera.view() * glm::vec4(p, 1);
    if (c.w <= 0.1f) return false;
    glm::vec3 n = glm::vec3(c) / c.w;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    out = ImVec2(viewX0 + (n.x * 0.5f + 0.5f) * viewW, (1.0f - (n.y * 0.5f + 0.5f)) * ds.y);
    return true;
}

static bool overMinimap(double mx, double my) {
    return showMinimap && mx >= mapX && mx <= mapX + mapSize && my >= mapY && my <= mapY + mapSize;
}

static glm::vec2 minimapToWorld(double mx, double my) {
    float u = float(mx - mapX) / mapSize - 0.5f, v = float(my - mapY) / mapSize - 0.5f;
    return campus.center + glm::vec2(u, v) * (2.0f * mapRadius);
}

// North-up view of the whole campus, like the printed venue map.
static void mapView(bool topDown) {
    camera.mode = Camera::ORBIT;
    camera.target = glm::vec3(campus.center.x, 0, campus.center.y);
    camera.orbitYaw = 90.0f;                            // camera to the south, looking north
    camera.orbitPitch = topDown ? 88.0f : 52.0f;
    camera.distance = glm::max(campus.extent.x, campus.extent.y) * (topDown ? 0.95f : 0.8f);
}

static void stopTour() {
    if (camera.mode == Camera::TOUR) { camera.mode = Camera::ORBIT; tour.active = false; }
}

static void startTour() {
    if (tour.stops.size() < 3) { flash("Not enough named buildings for a tour"); return; }
    tour.active = true;
    camera.mode = Camera::TOUR;
}

// ------------------------------------------------------------ callbacks
static void onResize(GLFWwindow*, int w, int h) { winW = w; winH = h; glViewport(0, 0, w, h); }

static void onCursor(GLFWwindow* w, double x, double y) {
    ImGui_ImplGlfw_CursorPosCallback(w, x, y);   // the panel needs the mouse position too
    float dx = float(x - lastX), dy = float(y - lastY);
    lastX = x; lastY = y;
    if (camera.mode == Camera::WALK && mouseCaptured) camera.onMouseMove(dx, dy);
    if (camera.mode == Camera::ORBIT && rightDragging) camera.onMouseMove(dx, dy);
}

static void onMouseButton(GLFWwindow* w, int button, int action, int mods) {
    ImGui_ImplGlfw_MouseButtonCallback(w, button, action, mods);
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (button == GLFW_MOUSE_BUTTON_RIGHT) rightDragging = (action == GLFW_PRESS);
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;

    double mx, my; glfwGetCursorPos(w, &mx, &my);
    if (!mouseCaptured && overMinimap(mx, my)) {          // click on the minimap: jump there
        glm::vec2 p = minimapToWorld(mx, my);
        stopTour();
        if (camera.mode == Camera::WALK) {
            glm::vec3 np(p.x, camera.eyeHeight, p.y);
            if (!collides(np)) camera.walkPos = np; else flash("That spot is inside a building");
        } else camera.target = glm::vec3(p.x, 0, p.y);
        return;
    }
    if (camera.mode == Camera::WALK && !mouseCaptured) { setMouseCapture(w, true); return; }
    selected = hovered;    // select the building under the cursor / crosshair
}

static void onScroll(GLFWwindow* w, double xo, double yo) {
    ImGui_ImplGlfw_ScrollCallback(w, xo, yo);
    if (ImGui::GetIO().WantCaptureMouse) return;
    camera.onScroll(float(yo));
}

static void toggleFullscreen(GLFWwindow* w) {
    fullscreen = !fullscreen;
    if (fullscreen) {
        glfwGetWindowPos(w, &savedX, &savedY);
        glfwGetWindowSize(w, &savedW, &savedH);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* vm = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(w, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
    } else {
        glfwSetWindowMonitor(w, nullptr, savedX, savedY, savedW, savedH, 0);
    }
}

static void startPhoneVR() {
    if (!phone.start(8080)) { flash("Phone VR: could not start the server (" + phone.error + ")"); return; }
    phoneHeadset = true; phoneRecenter = true;
    stereo.mode = Stereo::CARDBOARD;
    if (camera.mode == Camera::ORBIT) {                      // stand on the ground where the orbit camera looked
        camera.mode = Camera::WALK;
        glm::vec3 t = camera.target;
        camera.walkPos = glm::vec3(t.x, camera.eyeHeight, t.z);
    }
    flash("Phone VR: open " + phone.url() + " on your phone (same Wi-Fi)");
}
static void stopPhoneVR() { phone.stop(); phoneHeadset = false; camera.headOverride = false; }

static void onKey(GLFWwindow* w, int key, int sc, int action, int mods) {
    // VR comfort: snap turn (turning in steps is much easier on the stomach than smooth turning)
    if (action == GLFW_PRESS && (key == GLFW_KEY_Q || key == GLFW_KEY_E) && camera.mode == Camera::WALK &&
        (stereo.mode != Stereo::OFF || phoneHeadset) && !ImGui::GetIO().WantCaptureKeyboard) {
        float d = key == GLFW_KEY_Q ? -30.0f : 30.0f;
        if (phoneHeadset && phone.tracking()) refYaw += d; else camera.walkYaw += d;
        vignetteNow = 1.0f;
        return;
    }
    ImGui_ImplGlfw_KeyCallback(w, key, sc, action, mods);
    if (action != GLFW_PRESS) return;
    bool typing = ImGui::GetIO().WantCaptureKeyboard;
    if (key == GLFW_KEY_ESCAPE) {
        if (camera.mode == Camera::TOUR) stopTour();
        else if (mouseCaptured) setMouseCapture(w, false);
        else glfwSetWindowShouldClose(w, 1);
        return;
    }
    if (typing) return;
    if (key == GLFW_KEY_C) {
        if (camera.mode == Camera::TOUR) stopTour();
        else camera.mode = (camera.mode == Camera::ORBIT) ? Camera::WALK : Camera::ORBIT;
        setMouseCapture(w, false);
    }
    if (key == GLFW_KEY_T) { if (camera.mode == Camera::TOUR) stopTour(); else { setMouseCapture(w, false); startTour(); } }
    if (key == GLFW_KEY_V) stereo.mode = (stereo.mode + 1) % Stereo::MODE_COUNT;
    if (key == GLFW_KEY_M) showMinimap = !showMinimap;
    if (key == GLFW_KEY_H) showUI = !showUI;
    if (key == GLFW_KEY_F1) showHelp = !showHelp;
    if (key == GLFW_KEY_F11) toggleFullscreen(w);
    if (key == GLFW_KEY_TAB) setMouseCapture(w, false);
    if (key == GLFW_KEY_F12) saveScreenshot("screenshot_" + std::to_string(++screenshotCounter) + ".png");
}

// ------------------------------------------------------------ lighting from time of day
struct Lighting {
    glm::vec3 lightDir, lightColor, ambient;
    glm::vec3 sunDir, moonDir;
    glm::vec3 horizon, zenith;   // sky colours (horizon is also the fog colour)
    float night;                 // 0 = full day, 1 = full night
};

static Lighting computeLighting(float hours) {
    // the sun travels a half circle from east (6:00) through overhead (12:00) to west (18:00)
    float a = (hours - 6.0f) / 12.0f * glm::pi<float>();
    glm::vec3 sunDir = glm::normalize(glm::vec3(std::cos(a), std::sin(a), 0.35f));
    float elevation = sunDir.y;

    Lighting L;
    L.sunDir = sunDir;
    L.moonDir = glm::normalize(glm::vec3(-sunDir.x, -sunDir.y, 0.5f));
    L.night = glm::clamp(1.0f - (elevation + 0.1f) / 0.3f, 0.0f, 1.0f);       // fades around sunrise/sunset
    float dusk = glm::clamp(1.0f - std::abs(elevation) / 0.35f, 0.0f, 1.0f);  // orange glow near the horizon

    glm::vec3 dayH(0.70f, 0.82f, 0.95f), duskH(0.98f, 0.62f, 0.38f), nightH(0.05f, 0.07f, 0.13f);
    glm::vec3 dayZ(0.25f, 0.50f, 0.85f), duskZ(0.35f, 0.38f, 0.60f), nightZ(0.01f, 0.015f, 0.04f);
    float d = dusk * (1.0f - L.night);
    L.horizon = glm::mix(glm::mix(dayH, duskH, d), nightH, L.night);
    L.zenith  = glm::mix(glm::mix(dayZ, duskZ, d), nightZ, L.night);

    if (L.night < 1.0f) {   // sun
        L.lightDir = sunDir;
        L.lightColor = glm::mix(glm::vec3(1.0f, 0.96f, 0.88f), glm::vec3(1.0f, 0.6f, 0.35f), dusk) * 1.35f * (1.0f - L.night);
    } else {                // moon: dim and blue
        L.lightDir = L.moonDir;
        L.lightColor = glm::vec3(0.16f, 0.20f, 0.32f);
    }
    L.ambient = glm::mix(glm::vec3(0.24f, 0.27f, 0.32f), glm::vec3(0.05f, 0.06f, 0.10f), L.night);
    return L;
}

// Orthographic "camera" at the light, covering the whole campus (directional light).
static glm::mat4 lightSpaceMatrix(const glm::vec3& lightDir) {
    glm::vec3 c(campus.center.x, 0, campus.center.y);
    float r = glm::max(campus.extent.x, campus.extent.y) * 0.62f + 60.0f;
    glm::vec3 L = glm::normalize(lightDir);
    glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    glm::mat4 view = glm::lookAt(c + L * 1200.0f, c, up);
    glm::mat4 proj = glm::ortho(-r, r, -r, r, 1.0f, 2400.0f);
    return proj * view;
}

// Where are the shaders, textures and campus.json?  1) next to the exe (assets/, ../assets/),
// 2) the project folder baked in at build time.  So a copied exe + assets folder also works.
static std::string findAssetDir(const char* argv0) {
    auto exists = [](const std::string& f) { std::ifstream t(f); return t.good(); };
    std::string exe = argv0 ? argv0 : "";
    size_t slash = exe.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? std::string("./") : exe.substr(0, slash + 1);
    for (const std::string& c : {dir + "assets/", dir + "../assets/", std::string("assets/"), std::string(ASSET_DIR)})
        if (exists(c + "shaders/scene.vert")) return c;
    return ASSET_DIR;
}

// ------------------------------------------------------------ main
int main(int argc, char** argv) {
    const std::string assets = findAssetDir(argc > 0 ? argv[0] : nullptr);
    bool uiShot = false; int uiShotFrames = 0; bool startPhoneVRAtLaunch = false;
    std::string screenshotPath, campusPath = assets + "campus/campus.json";
    bool camOverride = false; float cy = 0, cp = 0, cd = 0;
    float tourSeconds = -1;
    bool targetOverride = false; float tx = 0, tz = 0;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* s) { return !std::strcmp(argv[i], s); };
        bool more = i + 1 < argc;
        if (is("--screenshot") && more) screenshotPath = argv[++i];
        else if (is("--ui")) uiShot = true;
        else if (is("--phonevr")) startPhoneVRAtLaunch = true;
        else if (is("--size") && i + 2 < argc) { winW = std::atoi(argv[++i]); winH = std::atoi(argv[++i]); }
        else if (is("--time") && more) timeOfDay = std::stof(argv[++i]);
        else if (is("--campus") && more) campusPath = argv[++i];
        else if (is("--stereo") && more) stereo.mode = glm::clamp(std::stoi(argv[++i]), 0, Stereo::MODE_COUNT - 1);
        else if (is("--tour") && more) tourSeconds = std::stof(argv[++i]);
        else if (is("--style") && more) colourStyle = std::stoi(argv[++i]);
        else if (is("--surroundings") && more) surroundings = std::stoi(argv[++i]);
        else if (is("--minimap")) showMinimap = true;
        else if (is("--nominimap")) showMinimap = false;
        else if (is("--target") && more) { float x, z; if (std::sscanf(argv[++i], "%f,%f", &x, &z) == 2) { targetOverride = true; tx = x; tz = z; } }
        else if (is("--cam") && more) camOverride = std::sscanf(argv[++i], "%f,%f,%f", &cy, &cp, &cd) == 3;
        else if (is("--mode") && more) {
            const char* m = argv[++i];
            camera.mode = !std::strcmp(m, "walk") ? Camera::WALK : Camera::ORBIT;
        }
    }

    if (!glfwInit()) { std::fprintf(stderr, "GLFW init failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_SAMPLES, 4);   // 4x MSAA anti-aliasing

    GLFWwindow* win = glfwCreateWindow(winW, winH, "Virtual Campus - CGVR", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "Window creation failed (needs OpenGL 3.3)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) { std::fprintf(stderr, "GLAD failed\n"); return 1; }
    std::printf("OpenGL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    glfwGetFramebufferSize(win, &winW, &winH);
    glViewport(0, 0, winW, winH);
    glfwSetFramebufferSizeCallback(win, onResize);

    // ImGui first (installs its callbacks), then ours, which forward to ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 0.97f;
    ImGui_ImplGlfw_InitForOpenGL(win, false);
    ImGui_ImplOpenGL3_Init("#version 330");
    glfwSetCursorPosCallback(win, onCursor);
    glfwSetMouseButtonCallback(win, onMouseButton);
    glfwSetScrollCallback(win, onScroll);
    glfwSetKeyCallback(win, onKey);
    glfwSetCharCallback(win, ImGui_ImplGlfw_CharCallback);
    glfwSetWindowFocusCallback(win, ImGui_ImplGlfw_WindowFocusCallback);
    glfwSetCursorEnterCallback(win, ImGui_ImplGlfw_CursorEnterCallback);

    glEnable(GL_DEPTH_TEST);    // hidden-surface removal with the z-buffer
    glEnable(GL_CULL_FACE);     // back-face culling
    glEnable(GL_MULTISAMPLE);

    std::printf("Assets: %s\n", assets.c_str());
    Shader shader(assets + "shaders/scene.vert", assets + "shaders/scene.frag");
    Shader depthShader(assets + "shaders/depth.vert", assets + "shaders/depth.frag");
    Shader skyShader(assets + "shaders/sky.vert", assets + "shaders/sky.frag");
    GLuint emptyVAO; glGenVertexArrays(1, &emptyVAO);     // the sky triangle needs no vertex data
    stereo.init(assets);

    tex::Set textures;
    textures.create(assets + "textures/");

    // ---- shadow map: a depth-only framebuffer rendered from the sun
    GLuint shadowFBO, shadowTex;
    glGenFramebuffers(1, &shadowFBO);
    glGenTextures(1, &shadowTex);
    glBindTexture(GL_TEXTURE_2D, shadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SHADOW_SIZE, SHADOW_SIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border[4] = {1, 1, 1, 1};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);   // sampler2DShadow
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) std::fprintf(stderr, "Shadow FBO incomplete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    Mesh plane = makePlane();
    if (!campus.load(campusPath)) campus.loadPlaceholder();
    campus.style = colourStyle;
    campus.build();
    tour.build(campus);
    mapRadius = glm::max(campus.extent.x, campus.extent.y) * 0.5f + 15.0f;

    // fit the cameras to the campus: north up, like the venue map
    { Camera::Mode m = camera.mode; mapView(false); camera.mode = m; }
    // pins: one per distinct name, on the largest building with that name
    for (size_t i = 0; i < campus.buildings.size(); ++i) {
        const BuildingInfo& b = campus.buildings[i];
        if (!b.inCampus || b.shortName.empty()) continue;
        bool dup = false;
        auto lower = [](std::string x) { for (char& ch : x) ch = (char)std::tolower((unsigned char)ch); return x; };
        for (int& j : labelBuildings)
            if (lower(campus.buildings[j].shortName) == lower(b.shortName)) { if (b.area > campus.buildings[j].area) j = (int)i; dup = true; break; }
        if (!dup) labelBuildings.push_back((int)i);
    }
    glm::vec2 start = campus.startPoint();
    camera.walkPos = glm::vec3(start.x, camera.eyeHeight, start.y);
    if (camOverride) { camera.orbitYaw = cy; camera.orbitPitch = cp; camera.distance = cd; }
    if (startPhoneVRAtLaunch) startPhoneVR();
    if (targetOverride) camera.target = glm::vec3(tx, 0, tz);

    if (tourSeconds >= 0) {
        startTour();
        std::string c; float a;
        tour.update(tourSeconds, camera.tourEye, camera.tourLook, c, a);
        tourCaption = c; tourCaptionAlpha = a;
    }

    // ------------------------------------------------------------ draws the whole scene once
    // (called once per frame normally, twice in stereo, and again for the minimap)
    auto drawScene = [&](const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye,
                         const Lighting& L, const glm::mat4& lightSpace, float now, bool withSky) {
        if (withSky) {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            skyShader.use();
            skyShader.set("uInvViewProj", glm::inverse(proj * view));
            skyShader.set("uCamPos", eye);
            skyShader.set("uZenith", L.zenith);
            skyShader.set("uHorizon", L.horizon);
            skyShader.set("uSunDir", L.sunDir);
            skyShader.set("uMoonDir", L.moonDir);
            skyShader.set("uNight", L.night);
            skyShader.set("uTime", now);
            skyShader.set("uClouds", showClouds ? 1 : 0);
            glBindVertexArray(emptyVAO);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
        }

        shader.use();
        shader.set("uView", view);
        shader.set("uProj", proj);
        shader.set("uViewPos", eye);
        shader.set("uLightDir", L.lightDir);
        shader.set("uLightColor", L.lightColor);
        shader.set("uAmbient", L.ambient);
        shader.set("uFogColor", L.horizon);
        shader.set("uFogDensity", withSky ? fogDensity : 0.0f);
        shader.set("uNight", L.night);
        shader.set("uColor", glm::vec3(1.0f));
        shader.set("uHighlight", float(hovered + 1));
        shader.set("uSelected", float(selected + 1));
        shader.set("uShading", shadingMode);
        shader.set("uFade", 0.0f);
        shader.set("uUseTextures", useTextures ? 1 : 0);
        shader.set("uUseShadows", useShadows ? 1 : 0);
        shader.set("uSoftShadows", softShadows ? 1 : 0);
        shader.set("uLightSpace", lightSpace);
        shader.set("uInstanced", 0);
        shader.set("uTex", 0);
        shader.set("uShadowMap", 1);
        shader.set("uRoofTex", 2);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, shadowTex);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, textures.roof);
        glActiveTexture(GL_TEXTURE0);

        // ground: a big plane, scaled and moved with a model matrix
        glm::mat4 M = glm::translate(glm::mat4(1.0f), glm::vec3(campus.center.x, 0, campus.center.y));
        M = glm::scale(M, glm::vec3(4000, 1, 4000));
        shader.set("uModel", M);
        shader.set("uColor", colourStyle == 1 ? glm::vec3(0.40f, 0.68f, 0.52f) : glm::vec3(1.0f));
        shader.set("uPattern", 1);
        shader.set("uUVScale", glm::vec2(1.0f));
        shader.set("uTexScale", 1.0f / 7.0f);              // grass texture repeats every 7 m
        glBindTexture(GL_TEXTURE_2D, textures.grass);
        plane.draw();

        // the campus is already in world coordinates -> identity model matrix
        shader.set("uModel", glm::mat4(1.0f));
        shader.set("uColor", glm::vec3(1.0f));
        shader.set("uPattern", 0);
        campus.areaMesh.draw();                            // lawns (grass detail)
        glBindTexture(GL_TEXTURE_2D, textures.paving);
        shader.set("uTexScale", 1.0f / 2.0f);              // paving tiles
        campus.pathMesh.draw();
        glBindTexture(GL_TEXTURE_2D, textures.asphalt);
        shader.set("uTexScale", 1.0f / 4.0f);
        shader.set("uPattern", 2);
        campus.roadMesh.draw();
        glBindTexture(GL_TEXTURE_2D, textures.facade);
        shader.set("uPattern", 3);
        campus.buildingMesh.draw();
        if (surroundings < 2) {                            // buildings outside the campus wall
            shader.set("uFade", surroundings == 1 ? 0.6f : 0.0f);
            campus.outsideMesh.draw();
            shader.set("uFade", 0.0f);
        }
        if (showTrees) {
            shader.set("uPattern", 4);
            shader.set("uInstanced", 1);
            campus.drawTrees();
        }
        shader.set("uInstanced", 0);
        if (showTourPath && withSky) {
            shader.set("uPattern", 6);
            tour.pathMesh.draw();
        }
    };

    double prevTime = glfwGetTime();
    while (!glfwWindowShouldClose(win)) {
        double now = glfwGetTime();
        float dt = float(now - prevTime);
        prevTime = now;
        glfwPollEvents();
        {   // the 3D view covers the whole window
            int ww, wh; glfwGetWindowSize(win, &ww, &wh);
            viewX0 = 0.0f; viewW = float(ww);
        }
        if (statusTimer > 0) statusTimer -= dt;

        // ---------- input: movement
        bool moving = false;
        if (!ImGui::GetIO().WantCaptureKeyboard && camera.mode != Camera::TOUR) {
            float fwd = 0, strafe = 0;
            if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS) fwd += 1;
            if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS) fwd -= 1;
            if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS) strafe += 1;
            if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS) strafe -= 1;
            bool run = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
            if (phoneHeadset && phone.walkHeld() && camera.mode == Camera::WALK) fwd += 1;   // touch & hold on the phone
            if (fwd != 0 || strafe != 0) {
                moving = true;
                glm::vec3 before = camera.walkPos;
                camera.move(strafe, fwd, dt, run);
                if (camera.mode == Camera::WALK && collides(camera.walkPos)) {
                    // slide along walls: try keeping only the x or only the z part of the move
                    glm::vec3 tryX(camera.walkPos.x, before.y, before.z);
                    glm::vec3 tryZ(before.x, before.y, camera.walkPos.z);
                    if (!collides(tryX)) camera.walkPos = tryX;
                    else if (!collides(tryZ)) camera.walkPos = tryZ;
                    else camera.walkPos = before;
                }
            }
        }
        if (!moving) camera.bobAmount *= std::pow(0.02f, dt);      // head bob fades out when standing still

        // ---------- phone VR: the phone's orientation becomes the camera's (head tracking)
        camera.headOverride = false;
        if (phoneHeadset && phone.isRunning() && phone.tracking() && camera.mode != Camera::ORBIT) {
            glm::vec3 pf = phone.forward(), pu = phone.up();
            bool vehicle = camera.mode == Camera::TOUR;       // on the tour the head turns relative to the flight
            glm::vec3 base = glm::normalize(camera.tourLook - camera.tourEye);
            if (phone.takeRecenter() || phoneRecenter) {
                phoneYaw0 = yawOf(pf);
                refYaw = vehicle ? 0.0f : camera.walkYaw;
                phoneRecenter = false;
                flash("Phone VR: view re-centred");
            }
            float offset = (vehicle ? yawOf(base) + refYaw : refYaw) - phoneYaw0;
            glm::vec3 f = glm::normalize(rotY(pf, offset)), u = rotY(pu, offset);
            u = glm::normalize(u - f * glm::dot(u, f));
            camera.headOverride = true; camera.headF = f; camera.headU = u;
            if (camera.mode == Camera::WALK) {
                camera.walkYaw = yawOf(f);
                camera.walkPitch = glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f)));
            }
        }
        // comfort vignette: narrow the view while moving or turning in VR
        {
            float want = (comfortVignette && stereo.mode != Stereo::OFF && moving) ? 0.8f : 0.0f;
            vignetteNow = comfortVignette ? std::max(want, vignetteNow - dt * 2.5f) : 0.0f;
            stereo.tunnel = stereo.mode == Stereo::OFF ? 0.0f : vignetteNow;
        }

        if (animateTime) timeOfDay = std::fmod(timeOfDay + dt * timeSpeed, 24.0f);
        if (camera.mode == Camera::TOUR) tour.update(dt, camera.tourEye, camera.tourLook, tourCaption, tourCaptionAlpha);

        // ---------- picking: building under the mouse / crosshair
        {
            glm::vec3 ro, rd;
            if (camera.mode == Camera::WALK && mouseCaptured) { ro = camera.position(); rd = camera.forward(); }
            else mouseRay(win, ro, rd);
            double mx, my; glfwGetCursorPos(win, &mx, &my);
            bool overUI = ImGui::GetCurrentContext() && (ImGui::GetIO().WantCaptureMouse || overMinimap(mx, my));
            hovered = overUI ? -1 : campus.pick(ro, rd, camera.mode == Camera::WALK ? 120.0f : 3000.0f, surroundings == 2);
            if (!screenshotPath.empty()) hovered = -1;                // clean screenshots
        }

        Lighting L = computeLighting(timeOfDay);
        glm::mat4 lightSpace = lightSpaceMatrix(L.lightDir);

        // ---------- pass 1: shadow map (depth from the light's point of view)
        if (useShadows) {
            glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO);
            glViewport(0, 0, SHADOW_SIZE, SHADOW_SIZE);
            glClear(GL_DEPTH_BUFFER_BIT);
            glCullFace(GL_FRONT);                // store back faces -> far less shadow acne
            depthShader.use();
            depthShader.set("uLightSpace", lightSpace);
            depthShader.set("uModel", glm::mat4(1.0f));
            depthShader.set("uInstanced", 0);
            campus.buildingMesh.draw();
            if (surroundings < 2) campus.outsideMesh.draw();
            if (showTrees) { depthShader.set("uInstanced", 1); campus.drawTrees(); }
            glCullFace(GL_BACK);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        // ---------- pass 2: the scene - once (mono) or once per eye (stereo)
        glm::vec3 camPos = camera.position(), fwd = camera.forward();
        if (stereo.mode == Stereo::OFF) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, winW, winH);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            drawScene(camera.view(), camera.projection(aspectRatio()), camPos, L, lightSpace, float(now), true);
        } else {
            if (stereo.autoSettings) {   // sensible depth for each camera mode
                if (camera.mode == Camera::ORBIT) { stereo.eyeSeparation = camera.distance / 40.0f; stereo.convergence = camera.distance; }
                else if (camera.mode == Camera::TOUR) { stereo.eyeSeparation = 1.5f; stereo.convergence = glm::distance(camera.tourEye, camera.tourLook); }
                else { stereo.eyeSeparation = 0.065f; stereo.convergence = 8.0f; }
            }
            stereo.resize(winW, winH);
            glm::ivec2 es = stereo.eyeSize(winW, winH);
            for (int i = 0; i < 2; ++i) {
                glm::mat4 v, p; glm::vec3 eyePos;
                stereo.eyeMatrices(i == 0 ? -1 : 1, camPos, fwd, camera.fov, float(es.x) / float(es.y),
                                   camera.nearPlane, camera.farPlane, v, p, eyePos, camera.upVector());
                stereo.beginEye(i, es.x, es.y);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                drawScene(v, p, eyePos, L, lightSpace, float(now), true);
                stereo.endEye(i, es.x, es.y);
            }
            stereo.composite(winW, winH);
        }

        // ---------- phone VR: send the finished 3D image (before the panel is drawn) to the phone
        if (phoneHeadset && phone.wantsFrame()) {
            static std::vector<unsigned char> px, small;
            px.resize(size_t(winW) * winH * 3);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, winW, winH, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            int f = winW > 1400 ? 2 : 1;                     // keep the stream light: about 1280 px wide at most
            if (f == 1) phone.submit(px.data(), winW, winH);
            else {
                int w = winW / f, h = winH / f;
                small.resize(size_t(w) * h * 3);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        for (int c = 0; c < 3; ++c) {
                            const unsigned char* p = &px[(size_t(y * f) * winW + x * f) * 3 + c];
                            small[(size_t(y) * w + x) * 3 + c] = (unsigned char)((p[0] + p[3] + p[winW * 3] + p[winW * 3 + 3] + 2) / 4);
                        }
                phone.submit(small.data(), w, h);
            }
        }

        // ---------- pass 3: minimap - the same scene, from straight above, orthographic projection
        float uiScale = ImGui::GetIO().DisplayFramebufferScale.x > 0 ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f;
        if (showMinimap && stereo.mode == Stereo::OFF) {
            int ww, wh; glfwGetWindowSize(win, &ww, &wh);
            if (ww > 0) uiScale = float(winW) / float(ww);
            mapSize = glm::clamp(wh * 0.3f, 140.0f, 260.0f);
            mapX = ww - mapSize - 12; mapY = wh - mapSize - 12;
            int px = int(mapX * uiScale), py = int((wh - mapY - mapSize) * uiScale), ps = int(mapSize * uiScale);
            glViewport(px, py, ps, ps);                           // window-to-viewport mapping
            glEnable(GL_SCISSOR_TEST);
            glScissor(px, py, ps, ps);
            glClearColor(0.2f, 0.25f, 0.2f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glm::vec3 c(campus.center.x, 0, campus.center.y);
            glm::mat4 topView = glm::lookAt(c + glm::vec3(0, 1000, 0), c, glm::vec3(0, 0, -1));   // north up
            glm::mat4 topProj = glm::ortho(-mapRadius, mapRadius, -mapRadius, mapRadius, 1.0f, 2000.0f);
            int savedHover = hovered; hovered = -1;
            drawScene(topView, topProj, c + glm::vec3(0, 1000, 0), L, lightSpace, float(now), false);
            hovered = savedHover;
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, winW, winH);
        }

        // ---------- screenshot-only run (no UI on the image)
        if (!screenshotPath.empty() && !uiShot) {
            glFinish();
            if (camera.mode == Camera::TOUR) std::printf("Tour: %zu stops, %.0f m, at '%s'\n", tour.stops.size(), tour.length(), tourCaption.c_str());
            saveScreenshot(screenshotPath);
            break;
        }

        // ---------- user interface
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImVec2 ds = ImGui::GetIO().DisplaySize;

        if (showUI) {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            // fixed width, height follows the open sections (scrolls if taller than the window)
            ImGui::SetNextWindowSizeConstraints(ImVec2(370, 0), ImVec2(370, ds.y - 20));
            ImGui::Begin("Virtual Campus", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextWrapped("%s", campus.name.c_str());
            ImGui::TextDisabled("%.0f FPS   F1 help   H hide panel", ImGui::GetIO().Framerate);
            if (selected < 0) ImGui::TextDisabled("Click a building to see its details");

            if (ImGui::CollapsingHeader("Camera & tour", ImGuiTreeNodeFlags_DefaultOpen)) {
                int mode = camera.mode;
                if (ImGui::RadioButton("Orbit", &mode, Camera::ORBIT)) { stopTour(); camera.mode = Camera::ORBIT; setMouseCapture(win, false); }
                ImGui::SameLine();
                if (ImGui::RadioButton("Walk", &mode, Camera::WALK)) { stopTour(); camera.mode = Camera::WALK; setMouseCapture(win, false); }
                ImGui::SameLine();
                if (ImGui::RadioButton("Guided tour", &mode, Camera::TOUR)) { setMouseCapture(win, false); startTour(); }
                ImGui::SliderFloat("Field of view", &camera.fov, 30.0f, 90.0f, "%.0f deg");
                if (camera.mode == Camera::TOUR) {
                    ImGui::SliderFloat("Tour speed", &tour.speed, 5.0f, 60.0f, "%.0f m/s");
                    ImGui::Text("%zu stops, %.1f km loop", tour.stops.size(), tour.length() / 1000.0f);
                }
                ImGui::Checkbox("Show tour path", &showTourPath); ImGui::SameLine();
                ImGui::Checkbox("Minimap", &showMinimap); ImGui::SameLine();
                ImGui::Checkbox("Head bob", &camera.headBob);
            }

            if (ImGui::CollapsingHeader("Campus look", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Colours:"); ImGui::SameLine();
                bool restyle = ImGui::RadioButton("Natural", &colourStyle, 0); ImGui::SameLine();
                restyle |= ImGui::RadioButton("Venue map", &colourStyle, 1);
                if (restyle) campus.setStyle(colourStyle);
                ImGui::Text("Outside campus:"); ImGui::SameLine();
                ImGui::RadioButton("Show", &surroundings, 0); ImGui::SameLine();
                ImGui::RadioButton("Fade", &surroundings, 1); ImGui::SameLine();
                ImGui::RadioButton("Hide", &surroundings, 2);
                ImGui::Checkbox("Name pins", &showLabels); ImGui::SameLine();
                if (ImGui::Button("Map view (north up)")) { stopTour(); mapView(true); }
                ImGui::SameLine();
                if (ImGui::Button("3D view")) { stopTour(); mapView(false); }
            }

            if (ImGui::CollapsingHeader("Time of day")) {
                int hh = int(timeOfDay), mm = int((timeOfDay - hh) * 60);
                ImGui::SliderFloat("Hour", &timeOfDay, 0.0f, 24.0f, "%.1f");
                ImGui::Text("Clock %02d:%02d  %s", hh, mm, L.night > 0.5f ? "(night)" : "(day)");
                ImGui::Checkbox("Animate", &animateTime);
                ImGui::SameLine();
                ImGui::SliderFloat("##speed", &timeSpeed, 0.1f, 4.0f, "%.1f h/s");
                ImGui::SliderFloat("Fog", &fogDensity, 0.0f, 0.004f, "%.4f");
            }

            if (ImGui::CollapsingHeader("Graphics")) {
                ImGui::Checkbox("Textures", &useTextures); ImGui::SameLine();
                ImGui::Checkbox("Trees", &showTrees); ImGui::SameLine();
                ImGui::Checkbox("Clouds", &showClouds);
                ImGui::Checkbox("Shadows", &useShadows); ImGui::SameLine();
                ImGui::Checkbox("Soft shadows (PCF)", &softShadows);
                ImGui::Text("Shading:"); ImGui::SameLine();
                ImGui::RadioButton("Flat", &shadingMode, 0); ImGui::SameLine();
                ImGui::RadioButton("Gouraud", &shadingMode, 1); ImGui::SameLine();
                ImGui::RadioButton("Phong", &shadingMode, 2);
                ImGui::TextDisabled("%zu trees (instanced), shadow map %dx%d", campus.trees.size(), SHADOW_SIZE, SHADOW_SIZE);
            }

            if (phoneHeadset) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::CollapsingHeader("VR / stereo 3D")) {
                const char* modes[] = {"Off (normal)", "Anaglyph red/cyan glasses", "Cardboard (phone viewer)"};
                ImGui::Combo("Mode (V)", &stereo.mode, modes, Stereo::MODE_COUNT);
                if (stereo.mode != Stereo::OFF) {
                    ImGui::Checkbox("Automatic depth", &stereo.autoSettings); ImGui::SameLine();
                    ImGui::Checkbox("Swap eyes", &stereo.swapEyes);
                    ImGui::BeginDisabled(stereo.autoSettings);
                    ImGui::SliderFloat("Eye separation", &stereo.eyeSeparation, 0.01f, 30.0f, "%.3f m", ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat("Convergence", &stereo.convergence, 1.0f, 1500.0f, "%.1f m", ImGuiSliderFlags_Logarithmic);
                    ImGui::EndDisabled();
                    ImGui::Checkbox("Comfort vignette", &comfortVignette); ImGui::SameLine();
                    ImGui::TextDisabled("Q/E: snap turn 30 deg");
                    ImGui::TextWrapped(stereo.mode == Stereo::ANAGLYPH ? "Wear red (left eye) / cyan (right eye) 3D glasses."
                                     : "For a phone in a Cardboard viewer: use Start phone VR below.");
                }
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "Phone as a VR headset (head tracking)");
                if (!phoneHeadset) {
                    ImGui::TextWrapped("Your phone shows the 3D view and its motion sensors move the camera "
                                       "when you turn your head. Phone and PC on the same Wi-Fi; no app needed.");
                    if (ImGui::Button("Start phone VR")) startPhoneVR();
                } else {
                    ImGui::Text("On the phone, open:");
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "  %s", phone.url().c_str());
                    if (ImGui::SmallButton("Copy address")) ImGui::SetClipboardText(phone.url().c_str());
                    ImGui::Text("Phone: %s", phone.tracking() ? "head tracking ON" : "not connected yet");
                    if (phone.streaming()) ImGui::Text("Video to phone: %.0f frames/s", phone.streamFps());
                    if (camera.mode == Camera::ORBIT) ImGui::TextColored(ImVec4(1, 0.7f, 0.5f, 1), "Switch to Walk or Guided tour.");
                    if (ImGui::Button("Re-centre view")) phoneRecenter = true;
                    ImGui::SameLine();
                    if (ImGui::Button("Stop phone VR")) stopPhoneVR();
                    ImGui::TextDisabled("Phone: touch & hold = walk, double-tap = re-centre.");
                    ImGui::TextDisabled("No head tracking? See README: Phone VR.");
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("Map data (c) OpenStreetMap contributors");
            ImGui::End();
        }

        // ---------- selected building: a small card at the top right
        if (selected >= 0 && showUI) {
            const BuildingInfo& b = campus.buildings[selected];
            ImGui::SetNextWindowPos(ImVec2(ds.x - 12, 12), ImGuiCond_Always, ImVec2(1, 0));
            ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);
            bool open = true;
            ImGui::Begin("Building", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            ImGui::TextColored(ImVec4(1, 0.85f, 0.5f, 1), "%s", b.name.empty() ? "(unnamed building)" : b.name.c_str());
            ImGui::Text("Type: %s%s", b.type.c_str(), b.inCampus ? "" : "  (outside campus)");
            ImGui::Text("Height: %.1f m  (~%d floors)", b.height, glm::max(1, int(b.height / 3.5f)));
            ImGui::Text("Footprint: %.0f m2", b.area);
            if (ImGui::Button("Fly here")) {
                stopTour();
                camera.mode = Camera::ORBIT;
                camera.target = glm::vec3((b.bmin.x + b.bmax.x) * 0.5f, 0, (b.bmin.y + b.bmax.y) * 0.5f);
                camera.distance = glm::max(60.0f, glm::length(b.bmax - b.bmin) * 1.6f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Walk here")) {
                stopTour();
                glm::vec2 c = (b.bmin + b.bmax) * 0.5f;
                // nearest free spot outside the building, searching outwards
                for (float r = 4; r < 200; r += 2) {
                    bool found = false;
                    for (int k = 0; k < 16 && !found; ++k) {
                        float ang = k * 0.3927f;
                        glm::vec2 p = c + glm::vec2(std::cos(ang), std::sin(ang)) * (glm::length(b.bmax - b.bmin) * 0.5f + r);
                        if (!collides({p.x, camera.eyeHeight, p.y})) {
                            camera.walkPos = glm::vec3(p.x, camera.eyeHeight, p.y);
                            glm::vec2 d = c - p;
                            camera.walkYaw = glm::degrees(std::atan2(d.y, d.x)); camera.walkPitch = 5;
                            found = true;
                        }
                    }
                    if (found) break;
                }
                camera.mode = Camera::WALK;
            }
            ImGui::End();
            if (!open) selected = -1;
        }

        // ---------- help window
        if (showHelp) {
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Help (F1)", &showHelp, ImGuiWindowFlags_AlwaysAutoResize);
            const char* lines[][2] = {
                {"C", "Orbit <-> walk camera"}, {"T", "Start / stop the guided tour"}, {"V", "Cycle VR / stereo modes"},
                {"Q / E", "Snap turn 30 degrees (VR, walk mode)"},
                {"M", "Minimap (click it to jump there)"}, {"H", "Hide / show the panel"}, {"F11", "Fullscreen"},
                {"F12", "Save a screenshot"}, {"Esc", "Stop tour / release mouse / quit"},
                {"Orbit", "Right-drag rotate, scroll zoom, WASD pan"},
                {"Walk", "Click to look around, WASD walk, Shift run, Tab free the mouse"},
                {"Left-click", "Select a building"}};
            if (ImGui::BeginTable("keys", 2)) {
                for (auto& l : lines) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "%s", l[0]);
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(l[1]);
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }

        // ---------- overlays: crosshair, hover label, name pins, tour caption, minimap marker
        if (stereo.mode == Stereo::OFF) {
            if (camera.mode == Camera::WALK && mouseCaptured) {
                ImVec2 c(ds.x * 0.5f, ds.y * 0.5f);
                fg->AddLine(ImVec2(c.x - 8, c.y), ImVec2(c.x + 8, c.y), IM_COL32(255, 255, 255, 200), 2);
                fg->AddLine(ImVec2(c.x, c.y - 8), ImVec2(c.x, c.y + 8), IM_COL32(255, 255, 255, 200), 2);
            }
            auto label = [&](ImVec2 pos, const char* text, ImU32 col) {
                ImVec2 ts = ImGui::CalcTextSize(text);
                fg->AddRectFilled(ImVec2(pos.x - 6, pos.y - 4), ImVec2(pos.x + ts.x + 6, pos.y + ts.y + 4), IM_COL32(20, 20, 30, 210), 4);
                fg->AddText(pos, col, text);
            };
            ImVec2 mouse = ImGui::GetIO().MousePos;
            if (hovered >= 0 && !campus.buildings[hovered].name.empty()) {
                ImVec2 pos = camera.mode == Camera::WALK && mouseCaptured ? ImVec2(ds.x * 0.5f + 14, ds.y * 0.5f + 10) : ImVec2(mouse.x + 16, mouse.y + 8);
                label(pos, campus.buildings[hovered].name.c_str(), IM_COL32(255, 230, 160, 255));
            }
            // name pins over the departments (venue-map style); hidden if another building is in front
            if (showLabels && camera.mode != Camera::WALK) {
                std::vector<ImVec4> placed;            // rectangles already used
                ImDrawList* bg = ImGui::GetBackgroundDrawList();   // behind the panel
                struct Pin { ImVec2 pin, roof; float d; const std::string* text; bool gate; };
                std::vector<Pin> pins;
                glm::vec3 eye = camera.position();
                for (int bi : labelBuildings) {
                    const BuildingInfo& b = campus.buildings[bi];
                    glm::vec3 top((b.bmin.x + b.bmax.x) * 0.5f, b.height + 1.0f, (b.bmin.y + b.bmax.y) * 0.5f);
                    glm::vec3 head = top + glm::vec3(0, 9.0f, 0);
                    float d = glm::distance(eye, head);
                    int hit = campus.pick(eye, glm::normalize(top - eye), glm::distance(eye, top) - 1.0f, surroundings == 2);
                    if (hit >= 0 && hit != bi) continue;                       // behind another building
                    ImVec2 a, r;
                    if (worldToScreen(head, a) && worldToScreen(top, r)) pins.push_back({a, r, d, &b.shortName, false});
                }
                for (const Campus::GateGroup& gg : campus.gateGroups) {     // gate pins (blue)
                    ImVec2 pa, pr;
                    if (worldToScreen({gg.pos.x, 12.0f, gg.pos.y}, pa) && worldToScreen({gg.pos.x, 3.0f, gg.pos.y}, pr))
                        pins.push_back({pa, pr, glm::distance(eye, glm::vec3(gg.pos.x, 12, gg.pos.y)), &gg.name, true});
                }
                std::sort(pins.begin(), pins.end(), [](const Pin& x, const Pin& y) { return x.d < y.d; });
                for (const Pin& pn : pins) {
                    ImVec2 ts = ImGui::CalcTextSize(pn.text->c_str());
                    ImVec4 rc(pn.pin.x - ts.x * 0.5f - 6, pn.pin.y - ts.y - 16, pn.pin.x + ts.x * 0.5f + 6, pn.pin.y - 6);
                    bool clash = false;
                    for (const ImVec4& q : placed)
                        if (rc.x < q.z && rc.z > q.x && rc.y < q.w && rc.w > q.y) { clash = true; break; }
                    if (clash || rc.x < viewX0 || rc.z > viewX0 + viewW || rc.y < 0) continue;
                    placed.push_back(rc);
                    ImU32 pinCol = pn.gate ? IM_COL32(40, 120, 220, 255) : IM_COL32(220, 60, 40, 255);
                    bg->AddLine(pn.roof, pn.pin, IM_COL32(255, 255, 255, 170), 1.5f);
                    bg->AddCircleFilled(pn.pin, 5.5f, pinCol);
                    bg->AddCircle(pn.pin, 5.5f, IM_COL32(255, 255, 255, 230), 0, 1.5f);
                    bg->AddRectFilled(ImVec2(rc.x, rc.y), ImVec2(rc.z, rc.w), IM_COL32(20, 30, 25, 215), 5);
                    bg->AddText(ImVec2(rc.x + 6, rc.y + 3), IM_COL32(255, 255, 255, 255), pn.text->c_str());
                }
            }
            // minimap frame, camera marker and view direction
            if (showMinimap) {
                fg->AddRect(ImVec2(mapX - 1, mapY - 1), ImVec2(mapX + mapSize + 1, mapY + mapSize + 1), IM_COL32(255, 255, 255, 200), 3, 0, 2);
                glm::vec3 p = camera.mode == Camera::ORBIT ? camera.target : camera.position();
                float u = (p.x - campus.center.x) / (2 * mapRadius) + 0.5f, v = (p.z - campus.center.y) / (2 * mapRadius) + 0.5f;
                if (u > 0 && u < 1 && v > 0 && v < 1) {
                    ImVec2 m(mapX + u * mapSize, mapY + v * mapSize);
                    glm::vec3 f = camera.forward();
                    glm::vec2 d = glm::length(glm::vec2(f.x, f.z)) > 1e-3f ? glm::normalize(glm::vec2(f.x, f.z)) : glm::vec2(0, -1);
                    glm::vec2 s(-d.y, d.x);
                    fg->AddTriangleFilled(ImVec2(m.x + d.x * 10, m.y + d.y * 10), ImVec2(m.x - d.x * 5 + s.x * 6, m.y - d.y * 5 + s.y * 6),
                                          ImVec2(m.x - d.x * 5 - s.x * 6, m.y - d.y * 5 - s.y * 6), IM_COL32(255, 60, 40, 255));
                }
                fg->AddText(ImVec2(mapX + 6, mapY + 4), IM_COL32(255, 255, 255, 220), "N");
            }
        }
        if (camera.mode == Camera::TOUR && tourCaptionAlpha > 0.01f && !tourCaption.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(tourCaption.c_str());
            float sc = 1.8f;
            ImVec2 pos(ds.x * 0.5f - ts.x * sc * 0.5f, ds.y * 0.82f);
            int a = int(tourCaptionAlpha * 230);
            fg->AddRectFilled(ImVec2(pos.x - 14, pos.y - 8), ImVec2(pos.x + ts.x * sc + 14, pos.y + ts.y * sc + 8), IM_COL32(15, 20, 30, a), 8);
            fg->AddText(ImGui::GetFont(), ImGui::GetFontSize() * sc, pos, IM_COL32(255, 235, 180, a + 25), tourCaption.c_str());
        }
        if (statusTimer > 0 && !statusText.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(statusText.c_str());
            ImVec2 pos(ds.x * 0.5f - ts.x * 0.5f, 18);
            int a = int(std::min(1.0f, statusTimer) * 230);
            fg->AddRectFilled(ImVec2(pos.x - 10, pos.y - 6), ImVec2(pos.x + ts.x + 10, pos.y + ts.y + 6), IM_COL32(20, 20, 30, a), 6);
            fg->AddText(pos, IM_COL32(255, 255, 255, a + 25), statusText.c_str());
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (uiShot && !screenshotPath.empty() && ++uiShotFrames >= 4) { glFinish(); saveScreenshot(screenshotPath); break; }
        glfwSwapBuffers(win);
    }

    phone.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
