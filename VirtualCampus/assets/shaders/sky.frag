#version 330 core
// Procedural sky dome: for every pixel, un-project to a world-space view direction and
// compute the sky colour in that direction (gradient + sun + moon + stars + clouds).
in vec2 vNdc;

uniform mat4  uInvViewProj;    // inverse(projection * view)
uniform vec3  uCamPos;
uniform vec3  uZenith;         // sky colour straight up
uniform vec3  uHorizon;        // sky colour at the horizon (= fog colour)
uniform vec3  uSunDir;
uniform vec3  uMoonDir;
uniform float uNight;
uniform float uTime;           // seconds, moves the clouds
uniform int   uClouds;

out vec4 FragColor;

float hash(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + vec2(1, 0)), f.x), mix(hash2(i + vec2(0, 1)), hash2(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { s += a * noise(p); p *= 2.03; a *= 0.5; }
    return s;
}

void main() {
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - uCamPos);

    float up = max(dir.y, 0.0);
    vec3 col = mix(uHorizon, uZenith, pow(up, 0.5));

    // sun: bright disc + soft glow
    float s = max(dot(dir, normalize(uSunDir)), 0.0);
    vec3 sunCol = mix(vec3(1.0, 0.95, 0.85), vec3(1.0, 0.55, 0.25), clamp(1.0 - uSunDir.y * 3.0, 0.0, 1.0));
    float sunUp = smoothstep(-0.05, 0.05, uSunDir.y);
    col += sunCol * (pow(s, 600.0) * 4.0 + pow(s, 12.0) * 0.25) * sunUp;

    // night: stars and the moon
    if (uNight > 0.01 && dir.y > 0.0) {
        vec3 cell = floor(dir * 300.0);
        float star = step(0.9985, hash(cell));
        float twinkle = 0.6 + 0.4 * sin(uTime * 3.0 + hash(cell + 1.0) * 40.0);
        col += vec3(star * twinkle * uNight * smoothstep(0.0, 0.2, dir.y));
        float m = max(dot(dir, normalize(uMoonDir)), 0.0);
        col += vec3(0.9, 0.92, 1.0) * (smoothstep(0.9993, 0.9996, m) * 1.2 + pow(m, 40.0) * 0.08) * uNight;
    }

    // clouds: fbm noise projected on a plane above the camera
    if (uClouds == 1 && dir.y > 0.02) {
        vec2 uv = dir.xz / dir.y * 1.5 + vec2(uTime * 0.01, uTime * 0.004);
        float c = smoothstep(0.5, 0.8, fbm(uv));
        vec3 cloudCol = mix(vec3(1.0), sunCol * 0.9, 0.3 * (1.0 - sunUp * 0.7)) * (1.0 - 0.85 * uNight);
        col = mix(col, cloudCol, c * smoothstep(0.02, 0.25, dir.y) * 0.85);
    }

    FragColor = vec4(col, 1.0);
}
