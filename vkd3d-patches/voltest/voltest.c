// Runs D2R's 83e2eb4a6bf5f03b compute shader and FEX's version of it (VKD3D fex30, libs/vkd3d/fex_shader_swap_83e2.comp)
// on the same inputs and compares the outputs: an N^3 RGBA32F output, a random RGBA16F source volume of srcN^3 with all
// mips, A x B samples per cell, the given mip level and address mode, vkd3d-like descriptor set layouts.
//   gcc -O2 -o voltest voltest.c -lvulkan -lm
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./voltest ORIG.spv NEW.spv [N A B lod addr srcN]
// ORIG.spv = vkd3d's translation of the game's shader (FEX shader copy), NEW.spv = glslangValidator -V
// --target-env vulkan1.3 of the .comp. VOL_WHD=W,H,D sets the volume size the shader sees inside the N^3 output.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkCommandPool cpool;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags f) {
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
   printf("no memtype\n"); exit(1);
}
static void *readfile(const char *p, size_t *n) {
   FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
   fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
   void *b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b;
}
static uint16_t f2h(float f) { // float -> half (round to nearest even), values in [0, 4)
   uint32_t x; memcpy(&x, &f, 4); uint32_t s = (x >> 16) & 0x8000; int e = ((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
   if (e <= 0) return s; if (e >= 31) return s | 0x7c00;
   uint32_t h = s | (e << 10) | (m >> 13); uint32_t rem = m & 0x1fff; if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++; return h;
}
static VkCommandBuffer begin(void) {
   VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ai, &cb));
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   CK(vkBeginCommandBuffer(cb, &bi)); return cb;
}
static double submit(VkCommandBuffer cb) {
   CK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
   CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
   clock_gettime(CLOCK_MONOTONIC, &b);
   vkFreeCommandBuffers(dev, cpool, 1, &cb);
   return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
}
static void barrier_img(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t levels) {
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 } };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static VkBuffer mkbuf(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *mem, void **map) {
   VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
   VkBuffer b; CK(vkCreateBuffer(dev, &bi, NULL, &b));
   VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev, b, &r);
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
   CK(vkAllocateMemory(dev, &ai, NULL, mem)); CK(vkBindBufferMemory(dev, b, *mem, 0)); CK(vkMapMemory(dev, *mem, 0, size, 0, map)); return b;
}
static VkImage mkimg(uint32_t n, uint32_t levels, VkFormat fmt, VkImageUsageFlags usage, VkImageView *view) {
   VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D, .format = fmt, .extent = { n, n, n }, .mipLevels = levels, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage img; CK(vkCreateImage(dev, &ii, NULL, &img));
   VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, img, &r);
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = memtype(r.memoryTypeBits, 0) };
   VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, NULL, &m)); CK(vkBindImageMemory(dev, img, m, 0));
   VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_3D, .format = fmt,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 } };
   CK(vkCreateImageView(dev, &vi, NULL, view)); return img;
}

