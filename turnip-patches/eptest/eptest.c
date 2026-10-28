// Compiles a D2R lighting pixel shader (vkd3d SPIR-V, e.g. from FEX_VKD3D_SHADER_COPY) with vkd3d-like descriptor
// set layouts on Turnip (noop drm shim, a740) and prints the driver's statistics for each stage (early preamble etc.).
//   glslangValidator -V vs.vert -o vs.spv && cc -O1 -o eptest eptest.c -lvulkan
//   VK_ICD_FILENAMES=<freedreno_icd.json of a host Mesa build> \
//   LD_PRELOAD=<build>/src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so FD_GPU_ID=740 MESA_SHADER_CACHE_DISABLE=1 \
//   [EPTEST_DB=1] [EPTEST_FEX=<bits>] [EPTEST_EXT=<ext_size>] ./eptest vs.spv FS.spv [ir]
// EPTEST_DB=1: descriptor buffer layouts as vkd3d-proton makes them on the device (DESCRIPTOR_BUFFER_BIT on every set,
// heaps VARIABLE_DESCRIPTOR_COUNT only, push set PUSH_DESCRIPTOR|DESCRIPTOR_BUFFER). EPTEST_FEX=<compile bits>: hands
// the driver a FEX link block (tu_fex.h) with those compile bits. 'ir' writes the driver's IR to ir_e*_*.txt. EPTEST_EXT (fex11+): channel
// extension size (0x358 for fex12); with fex12 prints the no-alias statistics of both fragment shader binaries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static void *readfile(const char *p, size_t *n) {
   FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
   fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
   void *b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b;
}

