#version 330 core
// Shadow pass: render the scene from the sun; only depth is written.
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aInstance;

uniform mat4 uLightSpace;   // light projection * light view
uniform mat4 uModel;
uniform int  uInstanced;

void main() {
    mat4 model = uModel;
    if (uInstanced >= 1) {
        float c = cos(aInstance.w), s = sin(aInstance.w), k = aInstance.z;
        model = mat4(vec4(c * k, 0, -s * k, 0), vec4(0, k, 0, 0), vec4(s * k, 0, c * k, 0),
                     vec4(aInstance.x, 0, aInstance.y, 1));
    }
    gl_Position = uLightSpace * model * vec4(aPos, 1.0);
}
