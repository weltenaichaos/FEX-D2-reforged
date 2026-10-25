#version 450
layout(location = 2) out vec3 n;
layout(location = 3) out vec4 t;
layout(location = 4) out vec2 uv;
layout(location = 5) out vec4 uv1;
void main() {
  float a = float(gl_VertexIndex & 1), b = float(gl_VertexIndex >> 1);
  gl_Position = vec4(a, b, 0.5, 1.0);
  n = vec3(a, b, a + b); t = vec4(b, a, 1.0 - a, a * b); uv = vec2(a, b); uv1 = vec4(a * 0.5, b * 0.25, a, b);
}