int main(int argc, char **argv) {
   if (argc < 3) { printf("usage: eptest VS.spv FS.spv [ir]\n"); return 1; }
   int dump_ir = argc > 3;
   static uint32_t chan[0x400 / 4] __attribute__((aligned(64)));
   if (getenv("EPTEST_FEX")) {
      /* FEX link block (tu_fex.h): magic, version 1, size, no counters, compile bits at 0x18 */
      chan[0] = 0x58465554u; chan[1] = 1; chan[2] = 0x2a8; chan[6] = (uint32_t)strtoul(getenv("EPTEST_FEX"), NULL, 0);
      chan[7] = getenv("EPTEST_EXT") ? (uint32_t)strtoul(getenv("EPTEST_EXT"), NULL, 0) : 0; /* fex11+: ext_size */
      char v[32]; snprintf(v, sizeof v, "%llx", (unsigned long long)(uintptr_t)chan); setenv("TU_FEX_CHANNEL", v, 1);
   }
   VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n) { printf("no device\n"); return 1; }

   int db = getenv("EPTEST_DB") != NULL;
   VkPhysicalDeviceDescriptorBufferFeaturesEXT dbf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
      .descriptorBuffer = 1, .descriptorBufferPushDescriptors = 1 };
   VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mut = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, .pNext = db ? &dbf : NULL, .mutableDescriptorType = 1 };
   VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pep = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pNext = &mut, .pipelineExecutableInfo = 1 };
   VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &pep, .dynamicRendering = 1 };
   VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13,
      .descriptorIndexing = 1, .runtimeDescriptorArray = 1, .descriptorBindingPartiallyBound = 1,
      .descriptorBindingVariableDescriptorCount = 1, .descriptorBindingUpdateUnusedWhilePending = 1,
      .descriptorBindingSampledImageUpdateAfterBind = 1, .descriptorBindingStorageBufferUpdateAfterBind = 1,
      .descriptorBindingUniformTexelBufferUpdateAfterBind = 1, .descriptorBindingStorageImageUpdateAfterBind = 1,
      .shaderSampledImageArrayNonUniformIndexing = 1, .bufferDeviceAddress = 1, .vulkanMemoryModel = 1,
      .vulkanMemoryModelDeviceScope = 1, .shaderFloat16 = 0 };
   VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
   f2.features.shaderInt64 = 1;
   float prio = 1;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   const char *exts[] = { "VK_KHR_push_descriptor", "VK_EXT_mutable_descriptor_type", "VK_KHR_pipeline_executable_properties", "VK_EXT_descriptor_buffer" };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = db ? 4 : 3, .ppEnabledExtensionNames = exts };
   VkDevice dev; CK(vkCreateDevice(pd, &dci, NULL, &dev));

   // vkd3d-like layouts: set 0 sampler heap, set 1 mutable resource heap (binding 1), set 2 raw SSBO heap,
   // set 3 = root descriptors as push descriptors (18 UBOs, no binding flags).
   VkDescriptorSetLayout sl[4];
   VkDescriptorBindingFlags heapflags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
   if (db) heapflags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
   VkDescriptorSetLayoutCreateFlags heaplayout = db ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
   {
      VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 2048, VK_SHADER_STAGE_ALL };
      VkDescriptorSetLayoutBindingFlagsCreateInfo bf = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 1, .pBindingFlags = &heapflags };
      VkDescriptorSetLayoutCreateInfo c = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bf,
         .flags = heaplayout, .bindingCount = 1, .pBindings = &b };
      CK(vkCreateDescriptorSetLayout(dev, &c, NULL, &sl[0]));
   }
   {
      VkDescriptorType types[] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
      VkMutableDescriptorTypeListEXT lists[2] = { { 0, NULL }, { 4, types } };
      VkMutableDescriptorTypeCreateInfoEXT mi = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, .mutableDescriptorTypeListCount = 2, .pMutableDescriptorTypeLists = lists };
      VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, VK_SHADER_STAGE_ALL },
                                            { 1, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 500000, VK_SHADER_STAGE_ALL } };
      VkDescriptorBindingFlags fl[2] = { 0, heapflags };
      VkDescriptorSetLayoutBindingFlagsCreateInfo bf = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .pNext = &mi, .bindingCount = 2, .pBindingFlags = fl };
      VkDescriptorSetLayoutCreateInfo c = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bf,
         .flags = heaplayout, .bindingCount = 2, .pBindings = b };
      CK(vkCreateDescriptorSetLayout(dev, &c, NULL, &sl[1]));
   }
   {
      VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 500000, VK_SHADER_STAGE_ALL };
      VkDescriptorSetLayoutBindingFlagsCreateInfo bf = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 1, .pBindingFlags = &heapflags };
      VkDescriptorSetLayoutCreateInfo c = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bf,
         .flags = heaplayout, .bindingCount = 1, .pBindings = &b };
      CK(vkCreateDescriptorSetLayout(dev, &c, NULL, &sl[2]));
   }
   {
      VkDescriptorSetLayoutBinding b[18];
      for (int i = 0; i < 18; i++) b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL };
      VkDescriptorSetLayoutCreateInfo c = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR | (db ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0), .bindingCount = 18, .pBindings = b };
      CK(vkCreateDescriptorSetLayout(dev, &c, NULL, &sl[3]));
   }
   VkPushConstantRange pcr = { VK_SHADER_STAGE_ALL, 0, 52 };
   VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 4, .pSetLayouts = sl,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
   VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

   size_t vn, fn; void *vb = readfile(argv[1], &vn), *fb = readfile(argv[2], &fn);
   VkShaderModuleCreateInfo smv = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vn, .pCode = vb };
   VkShaderModuleCreateInfo smf = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fn, .pCode = fb };
   VkShaderModule vm, fm; CK(vkCreateShaderModule(dev, &smv, NULL, &vm)); CK(vkCreateShaderModule(dev, &smf, NULL, &fm));
   VkPipelineShaderStageCreateInfo st[2] = {
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
   VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, 1178, 662, 0, 1 }; VkRect2D sc = { {0, 0}, {1178, 662} };
   VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .cullMode = VK_CULL_MODE_BACK_BIT, .lineWidth = 1 };
   VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = 1 };
   VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = 1,
      .depthCompareOp = VK_COMPARE_OP_EQUAL, .stencilTestEnable = 1,
      .front = { .compareOp = VK_COMPARE_OP_EQUAL, .compareMask = 2, .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP, .depthFailOp = VK_STENCIL_OP_KEEP },
      .back = { .compareOp = VK_COMPARE_OP_EQUAL, .compareMask = 2 } };
   VkPipelineColorBlendAttachmentState cba[2] = { { .colorWriteMask = 0xf }, { .colorWriteMask = 0xf } };
   VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 2, .pAttachments = cba };
   VkFormat cf[2] = { VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM };
   VkPipelineRenderingCreateInfo pr = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 2, .pColorAttachmentFormats = cf,
      .depthAttachmentFormat = VK_FORMAT_D24_UNORM_S8_UINT, .stencilAttachmentFormat = VK_FORMAT_D24_UNORM_S8_UINT };
   VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pr,
      .flags = (db ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0) | VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | (dump_ir ? VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR : 0),
      .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
      .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .layout = pl };
   VkPipeline p; CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &p));

   PFN_vkGetPipelineExecutablePropertiesKHR gpe = (void *)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR");
   PFN_vkGetPipelineExecutableStatisticsKHR gps = (void *)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR");
   PFN_vkGetPipelineExecutableInternalRepresentationsKHR gir = (void *)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableInternalRepresentationsKHR");
   VkPipelineInfoKHR pi = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = p };
   uint32_t ne = 0; gpe(dev, &pi, &ne, NULL);
   VkPipelineExecutablePropertiesKHR ep[8]; for (uint32_t i = 0; i < 8; i++) ep[i] = (VkPipelineExecutablePropertiesKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR };
   if (ne > 8) ne = 8; gpe(dev, &pi, &ne, ep);
   for (uint32_t e = 0; e < ne; e++) {
      printf("executable %u: %s\n", e, ep[e].name);
      VkPipelineExecutableInfoKHR ei = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = p, .executableIndex = e };
      uint32_t ns = 0; gps(dev, &ei, &ns, NULL);
      VkPipelineExecutableStatisticKHR *s = calloc(ns, sizeof(*s));
      for (uint32_t i = 0; i < ns; i++) s[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
      gps(dev, &ei, &ns, s);
      for (uint32_t i = 0; i < ns; i++) {
         if (s[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR) printf("  %s = %s\n", s[i].name, s[i].value.b32 ? "true" : "false");
         else if (s[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) printf("  %s = %llu\n", s[i].name, (unsigned long long)s[i].value.u64);
      }
      if (dump_ir) {
         uint32_t nr = 0; gir(dev, &ei, &nr, NULL);
         VkPipelineExecutableInternalRepresentationKHR *r = calloc(nr, sizeof(*r));
         for (uint32_t i = 0; i < nr; i++) r[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
         gir(dev, &ei, &nr, r);
         for (uint32_t i = 0; i < nr; i++) { r[i].pData = malloc(r[i].dataSize); }
         gir(dev, &ei, &nr, r);
         for (uint32_t i = 0; i < nr; i++) {
            char fname[64]; snprintf(fname, sizeof fname, "ir_e%u_%u.txt", e, i);
            FILE *f = fopen(fname, "w"); fprintf(f, "%s\n%s", r[i].name, (char *)r[i].pData); fclose(f);
            printf("  ir %s -> %s (%zu bytes)\n", r[i].name, fname, r[i].dataSize);
         }
      }
   }
   if (getenv("EPTEST_FEX")) printf("fex link: status 0x%x patch %u driver '%s' loop prefetch loads %u shaders %u\n", chan[0x1b0 / 4], chan[0x1b4 / 4], (char *)&chan[0x1c0 / 4], chan[0x1a8 / 4], chan[0x1ac / 4]);
   if (getenv("EPTEST_EXT") && chan[7] >= 0x350) {
      /* fex12 no-alias statistics (tu_fex_channel::na_*) */
      const uint32_t *n = &chan[0x310 / 4];
      printf("fex12 no-alias: shaders %u refused %u | instr %u -> %u, nops %u -> %u, alias %u -> %u, movs %u -> %u, regs %u -> %u, ss stall %u -> %u, sy stall %u -> %u\n",
             n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9], n[10], n[11], n[12], n[13], n[14], n[15]);
   }
   return 0;
}
