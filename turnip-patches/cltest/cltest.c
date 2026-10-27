// Host test for Turnip fex3/fex4 (TU_FEX_MODE_DEPTH_CLEAR_3D, _FASTZ) on the freedreno noop drm shim.
// Case A: depth-only D16 WxH dynamic rendering pass, depth CLEAR + STORE, one VS-only draw.
// Case B: RGBA8 color CLEAR + D24S8 with depth CLEAR and stencil LOAD, one draw.
// Prints the channel's depth clear statistics and the sampled slots (mode 0x80 = the pass clears depth); the
// command stream is checked with FD_RD_DUMP + cffdump: mode 0 clears both depth attachments with CP_BLIT (2D
// engine), mode 4 with a DI_PT_RECTLIST draw (RB_DEPTH_CNTL Z_WRITE_ENABLE, ZFUNC ALWAYS) inside
// CP_COND_REG_EXEC RENDER_MODE|SYSMEM, after RB_DEPTH_BUFFER_INFO is set; mode 12 (fex4) the same draw with
// RB_RENDER_CNTL / GRAS_SU_RENDER_CNTL / SP_RENDER_CNTL FS_DISABLE set before it.
// fex8: mode 4 clears case B's color in the same draw as its depth (no CP_BLIT, "fast z 1" = one color clear by
// the draw); mode 12 clears color with CP_BLIT and depth with the draw (with fragment shader, no FS_DISABLE).
// Build (tu_fex.h from a Mesa tree with the patches):
//   glslangValidator -V --vn tri_vert_spv -o tri_vert_spv.h tri.vert
//   glslangValidator -V --vn col_frag_spv -o col_frag_spv.h col.frag
//   cc -O1 -I<mesa>/src/freedreno/vulkan -o cltest cltest.c -lvulkan
// Run: VK_ICD_FILENAMES=<freedreno_icd.json of a host Mesa build>
//   LD_PRELOAD=<build>/src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so FD_GPU_ID=740 TU_DEBUG=sysmem
//   FD_RD_DUMP=full FD_RD_DUMP_PATH=<dir> ./cltest <mode 0|4|12> [size, default 256]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "tu_fex.h"
#include "tri_vert_spv.h"
#include "col_frag_spv.h"

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static VkPhysicalDevice pd;
static VkDevice dev;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags f) {
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
   printf("no memtype\n"); exit(1);
}

static VkImageView mkimage(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
   VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
      .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
   VkImage img; CK(vkCreateImage(dev, &ii, NULL, &img));
   VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) };
   VkDeviceMemory m; CK(vkAllocateMemory(dev, &mai, NULL, &m)); CK(vkBindImageMemory(dev, img, m, 0));
   VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = fmt, .subresourceRange = { aspect, 0, 1, 0, 1 } };
   VkImageView iv; CK(vkCreateImageView(dev, &ivci, NULL, &iv));
   return iv;
}

static VkPipeline mkpipe(VkPipelineLayout pl, VkFormat color, VkFormat depth, int stencil, uint32_t w, uint32_t h) {
   VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(tri_vert_spv), .pCode = tri_vert_spv };
   VkShaderModule vs, fs; CK(vkCreateShaderModule(dev, &smci, NULL, &vs));
   smci.codeSize = sizeof(col_frag_spv); smci.pCode = col_frag_spv; CK(vkCreateShaderModule(dev, &smci, NULL, &fs));
   VkPipelineShaderStageCreateInfo st[2] = {
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
   VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, w, h, 0, 1 }; VkRect2D sc = { {0, 0}, {w, h} };
   VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1 };
   VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = 1 };
   VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = 1, .depthWriteEnable = 1, .depthCompareOp = VK_COMPARE_OP_LESS, .stencilTestEnable = stencil,
      .front = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xff, 0, 0 },
      .back = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xff, 0, 0 } };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = color ? 1 : 0, .pAttachments = &cba };
   VkPipelineRenderingCreateInfo pri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = color ? 1 : 0,
      .pColorAttachmentFormats = &color, .depthAttachmentFormat = depth, .stencilAttachmentFormat = stencil ? depth : VK_FORMAT_UNDEFINED };
   VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pri, .stageCount = color ? 2 : 1, .pStages = st,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .layout = pl };
   VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
   return pipe;
}

