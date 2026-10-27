#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=2, binding=0) readonly buffer L { uint d[]; } heap[];
layout(set=3, binding=0) uniform U { uint first; uint count; uint stride; uint pad; } u;
layout(location=0) out vec4 o;
// nested lists: expect 2 (inner and outer)
void main() {
  uint cl = uint(gl_FragCoord.x) & 63u;
  uint n = heap[1].d[cl] & 15u;
  vec4 acc = vec4(0);
  for (uint i = 0; i < n; i++) {
     uint grp = heap[2].d[(cl << 8) + i];
     uint m = grp & 7u;
     for (uint j = 0; j < m; j++) {
        uint idx = heap[4].d[(grp >> 3) + j];
        acc += vec4(uintBitsToFloat(heap[3].d[idx]), 0, 0, 1);
     }
  }
  o = acc;
}
