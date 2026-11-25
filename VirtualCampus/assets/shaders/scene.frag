#version 330 core
// Fragment shader: texture mapping, shadow mapping (PCF), Flat / Gouraud / Blinn-Phong
// shading, night window lights, hover highlight and distance fog.
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec3 vColor;
in vec3 vGouraud;
in float vSpecG;
flat in float vId;

uniform vec3  uColor;          // material colour multiplier
uniform vec3  uLightDir;       // direction TOWARDS the light (sun or moon)
uniform vec3  uLightColor;
uniform vec3  uAmbient;
uniform vec3  uViewPos;
uniform vec3  uFogColor;
uniform float uFogDensity;
uniform int   uPattern;        // 0 = ground cover, 1 = ground, 2 = road, 3 = building, 4 = tree, 6 = overlay (tour path)
uniform float uNight;          // 0 = day, 1 = night
uniform float uHighlight;      // id of the building under the cursor (0 = none)
uniform float uSelected;       // id of the clicked building (0 = none)
uniform int   uShading;        // 0 = flat, 1 = Gouraud, 2 = Phong
uniform float uFade;           // 0..1: wash out (buildings outside the campus)

// textures
uniform sampler2D uTex;        // detail texture for this draw (grass, asphalt, paving, facade)
uniform sampler2D uRoofTex;    // roofs
uniform float uTexScale;       // world-space tiling for horizontal surfaces (1 / metres)
uniform int   uUseTextures;

// shadows
uniform sampler2DShadow uShadowMap;
uniform mat4  uLightSpace;     // world -> light clip space
uniform int   uUseShadows;
uniform int   uSoftShadows;    // percentage-closer filtering

out vec4 FragColor;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// 1 = fully lit, 0 = in shadow
float shadowFactor(vec3 N, vec3 L) {
    if (uUseShadows == 0) return 1.0;
    // normal offset: nudge the lookup point off the surface to avoid "shadow acne"
    vec3 p = vWorldPos + N * 0.12;
    vec4 ls = uLightSpace * vec4(p, 1.0);
    vec3 proj = ls.xyz / ls.w * 0.5 + 0.5;               // NDC -> [0,1] texture space
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) return 1.0;
    float bias = 0.0002 + 0.0006 * (1.0 - max(dot(N, L), 0.0));
    float ref = proj.z - bias;
    if (uSoftShadows == 0) return texture(uShadowMap, vec3(proj.xy, ref));
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x)                         // 3x3 PCF; each tap is also
        for (int y = -1; y <= 1; ++y)                     // hardware-filtered (2x2) -> soft edges
            sum += texture(uShadowMap, vec3(proj.xy + vec2(x, y) * texel, ref));
    return sum / 9.0;
}