int main(int argc, char **argv) {
   uint32_t mode = argc > 1 ? atoi(argv[1]) : 4;
   uint32_t W = argc > 2 ? atoi(argv[2]) : 256, H = W;
   struct tu_fex_channel *chan = aligned_alloc(64, sizeof(*chan));
   memset(chan, 0, sizeof(*chan));
   chan->magic = TU_FEX_MAGIC; chan->version = TU_FEX_VERSION; chan->size = sizeof(*chan);
   chan->sample_min_draws = 1; chan->ring_slots = 64;
   chan->mode = mode;
   char buf[64]; snprintf(buf, sizeof buf, "%llx", (unsigned long long)(uintptr_t)chan);
   setenv("TU_FEX_CHANNEL", buf, 1);

   VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n) { printf("no device\n"); return 1; }
   float prio = 1;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = 1 };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   CK(vkCreateDevice(pd, &dci, NULL, &dev));
   printf("status 0x%x patch %u driver '%s'\n", chan->status, chan->patch, chan->driver);
   VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

   VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkImageView d16 = mkimage(VK_FORMAT_D16_UNORM, W, H, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
   VkImageView col = mkimage(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
   VkImageView d24 = mkimage(VK_FORMAT_D24_UNORM_S8_UINT, 64, 64, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
   VkPipeline pa = mkpipe(pl, VK_FORMAT_UNDEFINED, VK_FORMAT_D16_UNORM, 0, W, H);
   VkPipeline pb = mkpipe(pl, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D24_UNORM_S8_UINT, 1, 64, 64);

   VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
   VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .commandBufferCount = 1 };
   VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CK(vkBeginCommandBuffer(cmd, &cbbi));

   // Case A: depth-only D16, clear to 1.0
   VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = d16,
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .depthStencil = { 1.0f, 0 } } };
   VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { {0, 0}, {W, H} }, .layerCount = 1, .pDepthAttachment = &da };
   vkCmdBeginRendering(cmd, &ri);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pa);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRendering(cmd);

   // Case B: color clear + D24S8 depth clear 0.5, stencil load
   VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = col,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
   VkRenderingAttachmentInfo za = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = d24,
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .depthStencil = { 0.5f, 0 } } };
   VkRenderingAttachmentInfo sa = za; sa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
   VkRenderingInfo rib = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { {0, 0}, {64, 64} }, .layerCount = 1,
      .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &za, .pStencilAttachment = &sa };
   vkCmdBeginRendering(cmd, &rib);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pb);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRendering(cmd);

   CK(vkEndCommandBuffer(cmd));
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
   CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CK(vkQueueWaitIdle(q));
   printf("mode %u: depth clears 2d %llu (%llu px) 3d %llu (%llu px) of them fast z %llu (%llu px), cmdbufs %llu\n", mode,
          (unsigned long long)chan->depth_clears[0], (unsigned long long)chan->depth_clear_px[0],
          (unsigned long long)chan->depth_clears[1], (unsigned long long)chan->depth_clear_px[1],
          (unsigned long long)chan->depth_clears_fastz, (unsigned long long)chan->depth_clear_px_fastz,
          (unsigned long long)(chan->cmdbufs[0] + chan->cmdbufs[1]));
   for (int i = 1; i <= 2 && chan->ring_cpu; i++) {
      struct tu_fex_slot *sl = (struct tu_fex_slot *)(uintptr_t)(chan->ring_cpu + i * chan->slot_size);
      printf("slot%d: seq %llu draws %u %ux%u mode 0x%x sysmem %u colors %u att %u fmt %u\n", i,
             (unsigned long long)sl->seq, sl->draws, sl->width, sl->height, sl->mode, sl->sysmem, sl->colors,
             sl->attachments, sl->format0);
   }
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return 0;
}
