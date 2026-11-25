#version 330 core
// Combines the left- and right-eye images (see Stereo.h).
in vec2 vNdc;

uniform sampler2D uLeft;
uniform sampler2D uRight;
uniform int  uMode;      // 1 anaglyph, 2 cardboard
uniform vec2 uEyeUV;     // fraction of the eye texture that holds the image
uniform float uAspect;   // width / height of one eye image
uniform float uTunnel;   // comfort vignette: narrows the view while moving, which reduces motion sickness

out vec4 FragColor;

// Barrel distortion: pre-distorts the image so that a phone VR viewer's magnifying
// lenses (which cause pincushion distortion) make it look straight again.
vec2 barrel(vec2 uv, out bool outside) {
    vec2 c = uv * 2.0 - 1.0;                       // -1..1 around the lens centre
    c.x *= uAspect;
    float r2 = dot(c, c);
    c *= 1.0 + 0.22 * r2 + 0.10 * r2 * r2;         // k1, k2 distortion coefficients
    c.x /= uAspect;
    c *= 0.78;                                     // zoom so the edges stay inside the image
    vec2 res = c * 0.5 + 0.5;
    outside = any(lessThan(res, vec2(0.0))) || any(greaterThan(res, vec2(1.0)));
    return res;
}

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;                    // 0..1 over the window

    if (uMode == 1) {                              // anaglyph (Dubois, red/cyan)
        vec3 l = texture(uLeft, uv * uEyeUV).rgb;
        vec3 r = texture(uRight, uv * uEyeUV).rgb;
        mat3 ml = mat3( 0.456, -0.040, -0.015,     // columns
                        0.500, -0.038, -0.021,
                        0.176, -0.016, -0.005);
        mat3 mr = mat3(-0.043,  0.378, -0.072,
                       -0.088,  0.734, -0.113,
                       -0.002, -0.018,  1.226);
        FragColor = vec4(clamp(ml * l + mr * r, 0.0, 1.0), 1.0);
        return;
    }

    // cardboard: left half of the window = left eye, right half = right eye,
    // each pre-distorted for the viewer's lens, with a soft round vignette
    bool rightHalf = uv.x >= 0.5;
    vec2 eyeUV = vec2(fract(uv.x * 2.0), uv.y);
    bool outside;
    vec2 d = barrel(eyeUV, outside);
    vec3 col = outside ? vec3(0.0) : (rightHalf ? texture(uRight, d * uEyeUV).rgb : texture(uLeft, d * uEyeUV).rgb);
    float v = length((eyeUV - 0.5) * vec2(uAspect, 1.0));
    col *= smoothstep(0.75, 0.45, v);
    if (uTunnel > 0.001) {
        float r = length((eyeUV - 0.5) * vec2(uAspect, 1.0));
        col *= mix(1.0, smoothstep(0.62, 0.32, r), uTunnel);
    }
    if (abs(uv.x - 0.5) < 0.0015) col = vec3(0.0);  // divider line
    FragColor = vec4(col, 1.0);
}
