#version 330 core
// Vertex shader: Model-View-Projection pipeline, instancing (trees), and Gouraud shading.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;     // rgb + building id
layout(location = 4) in vec4 aInstance;  // trees: x, z, scale, rotation

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform vec2 uUVScale;
uniform int  uInstanced;      // 0 = uModel, 1 = tree instance

// for Gouraud shading (lighting evaluated here, per vertex)
uniform int  uShading;        // 0 = flat, 1 = Gouraud, 2 = Phong (Blinn-Phong)
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;
uniform vec3 uViewPos;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec3 vColor;
out vec3 vGouraud;            // light reaching this vertex (diffuse + ambient), interpolated
out float vSpecG;
flat out float vId;

mat4 instanceMatrix(vec4 inst) {
    float c = cos(inst.w), s = sin(inst.w), k = inst.z;
    // translate(x,0,z) * rotateY(w) * scale(k)   - built by hand
    return mat4(vec4( c * k, 0.0, -s * k, 0.0),
                vec4( 0.0,   k,    0.0,   0.0),
                vec4( s * k, 0.0,  c * k, 0.0),
                vec4( inst.x, 0.0, inst.y, 1.0));
}

void main() {
    mat4 model = (uInstanced >= 1) ? instanceMatrix(aInstance) : uModel;
    vec4 world = model * vec4(aPos, 1.0);                 // object -> world
    vWorldPos = world.xyz;
    vNormal = normalize(mat3(transpose(inverse(model))) * aNormal);
    vUV = aUV * uUVScale;
    vColor = aColor.rgb;
    vId = aColor.a;

    // Gouraud: compute the light at the vertex, let the rasteriser interpolate it
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 H = normalize(L + V);
    float diff = max(dot(vNormal, L), 0.0);
    vGouraud = uAmbient + diff * uLightColor;
    vSpecG = diff > 0.0 ? pow(max(dot(vNormal, H), 0.0), 32.0) : 0.0;

    gl_Position = uProj * uView * world;                  // world -> camera -> clip space
}
