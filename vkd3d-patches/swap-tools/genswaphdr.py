import sys, subprocess, struct, os, tempfile
# usage: genswaphdr.py OUT.h DIR name1.comp:array1 name2.comp:array2 ...
out, d = sys.argv[1], sys.argv[2]
parts = ["/* SPIR-V of the FEX shader swap versions (fex_shader_swap.c), compiled with\n"
         " * glslangValidator -V --target-env vulkan1.3 from the .comp files next to this header. */\n"]
for spec in sys.argv[3:]:
    comp, arr = spec.split(':')
    with tempfile.TemporaryDirectory() as t:
        spv = os.path.join(t, 'o.spv')
        subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.3', os.path.join(d, comp), '-o', spv],
                       check=True, stdout=subprocess.DEVNULL)
        b = open(spv, 'rb').read()
    w = struct.unpack('<%dI' % (len(b) // 4), b)
    lines = ['    ' + ', '.join('0x%08x' % x for x in w[i:i + 8]) + ',' for i in range(0, len(w), 8)]
    parts.append("\n/* %s */\nstatic const uint32_t %s[] =\n{\n%s\n};\n" % (comp, arr, '\n'.join(lines)))
open(out, 'w').write(''.join(parts))
