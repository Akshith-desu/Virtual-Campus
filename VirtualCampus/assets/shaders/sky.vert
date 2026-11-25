#version 330 core
// Full-screen triangle (no vertex buffer needed: positions come from gl_VertexID).
out vec2 vNdc;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;   // (-1,-1) (3,-1) (-1,3)
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);   // at the far plane
}
