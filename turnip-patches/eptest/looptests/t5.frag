#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=2, binding=0) readonly buffer L { uint d[]; } heap[];
layout(set=3, binding=0) uniform U { uint first; uint count; uint stride; uint pad; } u;
layout(location=0) out vec4 o;
// conditional step: expect 0
void main() {
  uint cl = uint(gl_FragCoord.x) & 63u;
  uint n = heap[1].d[cl] & 63u;
  vec4 acc = vec4(0);
  for (uint i = 0; i < n; ) {
     uint idx = heap[2].d[(cl << 8) + i];
     float v = uintBitsToFloat(heap[3].d[idx]);
     acc += vec4(v, 0, 0, 1);
     if (v > 0.5) i += 2u; else i += 1u;
  }
  o = acc;
}
