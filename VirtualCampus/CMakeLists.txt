cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)   # allow FetchContent_Populate for header-only repos
endif()
project(VirtualCampus CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- GLFW (window + input) ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)

# --- GLM (math) ---
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE)

# --- Dear ImGui (control panel) ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE)

# --- stb (single-header image writer, used for screenshots) ---
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE)

# --- nlohmann/json (reads campus.json) ---
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(glfw glm json)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

# --- GLAD (OpenGL 3.3 core loader, pre-generated) ---
add_library(glad STATIC external/glad/src/glad.c)
target_include_directories(glad PUBLIC external/glad/include)

# --- The app ---
find_package(Threads REQUIRED)       # the before/after comparison runs in a background thread

add_executable(VirtualCampus
    src/main.cpp)
target_link_libraries(VirtualCampus PRIVATE glad glfw glm::glm imgui nlohmann_json::nlohmann_json Threads::Threads)
target_include_directories(VirtualCampus PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(VirtualCampus PRIVATE ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/")
if(UNIX AND NOT APPLE)
    target_link_libraries(VirtualCampus PRIVATE dl)
endif()
if(WIN32)
    target_link_libraries(VirtualCampus PRIVATE ws2_32)   # sockets for phone VR
endif()
if(MINGW)
    # ship-able exe: no dependency on the MinGW runtime DLLs
    target_link_options(VirtualCampus PRIVATE -static -static-libgcc -static-libstdc++)
endif()