void main() {
    // ---- normal: interpolated (Gouraud/Phong) or per-face (flat, from screen-space derivatives)
    vec3 N = normalize(vNormal);
    if (uShading == 0) {
        N = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
        if (dot(N, vNormal) < 0.0) N = -N;
    }
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 H = normalize(L + V);                            // Blinn-Phong half vector

    vec3 base = uColor * vColor;
    vec3 emissive = vec3(0.0);
    float shininess = 32.0, specStrength = 0.12;
    bool plain = (uPattern == 3 && vUV.y > 899.0);        // rooftop tanks, doors, solar panels: own colour
    bool roof = (uPattern == 3 && N.y > 0.5 && !plain);

    // ---- texture mapping
    if (uPattern == 1) {                                  // ground: grass texture, planar mapping
        vec3 g = vec3(0.42, 0.55, 0.28);
        if (uUseTextures == 1) {
            g = texture(uTex, vWorldPos.xz * uTexScale).rgb;
            // a second, much larger sample breaks up the visible repetition of the tile
            g *= 0.7 + 0.6 * texture(uTex, vWorldPos.xz * uTexScale * 0.083 + 0.37).g;
        }
        base *= g * 1.05;
    } else if (uPattern == 0 || uPattern == 2) {          // lawns, paths, roads: greyscale detail
        if (uUseTextures == 1) base *= 0.55 + 0.6 * luma(texture(uTex, vWorldPos.xz * uTexScale).rgb);
        if (uPattern == 2) {                              // dashed centre line
            float centre = step(abs(vUV.y - 0.5), 0.02);
            float dash = step(0.5, fract(vUV.x));
            base = mix(base, vec3(0.95, 0.9, 0.6), centre * dash);
        }
    } else if (plain) {
        if (vUV.x > 0.5) {                                // solar glass: cell grid, glossy
            vec2 g = fract(vWorldPos.xz * vec2(6.0, 3.5));
            base *= 1.0 - 0.45 * max(step(0.92, g.x), step(0.9, g.y));
            shininess = 128.0; specStrength = 0.9;
        } else specStrength = 0.08;
    } else if (roof) {
        if (uUseTextures == 1) base *= 0.6 + 0.5 * luma(texture(uRoofTex, vWorldPos.xz * 0.2).rgb);
    } else if (uPattern == 3 && vUV.y > 499.0) {          // parapets and the compound wall: plain plaster
        float coping = step(0.25, fract(vUV.y));          // the top of a parapet is a little darker
        base *= mix(1.0, 0.86, coping);
        if (uUseTextures == 1) base *= 0.9 + 0.2 * luma(texture(uRoofTex, vWorldPos.xz * 0.3 + vWorldPos.y * 0.2).rgb);
        // soft darkening where the wall meets the ground (cheap ambient occlusion)
        base *= mix(0.7, 1.0, smoothstep(0.0, 1.2, vWorldPos.y));
    } else if (uPattern == 3) {                           // walls: facade texture, one cell per window
        float glass;
        vec3 t;
        if (uUseTextures == 1) {
            vec4 f = texture(uTex, vUV);
            t = f.rgb; glass = f.a;
        } else {                                          // procedural fallback (step-2 look)
            vec2 c = fract(vUV);
            glass = step(0.25, c.x) * step(c.x, 0.75) * step(0.3, c.y) * step(c.y, 0.8);
            t = mix(vec3(1.0), vec3(0.25, 0.32, 0.4), glass);
        }
        base = glass < 0.3 ? base * t : t;                // plaster is tinted, frames/glass are not
        if (vWorldPos.y < 0.7) { base = vColor * vec3(0.55, 0.52, 0.5); glass = 0.0; }   // darker plinth
        base *= mix(0.72, 1.0, smoothstep(0.0, 3.0, vWorldPos.y));                    // contact shadow
        if (glass > 0.9) { shininess = 96.0; specStrength = 0.6; }
        // at night some windows light up (pseudo-random per window)
        float r = fract(sin(dot(floor(vUV) + vId * 7.31, vec2(12.9898, 78.233))) * 43758.5453);
        emissive = step(0.9, glass) * step(0.45, r) * uNight * vec3(1.0, 0.82, 0.5) * 0.9;
    } else if (uPattern == 4) {                           // trees: soft, little specular
        specStrength = 0.03;
    } else if (uPattern == 6) {                           // overlays: mostly self-lit so they read at night too
        emissive = pow(base, vec3(2.2)) * 0.45;
    }

    // wash out the surroundings so the campus stands out (like the venue map)
    if (uFade > 0.0) base = mix(base, vec3(luma(base)) * vec3(0.95, 1.0, 0.95), uFade);

    // hover / selection highlight
    if (vId > 0.5 && abs(vId - uSelected) < 0.5)       { base = mix(base, vec3(1.0, 0.55, 0.1), 0.45); emissive += vec3(0.12, 0.06, 0.0); }
    else if (vId > 0.5 && abs(vId - uHighlight) < 0.5) { base = mix(base, vec3(1.0, 0.85, 0.3), 0.30); emissive += vec3(0.06, 0.05, 0.0); }

    base = pow(base, vec3(2.2));                          // sRGB -> linear space for lighting

    // ---- lighting
    float sh = shadowFactor(N, L);
    vec3 color;
    if (uShading == 1) {                                  // Gouraud: lighting came from the vertices
        vec3 lit = uAmbient + (vGouraud - uAmbient) * sh;
        color = lit * base + vSpecG * uLightColor * specStrength * sh;
    } else {                                              // flat / Phong: per-fragment
        float diff = max(dot(N, L), 0.0);
        float spec = diff > 0.0 ? pow(max(dot(N, H), 0.0), shininess) : 0.0;
        // hemispheric ambient: surfaces facing the sky get a little more
        // hemispheric ambient: sky light from above, green light bounced off the lawns from below
        vec3 hemi = mix(vec3(0.55, 0.62, 0.45), vec3(1.0, 1.0, 1.05), N.y * 0.5 + 0.5);
        vec3 ambient = uAmbient * base * hemi;
        color = ambient + sh * (diff * uLightColor * base + spec * uLightColor * specStrength);
    }
    color += emissive;

    // ---- exponential-squared fog
    float d = length(uViewPos - vWorldPos);
    float fog = 1.0 - exp(-pow(d * uFogDensity, 2.0));
    color = mix(color, pow(uFogColor, vec3(2.2)), clamp(fog, 0.0, 1.0));

    FragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);  // gamma correction
}
