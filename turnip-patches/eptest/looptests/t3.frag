#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set=2, binding=0) readonly buffer L { uint d[]; } heap[];
layout(set=3, binding=0) uniform U { uint first; uint count; uint stride; uint pad; } u;
layout(location=0) out vec4 o;
// do-while, no outside use: expect 1, unguarded
void main() {
  uint cl = uint(gl_FragCoord.x) & 63u;
  uint n = heap[1].d[cl] & 63u;
  vec4 acc = vec4(0);
  uint i = 0;
  do {
     uint idx = heap[2].d[(cl << 8) + i];
     acc += vec4(uintBitsToFloat(heap[3].d[idx]), 0, 0, 1);
     i++;
  } while (i < n);
  o = acc;
}