int main(int argc, char **argv) {
   if (argc < 3) { printf("usage: voltest ORIG.spv NEW.spv [N A B lod addr(0 clamp,1 repeat) srcN]\n"); return 1; }
   uint32_t N = argc > 3 ? atoi(argv[3]) : 32, A = argc > 4 ? atoi(argv[4]) : N, B = argc > 5 ? atoi(argv[5]) : N;
   uint32_t lodi = argc > 6 ? atoi(argv[6]) : 1, addr = argc > 7 ? atoi(argv[7]) : 0, SN = argc > 8 ? atoi(argv[8]) : N;
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd) < 0);
   VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mut = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, .mutableDescriptorType = 1 };
   VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &mut };
   VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .runtimeDescriptorArray = 1, .descriptorIndexing = 1,
      .vulkanMemoryModel = 1, .vulkanMemoryModelDeviceScope = 1, .bufferDeviceAddress = 1, .shaderSampledImageArrayNonUniformIndexing = 1 };
   VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12, .features = { .shaderInt64 = 1, .shaderStorageImageWriteWithoutFormat = 1 } };
   float prio = 1; VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
   const char *exts[] = { "VK_EXT_mutable_descriptor_type" };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts };
   CK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, 0, 0, &q);
   VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 }; CK(vkCreateCommandPool(dev, &cpi, NULL, &cpool));

   // Source: SN^3 RGBA16F with all mips, random values in [0, 1); destination N^3 RGBA32F.
   uint32_t levels = 0; for (uint32_t s = SN; s; s >>= 1) levels++;
   VkImageView srcv, dstv;
   VkImage src = mkimg(SN, levels, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &srcv);
   VkImage dst = mkimg(N, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &dstv);
   VkDeviceMemory sm; void *smap; VkDeviceSize stotal = 0;
   for (uint32_t l = 0, s = SN; l < levels; l++, s >>= 1) stotal += (VkDeviceSize)s * s * s * 8;
   VkBuffer sbuf = mkbuf(stotal, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sm, &smap);
   srand(1234); uint16_t *hp = smap;
   for (VkDeviceSize i = 0; i < stotal / 2; i++) hp[i] = f2h((float)rand() / RAND_MAX);
   VkCommandBuffer cb = begin();
   barrier_img(cb, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels);
   VkDeviceSize off = 0;
   for (uint32_t l = 0, s = SN; l < levels; l++, s >>= 1) {
      VkBufferImageCopy c = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, .imageExtent = { s, s, s } };
      vkCmdCopyBufferToImage(cb, sbuf, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c); off += (VkDeviceSize)s * s * s * 8;
   }
   barrier_img(cb, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, levels);
   barrier_img(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 1);
   submit(cb);

   VkSamplerAddressMode am = addr ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = am, .addressModeV = am, .addressModeW = am, .maxLod = 16 };
   VkSampler smp; CK(vkCreateSampler(dev, &sci, NULL, &smp));

   // Layouts like vkd3d's: set 0 samplers, set 1 binding 1 mutable (sampled/storage images), set 2 empty, set 3 binding 7 UBO.
   VkDescriptorType mtypes[] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
   VkMutableDescriptorTypeListEXT ml[2] = { { 0, NULL }, { 2, mtypes } };
   VkMutableDescriptorTypeCreateInfoEXT mci = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, .mutableDescriptorTypeListCount = 2, .pMutableDescriptorTypeLists = ml };
   VkDescriptorSetLayoutBinding b0 = { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 8, VK_SHADER_STAGE_COMPUTE_BIT };
   VkDescriptorSetLayoutBinding b1[2] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT }, { 1, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 4, VK_SHADER_STAGE_COMPUTE_BIT } };
   VkDescriptorSetLayoutBinding b3 = { 7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
   VkDescriptorSetLayout sl[4];
   VkDescriptorSetLayoutCreateInfo lci0 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b0 };
   VkDescriptorSetLayoutCreateInfo lci1 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &mci, .bindingCount = 2, .pBindings = b1 };
   VkDescriptorSetLayoutCreateInfo lci2 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   VkDescriptorSetLayoutCreateInfo lci3 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b3 };
   CK(vkCreateDescriptorSetLayout(dev, &lci0, NULL, &sl[0])); CK(vkCreateDescriptorSetLayout(dev, &lci1, NULL, &sl[1]));
   CK(vkCreateDescriptorSetLayout(dev, &lci2, NULL, &sl[2])); CK(vkCreateDescriptorSetLayout(dev, &lci3, NULL, &sl[3]));
   VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 28 };
   VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 4, .pSetLayouts = sl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
   VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 9 }, { VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 4 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
   VkMutableDescriptorTypeListEXT pml[3] = { { 0, NULL }, { 2, mtypes }, { 0, NULL } };
   VkMutableDescriptorTypeCreateInfoEXT pmci = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, .mutableDescriptorTypeListCount = 3, .pMutableDescriptorTypeLists = pml };
   VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .pNext = &pmci, .maxSets = 4, .poolSizeCount = 3, .pPoolSizes = ps };
   VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
   VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 4, .pSetLayouts = sl };
   VkDescriptorSet ds[4]; CK(vkAllocateDescriptorSets(dev, &dai, ds));
   VkDeviceMemory um; void *umap; VkBuffer ubuf = mkbuf(128, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &um, &umap);
   uint32_t *u = umap; memset(u, 0, 128);
   u[24] = N; u[25] = N; u[26] = N;     // vec4[6].xyz = output dims (ints); VOL_WHD=W,H,D sets them inside N^3
   if (getenv("VOL_WHD")) sscanf(getenv("VOL_WHD"), "%u,%u,%u", &u[24], &u[25], &u[26]);
   u[28] = A; u[30] = B; u[23] = lodi;  // float[28], float[30] as ints, float[23] lod as uint
   float ep = 1.5f; memcpy(&u[17], &ep, 4); // float[17]
   VkDescriptorImageInfo sii[8]; for (int i = 0; i < 8; i++) sii[i] = (VkDescriptorImageInfo){ .sampler = smp };
   VkDescriptorImageInfo srci = { .imageView = srcv, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
   VkDescriptorImageInfo dsti = { .imageView = dstv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
   VkDescriptorBufferInfo ubi = { ubuf, 0, 128 };
   VkWriteDescriptorSet w[4] = {
      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds[0], .dstBinding = 0, .descriptorCount = 8, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = sii },
      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds[1], .dstBinding = 1, .dstArrayElement = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &srci },
      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds[1], .dstBinding = 1, .dstArrayElement = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dsti },
      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds[3], .dstBinding = 7, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ubi } };
   vkUpdateDescriptorSets(dev, 4, w, 0, NULL);
   uint32_t pc[7] = { 0, 0, 0, 3 /* dst */, 2 /* src */, 1 /* sampler index - 5 */, 0 };

   VkDeviceMemory rm; void *rmap; VkDeviceSize rsize = (VkDeviceSize)N * N * N * 16;
   VkBuffer rbuf = mkbuf(rsize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rm, &rmap);
   float *res[2]; double ms[2];
   for (int s = 0; s < 2; s++) {
      size_t len; void *code = readfile(argv[1 + s], &len);
      VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = len, .pCode = code };
      VkShaderModule mod; CK(vkCreateShaderModule(dev, &smci, NULL, &mod));
      VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" }, .layout = pl };
      VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
      cb = begin();
      VkClearColorValue cc = { .float32 = { -7, -7, -7, -7 } }; VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
      barrier_img(cb, dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 1);
      submit(cb);
      cb = begin();
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 4, ds, 0, NULL);
      vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 28, pc);
      vkCmdDispatch(cb, (N + 7) / 8, (N + 7) / 8, (N + 7) / 8);
      ms[s] = submit(cb);
      cb = begin();
      barrier_img(cb, dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1);
      VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { N, N, N } };
      vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &c);
      barrier_img(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, 1);
      submit(cb);
      res[s] = malloc(rsize); memcpy(res[s], rmap, rsize);
   }
   double maxabs = 0, maxrel = 0, sum = 0; size_t bad = 0, unwritten = 0, cnt = (size_t)N * N * N * 4, worst = 0;
   for (size_t i = 0; i < cnt; i++) {
      double a = res[0][i], b = res[1][i], d = fabs(a - b);
      if ((a == -7) != (b == -7) || isnan(a) != isnan(b)) bad++;
      if (a == -7) unwritten++;
      if (d > maxabs) { maxabs = d; worst = i; }
      double r = d / fmax(fabs(a), 1e-6); if (r > maxrel) maxrel = r;
      sum += fabs(a);
   }
   printf("N=%u A=%u B=%u lod=%u addr=%s srcN=%u: orig %.1f ms, new %.1f ms | max abs diff %.3g (at %zu: %.7g vs %.7g), max rel %.3g, mean |orig| %.4g, unwritten/nan mismatches %zu (unwritten in both %zu) whd %u,%u,%u\n",
          N, A, B, lodi, addr ? "repeat" : "clamp", SN, ms[0], ms[1], maxabs, worst, res[0][worst], res[1][worst], maxrel, sum / cnt, bad, unwritten, u[24], u[25], u[26]);
   return 0;
}
