// Compiles a D2R compute shader (vkd3d SPIR-V) with vkd3d-like descriptor set layouts on Turnip
// (noop drm shim, a740) and prints the driver's statistics for each stage (early preamble etc.).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "tu_fex.h"

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static void *readfile(const char *p, size_t *n) {
   FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
   fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
   void *b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b;
}

int main(int argc, char **argv) {
   if (argc < 2) { printf("usage: cstest CS.spv [ir]\n"); return 1; }
   int dump_ir = argc > 2;
   struct tu_fex_channel *chan = aligned_alloc(64, sizeof(*chan));
   memset(chan, 0, sizeof(*chan));
   chan->magic = TU_FEX_MAGIC; chan->version = TU_FEX_VERSION; chan->size = TU_FEX_CHANNEL_BASE_SIZE; chan->ext_size = sizeof(*chan);
   chan->compile = (uint32_t)strtoul(getenv("EPTEST_FEX") ? getenv("EPTEST_FEX") : "0x101f", NULL, 0);
   chan->sample_min_draws = 100;
   { char v[32]; snprintf(v, sizeof v, "%llx", (unsigned long long)(uintptr_t)chan); setenv("TU_FEX_CHANNEL", v, 1); }
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

   size_t cn; void *cbuf = readfile(argv[1], &cn);
   VkShaderModuleCreateInfo smc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = cn, .pCode = cbuf };
   VkShaderModule cm; CK(vkCreateShaderModule(dev, &smc, NULL, &cm));
   VkComputePipelineCreateInfo cp = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .flags = (db ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0) | VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | (dump_ir ? VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR : 0),
      .stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cm, .pName = "main" },
      .layout = pl };
   VkPipeline p; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &p));

   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
   PFN_vkCmdBindDescriptorBuffersEXT bindDB = (void *)vkGetDeviceProcAddr(dev, "vkCmdBindDescriptorBuffersEXT");
   PFN_vkCmdSetDescriptorBufferOffsetsEXT setOff = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetDescriptorBufferOffsetsEXT");
   VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandPool cpool; CK(vkCreateCommandPool(dev, &cpci, NULL, &cpool));
   VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .commandBufferCount = 2 };
   VkCommandBuffer cmd[2]; CK(vkAllocateCommandBuffers(dev, &cbai, cmd));
   uint32_t modes[2] = { 0, TU_FEX_MODE_CS_ALT };
   for (int k = 0; k < 2; k++) {
      chan->mode = modes[k];
      VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      CK(vkBeginCommandBuffer(cmd[k], &cbbi));
      vkCmdBindPipeline(cmd[k], VK_PIPELINE_BIND_POINT_COMPUTE, p);
      uint32_t pc[13]; for (int i = 0; i < 13; i++) pc[i] = 0x100 * (k + 1) + i;
      vkCmdPushConstants(cmd[k], pl, VK_SHADER_STAGE_ALL, 0, 52, pc);
      vkCmdDispatch(cmd[k], 144, 144, 1);
      vkCmdDispatch(cmd[k], 2, 2, 1); /* too small for a slot */
      CK(vkEndCommandBuffer(cmd[k]));
   }
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 2, .pCommandBuffers = cmd };
   CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CK(vkQueueWaitIdle(q));
   printf("status 0x%x patch %u driver '%s'\n", chan->status, chan->patch, chan->driver);
   printf("cs binds stock %llu alt %llu | alt shaders %u atomics %u instr %u -> %u refused %u | dispatches sampled %llu | ring next %llu\n",
      (unsigned long long)chan->cs_binds[0], (unsigned long long)chan->cs_binds[1], chan->cs_alt_shaders, chan->cs_alt_atomics,
      chan->cs_alt_instr[0], chan->cs_alt_instr[1], chan->cs_alt_refused, (unsigned long long)chan->dispatches_sampled,
      (unsigned long long)chan->ring_next);
   for (uint64_t sq = 1; sq < chan->ring_next; sq++) {
      struct tu_fex_slot *sl = (struct tu_fex_slot *)((uint8_t *)(uintptr_t)chan->ring_cpu + (sq & (chan->ring_count - 1)) * chan->slot_size);
      printf("slot %llu: draws %u %ux%u colors %u attachments 0x%x sysmem %u mode 0x%x format0 0x%x -> local %ux%ux%u\n",
         (unsigned long long)sl->seq, sl->draws, sl->width, sl->height, sl->colors, sl->attachments, sl->sysmem, sl->mode, sl->format0,
         (sl->format0 & 1023) + 1, ((sl->format0 >> 10) & 1023) + 1, ((sl->format0 >> 20) & 1023) + 1);
   }
   (void)bindDB; (void)setOff;
   return 0;
}
