/* Vulkan renderer for the guest's 3D scene submission (docs/pc-rendering-design.md section 9).

   Input is plain data (triangles + resolved textures), with no dependency on the guest stack.
   Guest transform and clipping are already done; the vertex shader can replay the final projection.
   Rasterization follows the algorithm tools/ref_raster.py verified pixel-exact for kinds 0 and 6:
   reciprocal-depth test (strictly greater passes), 8-bit palette texels, colour key 0xF81F, texture
   coordinates interpolated as (u*z, v*z, z) and divided per pixel (kind 1: affine, linear address).
   Kind 4 adds a biased depth test/write. Kind 5 tests biased depth without writing it, and blends
   direct RGB565 against a pre-draw colour snapshot in submission order.
   Vulkan 1.0 core only. The caller supplies the device; nothing here touches the swapchain. */
#ifndef VK_SCENE_H
#define VK_SCENE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
/* tools/vk_scene_spirv.h must precede this header (the generated unit inlines both; the test includes both). */

#define VKS_CHECK(call) do { VkResult vks_r = (call); if (vks_r != VK_SUCCESS) { \
    fprintf(stderr, "vk_scene: %s failed (%d)\n", #call, (int)vks_r); goto fail; } } while (0)

typedef struct {
    uint32_t address, width, height;
    const uint16_t *palette;
    const uint8_t *texels;
    const uint16_t *direct, *weights; /* kind 5 only: RGB565 and u16 weights (0..256) */
} vk_scene_texture;
/* When valid, projecting each stored x, y with its inverse factor and center reproduces v.xy. */
typedef struct { int32_t xy_inv[3][3], center[2]; int valid; } vk_scene_projection;

/* v[i] = x, y, idx, u, v, w5 exactly as the guest rasterizer receives them (post projection). */
typedef struct {
    int32_t v[3][6];
    uint32_t texture, kind;
    uint8_t alpha;
    vk_scene_projection projection;
} vk_scene_tri;
typedef struct {
    uint8_t *rgba;
    const float *depth;
    const uint8_t *kind6_rgba;
    const float *kind6_depth;
} vk_scene_layers;

typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; } vks_buffer;
typedef struct { VkImage image; VkDeviceMemory memory; VkImageView view; } vks_image;

#define VKS_TEXTURE_CACHE_SIZE 256
typedef struct {
    vks_image texels, palette;
    VkDescriptorSet set;
    uint32_t address, width, height;
    int direct;
    uint64_t hash, last_used;
} vks_cached_texture;

typedef struct {
    vks_cached_texture *entry;
    uint64_t hash;
    int used, upload, reuse;
} vks_texture_use;
typedef struct { uint32_t first, texture, kind, count, triangle; uint8_t alpha; } vks_run;

typedef struct {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    vks_buffer vertices;
    VkDeviceSize capacity;
    uint32_t max_range, max_groups;
} vks_transform;

typedef struct {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    uint32_t guest_width, guest_height, scale, width, height;
    vks_image colour, depth;
    VkRenderPass pass, load_pass; VkFramebuffer framebuffer;
    VkDescriptorSetLayout set_layout, snapshot_layout;
    VkPipelineLayout pipeline_layout; VkPipeline pipeline, blend_pipeline;
    vks_image snapshot;
    VkDescriptorPool snapshot_pool;
    VkDescriptorSet snapshot_set;
    VkSampler sampler; VkCommandPool pool; VkFence fence;
    VkCommandBuffer commands;
    VkDescriptorPool texture_pool;
    vks_cached_texture texture_cache[VKS_TEXTURE_CACHE_SIZE];
    uint64_t texture_clock;
    uint32_t texture_uploads; /* textures uploaded by the last successful render, including transient ones */
    vks_buffer vertices, staging;
    VkDeviceSize vertex_capacity, staging_capacity;
    vks_texture_use *texture_uses;
    vks_run *runs;
    uint32_t texture_use_capacity, run_capacity;
    vks_buffer readback; /* RGBA8, width*height, row 0 = top */
    vks_buffer layers_readback; /* primary D32, kind-6 RGBA8, kind-6 D32 */
    VkDeviceSize layers_capacity;
    vks_transform transform;
} vk_scene;

typedef struct { int32_t xy_inv[3]; float abz[3]; } vks_vertex;
typedef struct {
    float view[2];
    int32_t tex[2], kind;
    float shift, alpha;
    int32_t projected, center[2];
} vks_push;
_Static_assert(offsetof(vks_push, projected) == 28 && offsetof(vks_push, center) == 32 &&
               sizeof(vks_push) == 40, "scene shader push constant layout");

static int vks_projection_matches(const vk_scene_projection *a, const vk_scene_projection *b) {
    if (!a->valid) return !b->valid;
    return b->valid && a->center[0] == b->center[0] && a->center[1] == b->center[1];
}

static int vks_memory_type(const vk_scene *s, uint32_t bits, VkMemoryPropertyFlags wanted, uint32_t *index) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(s->physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) { *index = i; return 1; }
    return 0;
}

static void vks_buffer_destroy(const vk_scene *s, vks_buffer *b) {
    if (b->mapped) vkUnmapMemory(s->device, b->memory);
    if (b->buffer) vkDestroyBuffer(s->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(s->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

static int vks_buffer_create(const vk_scene *s, vks_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage) {
    memset(b, 0, sizeof *b);
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKS_CHECK(vkCreateBuffer(s->device, &info, NULL, &b->buffer));
    VkMemoryRequirements need;
    vkGetBufferMemoryRequirements(s->device, b->buffer, &need);
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = need.size;
    /* Readback buffers are read by the CPU every frame: uncached (write-combined) host memory makes that
       an order of magnitude slower on discrete GPUs, so prefer HOST_CACHED when it is also coherent. */
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!((usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) &&
          vks_memory_type(s, need.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &alloc.memoryTypeIndex)) &&
        !vks_memory_type(s, need.memoryTypeBits, host, &alloc.memoryTypeIndex)) {
        fprintf(stderr, "vk_scene: no host-visible memory\n"); goto fail;
    }
    VKS_CHECK(vkAllocateMemory(s->device, &alloc, NULL, &b->memory));
    VKS_CHECK(vkBindBufferMemory(s->device, b->buffer, b->memory, 0));
    VKS_CHECK(vkMapMemory(s->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped));
    return 1;
fail:
    vks_buffer_destroy(s, b);
    return 0;
}

static int vks_buffer_reserve(const vk_scene *s, vks_buffer *b, VkDeviceSize *capacity,
                              VkDeviceSize size, VkBufferUsageFlags usage) {
    if (size <= *capacity) return 1;
    VkDeviceSize grown = *capacity + *capacity / 2;
    vks_buffer_destroy(s, b);
    *capacity = 0;
    if (grown < size) grown = size;
    if (!vks_buffer_create(s, b, grown, usage)) return 0;
    *capacity = grown;
    return 1;
}

static void vks_image_destroy(const vk_scene *s, vks_image *i) {
    if (i->view) vkDestroyImageView(s->device, i->view, NULL);
    if (i->image) vkDestroyImage(s->device, i->image, NULL);
    if (i->memory) vkFreeMemory(s->device, i->memory, NULL);
    memset(i, 0, sizeof *i);
}

static int vks_image_create(const vk_scene *s, vks_image *i, uint32_t w, uint32_t h, VkFormat format,
                            VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    memset(i, 0, sizeof *i);
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = (VkExtent3D){ w, h, 1 };
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKS_CHECK(vkCreateImage(s->device, &info, NULL, &i->image));
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(s->device, i->image, &need);
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = need.size;
    if (!vks_memory_type(s, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc.memoryTypeIndex) &&
        !vks_memory_type(s, need.memoryTypeBits, 0, &alloc.memoryTypeIndex)) goto fail;
    VKS_CHECK(vkAllocateMemory(s->device, &alloc, NULL, &i->memory));
    VKS_CHECK(vkBindImageMemory(s->device, i->image, i->memory, 0));
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = i->image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
    view.subresourceRange = (VkImageSubresourceRange){ aspect, 0, 1, 0, 1 };
    VKS_CHECK(vkCreateImageView(s->device, &view, NULL, &i->view));
    return 1;
fail:
    vks_image_destroy(s, i);
    return 0;
}

static void vks_texture_clear(const vk_scene *s, vks_cached_texture *entry) {
    vks_image_destroy(s, &entry->texels);
    vks_image_destroy(s, &entry->palette);
}

static VkResult vks_texture_pool_create(const vk_scene *s, uint32_t count, VkDescriptorPool *pool) {
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * count };
    VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    info.maxSets = count; info.poolSizeCount = 1; info.pPoolSizes = &size;
    return vkCreateDescriptorPool(s->device, &info, NULL, pool);
}

static VkShaderModule vks_shader(const vk_scene *s, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size; info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(s->device, &info, NULL, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

static void vks_transform_destroy(const vk_scene *s, vks_transform *t) {
    if (t->pool) vkDestroyDescriptorPool(s->device, t->pool, NULL);
    if (t->pipeline) vkDestroyPipeline(s->device, t->pipeline, NULL);
    if (t->layout) vkDestroyPipelineLayout(s->device, t->layout, NULL);
    if (t->set_layout) vkDestroyDescriptorSetLayout(s->device, t->set_layout, NULL);
    vks_buffer_destroy(s, &t->vertices);
    memset(t, 0, sizeof *t);
}

static void vk_scene_destroy(vk_scene *s) {
    if (!s->device) return;
    vkDeviceWaitIdle(s->device);
    vks_buffer_destroy(s, &s->readback);
    vks_buffer_destroy(s, &s->layers_readback);
    if (s->fence) vkDestroyFence(s->device, s->fence, NULL);
    if (s->pool) vkDestroyCommandPool(s->device, s->pool, NULL);
    vks_transform_destroy(s, &s->transform);
    if (s->texture_pool) vkDestroyDescriptorPool(s->device, s->texture_pool, NULL);
    if (s->snapshot_pool) vkDestroyDescriptorPool(s->device, s->snapshot_pool, NULL);
    vks_image_destroy(s, &s->snapshot);
    for (uint32_t i = 0; i < VKS_TEXTURE_CACHE_SIZE; i++) vks_texture_clear(s, &s->texture_cache[i]);
    vks_buffer_destroy(s, &s->vertices);
    vks_buffer_destroy(s, &s->staging);
    free(s->texture_uses);
    free(s->runs);
    if (s->sampler) vkDestroySampler(s->device, s->sampler, NULL);
    if (s->pipeline) vkDestroyPipeline(s->device, s->pipeline, NULL);
    if (s->blend_pipeline) vkDestroyPipeline(s->device, s->blend_pipeline, NULL);
    if (s->pipeline_layout) vkDestroyPipelineLayout(s->device, s->pipeline_layout, NULL);
    if (s->set_layout) vkDestroyDescriptorSetLayout(s->device, s->set_layout, NULL);
    if (s->snapshot_layout) vkDestroyDescriptorSetLayout(s->device, s->snapshot_layout, NULL);
    if (s->framebuffer) vkDestroyFramebuffer(s->device, s->framebuffer, NULL);
    if (s->pass) vkDestroyRenderPass(s->device, s->pass, NULL);
    if (s->load_pass) vkDestroyRenderPass(s->device, s->load_pass, NULL);
    vks_image_destroy(s, &s->depth);
    vks_image_destroy(s, &s->colour);
    memset(s, 0, sizeof *s);
}

/* Render target is guest_width*scale by guest_height*scale. The caller owns the device. */
static int vk_scene_create(vk_scene *s, VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family,
                           uint32_t guest_width, uint32_t guest_height, uint32_t scale) {
    memset(s, 0, sizeof *s);
    s->physical = physical; s->device = device; s->queue = queue; s->family = family;
    s->guest_width = guest_width; s->guest_height = guest_height; s->scale = scale ? scale : 1;
    s->width = guest_width * s->scale; s->height = guest_height * s->scale;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE, blend_fragment = VK_NULL_HANDLE;
    if (!vks_image_create(s, &s->colour, s->width, s->height, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !vks_image_create(s, &s->depth, s->width, s->height, VK_FORMAT_D32_SFLOAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !vks_buffer_create(s, &s->readback, (VkDeviceSize)s->width * s->height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        goto fail;

    VkAttachmentDescription attachments[2] = {
        { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
          VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
        { 0, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
          VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } };
    VkAttachmentReference colour_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &colour_ref, NULL, &depth_ref };
    VkPipelineStageFlags attachment_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkAccessFlags attachment_writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkAccessFlags attachment_reads = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    /* Order store/copy/load, including depth when only colour was copied for a kind-5 draw. */
    VkSubpassDependency dependencies[2] = {
        { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT | attachment_stages, attachment_stages,
          VK_ACCESS_TRANSFER_READ_BIT | attachment_writes, attachment_reads | attachment_writes, 0 },
        { 0, VK_SUBPASS_EXTERNAL, attachment_stages, VK_PIPELINE_STAGE_TRANSFER_BIT | attachment_stages,
          attachment_writes, VK_ACCESS_TRANSFER_READ_BIT | attachment_reads | attachment_writes, 0 } };
    VkRenderPassCreateInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    pass.attachmentCount = 2; pass.pAttachments = attachments; pass.subpassCount = 1; pass.pSubpasses = &subpass;
    pass.dependencyCount = 2; pass.pDependencies = dependencies;
    VKS_CHECK(vkCreateRenderPass(device, &pass, NULL, &s->pass));
    /* Layouts and load ops can differ without breaking framebuffer/pipeline compatibility. */
    for (unsigned i = 0; i < 2; i++) {
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[i].initialLayout = attachments[i].finalLayout;
    }
    VKS_CHECK(vkCreateRenderPass(device, &pass, NULL, &s->load_pass));
    VkImageView views[2] = { s->colour.view, s->depth.view };
    VkFramebufferCreateInfo fb = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fb.renderPass = s->pass; fb.attachmentCount = 2; fb.pAttachments = views; fb.width = s->width; fb.height = s->height; fb.layers = 1;
    VKS_CHECK(vkCreateFramebuffer(device, &fb, NULL, &s->framebuffer));

    VkDescriptorSetLayoutBinding bindings[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set_info.bindingCount = 2; set_info.pBindings = bindings;
    VKS_CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &s->set_layout));
    set_info.bindingCount = 1;
    VKS_CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &s->snapshot_layout));
    VKS_CHECK(vks_texture_pool_create(s, VKS_TEXTURE_CACHE_SIZE, &s->texture_pool));
    VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(vks_push) };
    VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout set_layouts[] = { s->set_layout, s->snapshot_layout };
    layout.setLayoutCount = 2; layout.pSetLayouts = set_layouts;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    VKS_CHECK(vkCreatePipelineLayout(device, &layout, NULL, &s->pipeline_layout));

    vertex = vks_shader(s, scene_vertex_spirv, sizeof scene_vertex_spirv);
    fragment = vks_shader(s, scene_fragment_spirv, sizeof scene_fragment_spirv);
    blend_fragment = vks_shader(s, scene_blend_spirv, sizeof scene_blend_spirv);
    if (!vertex || !fragment || !blend_fragment) { fprintf(stderr, "vk_scene: shader module creation failed\n"); goto fail; }
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", NULL } };
    VkVertexInputBindingDescription binding = { 0, sizeof(vks_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SINT, offsetof(vks_vertex, xy_inv) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vks_vertex, abz) } };
    VkPipelineVertexInputStateCreateInfo input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 2; input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport = { 0, 0, (float)s->width, (float)s->height, 0.0f, 1.0f };
    VkRect2D scissor = { { 0, 0 }, { s->width, s->height } };
    VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport_state.viewportCount = 1; viewport_state.pViewports = &viewport; viewport_state.scissorCount = 1; viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_CLOCKWISE; raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depth.depthTestEnable = VK_TRUE; depth.depthWriteEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_GREATER; /* nearer = larger */
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1; blend.pAttachments = &blend_attachment;
    VkGraphicsPipelineCreateInfo pipeline = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeline.stageCount = 2; pipeline.pStages = stages; pipeline.pVertexInputState = &input; pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport_state; pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &blend; pipeline.layout = s->pipeline_layout;
    pipeline.renderPass = s->pass; pipeline.subpass = 0;
    VKS_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &s->pipeline));
    stages[1].module = blend_fragment;
    depth.depthWriteEnable = VK_FALSE;
    VkDynamicState scissor_state = VK_DYNAMIC_STATE_SCISSOR;
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 1; dynamic.pDynamicStates = &scissor_state;
    pipeline.pDynamicState = &dynamic;
    VKS_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &s->blend_pipeline));

    VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VKS_CHECK(vkCreateSampler(device, &sampler, NULL, &s->sampler));
    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.queueFamilyIndex = family; pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKS_CHECK(vkCreateCommandPool(device, &pool, NULL, &s->pool));
    VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.commandPool = s->pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
    VKS_CHECK(vkAllocateCommandBuffers(device, &allocate, &s->commands));
    VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VKS_CHECK(vkCreateFence(device, &fence, NULL, &s->fence));
    vkDestroyShaderModule(device, vertex, NULL);
    vkDestroyShaderModule(device, fragment, NULL);
    vkDestroyShaderModule(device, blend_fragment, NULL);
    return 1;
fail:
    if (vertex) vkDestroyShaderModule(device, vertex, NULL);
    if (fragment) vkDestroyShaderModule(device, fragment, NULL);
    if (blend_fragment) vkDestroyShaderModule(device, blend_fragment, NULL);
    vk_scene_destroy(s);
    return 0;
}

typedef struct { uint32_t rows[3][4], count, shift, post_shift; } vks_transform_push;
_Static_assert(offsetof(vks_transform_push, count) == 48 && offsetof(vks_transform_push, shift) == 52 &&
               offsetof(vks_transform_push, post_shift) == 56 && sizeof(vks_transform_push) == 60,
               "transform shader push constant layout");

static int vks_transform_prepare(vk_scene *s) {
    vks_transform *t = &s->transform;
    if (t->pipeline) return 1;

    // Graphics-only families remain valid for rendering, but not for dispatch.
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &families, NULL);
    if (s->family >= families) return 0;
    families = s->family + 1;
    VkQueueFamilyProperties *props = calloc(families, sizeof *props);
    if (!props) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &families, props);
    int compute = s->family < families && (props[s->family].queueFlags & VK_QUEUE_COMPUTE_BIT);
    free(props);
    if (!compute) return 0;

    VkPhysicalDeviceProperties physical;
    vkGetPhysicalDeviceProperties(s->physical, &physical);
    t->max_range = physical.limits.maxStorageBufferRange;
    t->max_groups = physical.limits.maxComputeWorkGroupCount[0];

    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo set = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set.bindingCount = 1; set.pBindings = &binding;
    VKS_CHECK(vkCreateDescriptorSetLayout(s->device, &set, NULL, &t->set_layout));
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vks_transform_push) };
    VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout.setLayoutCount = 1; layout.pSetLayouts = &t->set_layout;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    VKS_CHECK(vkCreatePipelineLayout(s->device, &layout, NULL, &t->layout));
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
    VKS_CHECK(vkCreateDescriptorPool(s->device, &pool, NULL, &t->pool));
    VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    alloc.descriptorPool = t->pool; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &t->set_layout;
    VKS_CHECK(vkAllocateDescriptorSets(s->device, &alloc, &t->set));
    shader = vks_shader(s, scene_transform_spirv, sizeof scene_transform_spirv);
    if (!shader) goto fail;
    VkComputePipelineCreateInfo pipeline = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = shader; pipeline.stage.pName = "main"; pipeline.layout = t->layout;
    VKS_CHECK(vkCreateComputePipelines(s->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &t->pipeline));
    vkDestroyShaderModule(s->device, shader, NULL);
    return 1;
fail:
    if (shader) vkDestroyShaderModule(s->device, shader, NULL);
    vks_transform_destroy(s, t);
    return 0;
}

static int vks_transform_dispatch(vk_scene *s, const uint32_t rows[3][4], const uint32_t (*world)[3],
                                  uint32_t (*camera)[3], uint32_t count, unsigned shift, unsigned post_shift) {
    if (!count) return 1;
    if (!s || !s->device || !rows || !world || !camera || !vks_transform_prepare(s)) return 0;
    vks_transform *t = &s->transform;
    VkDeviceSize size = (VkDeviceSize)count * sizeof *world;
    uint32_t groups = 1 + (count - 1) / 64;
    if (size > t->max_range || size > SIZE_MAX || groups > t->max_groups) return 0;

    int submitted = 0;
    VkCommandBuffer cb = s->commands;
    VKS_CHECK(vkResetCommandBuffer(cb, 0));
    if (size > t->capacity) {
        VkDeviceSize capacity = t->capacity + t->capacity / 2;
        if (capacity > t->max_range) capacity = t->max_range;
        if (capacity < size) capacity = size;
        vks_buffer buffer;
        if (!vks_buffer_create(s, &buffer, capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
        VkDescriptorBufferInfo info = { buffer.buffer, 0, capacity };
        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = t->set; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
        vkUpdateDescriptorSets(s->device, 1, &write, 0, NULL);
        vks_buffer_destroy(s, &t->vertices);
        t->vertices = buffer; t->capacity = capacity;
    }

    vks_transform_push push;
    memcpy(push.rows, rows, sizeof push.rows);
    push.count = count;
    push.shift = shift;
    push.post_shift = post_shift;
    memcpy(t->vertices.mapped, world, (size_t)size);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKS_CHECK(vkBeginCommandBuffer(cb, &begin));
    // Coherent host writes are made available to the device by queue submission.
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, t->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, t->layout, 0, 1, &t->set, 0, NULL);
    vkCmdPushConstants(cb, t->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(cb, groups, 1, 1);
    VkBufferMemoryBarrier host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = t->vertices.buffer; host.size = size;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host, 0, NULL);
    VKS_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
    VKS_CHECK(vkResetFences(s->device, 1, &s->fence));
    VKS_CHECK(vkQueueSubmit(s->queue, 1, &submit, s->fence));
    submitted = 1;
    VKS_CHECK(vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, UINT64_MAX));
    memcpy(camera, t->vertices.mapped, (size_t)size);
    return 1;
fail:
    if (submitted) vkDeviceWaitIdle(s->device);
    return 0;
}

/* Both transforms are synchronous; the caller serializes access to the scene and queue.
   World and camera are packed triples of 32-bit words, and may alias. Camera is only written on
   success. Nonempty batches require compute support; empty batches do not use Vulkan.
   Each row computes a wrapped 32-bit dot with world.xyz, arithmetic shifts by 8, then adds row[3]. */
static inline int vk_scene_transform(vk_scene *s, const uint32_t rows[3][4], const uint32_t (*world)[3],
                                     uint32_t (*camera)[3], uint32_t count) {
    return vks_transform_dispatch(s, rows, world, camera, count, 8, 0);
}

/* Guest mesh transform (E32 0x8a104): pass the signed-short XYZ sign-extended to 32 bits.
   Each row computes ASR(wrap(ASR(wrapped_dot, shift) + row[3]), shift), including the second shift
   after translation. As above, arithmetic uses packed 32-bit words. Only shifts 0..31 are valid. */
static int vk_scene_transform_mesh(vk_scene *s, const uint32_t rows[3][4], const uint32_t (*world)[3],
                                   uint32_t (*camera)[3], uint32_t count, unsigned shift) {
    if (shift > 31) return 0;
    return vks_transform_dispatch(s, rows, world, camera, count, shift, shift);
}

static uint8_t vks_expand(unsigned value, unsigned bits) { return (uint8_t)((value << (8 - bits)) | (value >> (2 * bits - 8))); }

static void vks_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                        VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = src; barrier.dstAccessMask = dst; barrier.oldLayout = from; barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.image = image;
    barrier.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* Word-at-a-time MurmurHash64A. memcpy also handles unaligned guest data. */
static uint64_t vks_hash_bytes(const void *data, size_t size, uint64_t hash) {
    const uint64_t m = UINT64_C(0xc6a4a7935bd1e995);
    const uint8_t *p = data;
    hash ^= size * m;
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, p, sizeof word);
        word *= m; word ^= word >> 47; word *= m;
        hash ^= word; hash *= m;
        p += sizeof word; size -= sizeof word;
    }
    if (size) {
        uint64_t tail = 0;
        memcpy(&tail, p, size);
        hash ^= tail; hash *= m;
    }
    hash ^= hash >> 47; hash *= m;
    return hash ^ (hash >> 47);
}

static int vks_texture_valid(const vk_scene_texture *tex, uint32_t kind) {
    uint32_t w = tex->width, h = tex->height;
    if (!w || !h || (uint64_t)w * h > INT32_MAX) return 0;
    if ((kind == 4 || kind == 5) && (w > 4096 || h > 4096 || (w & (w - 1)) || (h & (h - 1)))) return 0;
    if (kind == 5)
        return tex->direct && tex->weights && !tex->palette && !tex->texels &&
               !(((uintptr_t)tex->direct | (uintptr_t)tex->weights) & 1);
    return tex->palette && tex->texels && !tex->direct && !tex->weights;
}

static int vks_texture_reusable(const vks_cached_texture *entry, const vk_scene_texture *tex) {
    return entry->texels.image && entry->width == tex->width && entry->height == tex->height &&
           entry->direct == (tex->direct != NULL);
}

static int vks_texture_matches(const vks_cached_texture *entry, const vk_scene_texture *tex) {
    return entry->address == tex->address && vks_texture_reusable(entry, tex);
}

static vks_cached_texture *vks_texture_find(vk_scene *s, const vk_scene_texture *tex, uint64_t hash) {
    for (uint32_t i = 0; i < VKS_TEXTURE_CACHE_SIZE; i++) {
        vks_cached_texture *entry = &s->texture_cache[i];
        if (vks_texture_matches(entry, tex) && entry->hash == hash) return entry;
    }
    return NULL;
}

/* Only evict entries not pinned by this submission. Prefer updating an animated texture in place. */
static vks_cached_texture *vks_texture_replace(vk_scene *s, const vk_scene_texture *tex, uint64_t cutoff) {
    vks_cached_texture *empty = NULL, *lru = NULL;
    for (uint32_t i = 0; i < VKS_TEXTURE_CACHE_SIZE; i++) {
        vks_cached_texture *entry = &s->texture_cache[i];
        if (!entry->texels.image) { if (!empty) empty = entry; continue; }
        if (entry->last_used > cutoff) continue;
        if (vks_texture_matches(entry, tex)) return entry;
        if (!lru || entry->last_used < lru->last_used) lru = entry;
    }
    return empty ? empty : lru;
}

static int vks_texture_prepare(const vk_scene *s, vks_cached_texture *entry, const vk_scene_texture *tex,
                               VkDescriptorPool pool) {
    if (vks_texture_reusable(entry, tex)) return 1;
    vks_texture_clear(s, entry);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormat format = tex->direct ? VK_FORMAT_R16G16_UINT : VK_FORMAT_R8_UINT;
    if (!vks_image_create(s, &entry->texels, tex->width, tex->height, format, usage, VK_IMAGE_ASPECT_COLOR_BIT) ||
        (!tex->direct && !vks_image_create(s, &entry->palette, 256, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                           usage, VK_IMAGE_ASPECT_COLOR_BIT))) goto fail;
    if (!entry->set) {
        VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = pool; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &s->set_layout;
        VKS_CHECK(vkAllocateDescriptorSets(s->device, &alloc, &entry->set));
    }
    /* The blend shader does not use binding 1. Initialize it anyway for drivers that fetch the whole set. */
    VkDescriptorImageInfo images[2] = {
        { s->sampler, entry->texels.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { s->sampler, tex->direct ? entry->texels.view : entry->palette.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    VkWriteDescriptorSet writes[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, entry->set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[0], NULL, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, entry->set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[1], NULL, NULL } };
    vkUpdateDescriptorSets(s->device, 2, writes, 0, NULL);
    return 1;
fail:
    return 0; /* The render's cleanup owns partially initialized entries, too. */
}

static VkDeviceSize vks_palette_offset(const vk_scene_texture *tex) {
    return ((VkDeviceSize)tex->width * tex->height + 3) & ~(VkDeviceSize)3;
}

static VkDeviceSize vks_texture_upload_size(const vk_scene_texture *tex) {
    return tex->direct ? (VkDeviceSize)tex->width * tex->height * 4 : vks_palette_offset(tex) + 256 * 4;
}

static void vks_upload_image(VkCommandBuffer cb, VkBuffer staging, VkImage image, VkDeviceSize offset,
                             uint32_t width, uint32_t height, int reuse) {
    vks_barrier(cb, image, reuse ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, reuse ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, reuse ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = { offset, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { width, height, 1 } };
    vkCmdCopyBufferToImage(cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vks_barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

static int vks_texture_upload(const vk_scene *s, const vk_scene_texture *tex, const vks_cached_texture *entry,
                              VkDeviceSize offset, int reuse) {
    size_t pixels = (size_t)tex->width * tex->height;
    uint8_t *base = (uint8_t *)s->staging.mapped + offset;
    if (tex->direct) {
        uint16_t *pairs = (uint16_t *)base;
        for (size_t i = 0; i < pixels; i++) {
            if (tex->weights[i] > 256) { fprintf(stderr, "vk_scene: invalid kind-5 weight\n"); return 0; }
            pairs[2 * i] = tex->direct[i];
            pairs[2 * i + 1] = tex->weights[i];
        }
    } else {
        memcpy(base, tex->texels, pixels);
        VkDeviceSize palette_offset = vks_palette_offset(tex);
        uint8_t *palette = base + palette_offset;
        for (unsigned c = 0; c < 256; c++) {
            unsigned p = tex->palette[c];
            palette[c * 4 + 0] = vks_expand(p >> 11, 5);
            palette[c * 4 + 1] = vks_expand((p >> 5) & 63, 6);
            palette[c * 4 + 2] = vks_expand(p & 31, 5);
            palette[c * 4 + 3] = p == 0xF81F ? 0 : 255; /* colour key */
        }
        vks_upload_image(s->commands, s->staging.buffer, entry->palette.image, offset + palette_offset, 256, 1, reuse);
    }
    vks_upload_image(s->commands, s->staging.buffer, entry->texels.image, offset, tex->width, tex->height, reuse);
    return 1;
}

static void vks_copy_target(const vk_scene *s, VkImage image, VkImageAspectFlags aspect,
                            VkBuffer buffer, VkDeviceSize offset) {
    VkBufferImageCopy copy = { offset, 0, 0, { aspect, 0, 0, 1 }, { 0, 0, 0 }, { s->width, s->height, 1 } };
    vkCmdCopyImageToBuffer(s->commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
}

static int vks_snapshot_prepare(vk_scene *s) {
    if (s->snapshot_set) return 1;
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
    VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    alloc.descriptorSetCount = 1; alloc.pSetLayouts = &s->snapshot_layout;
    if (!vks_image_create(s, &s->snapshot, s->width, s->height, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) goto fail;
    VKS_CHECK(vkCreateDescriptorPool(s->device, &pool, NULL, &s->snapshot_pool));
    alloc.descriptorPool = s->snapshot_pool;
    VKS_CHECK(vkAllocateDescriptorSets(s->device, &alloc, &s->snapshot_set));
    VkDescriptorImageInfo image = { s->sampler, s->snapshot.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s->snapshot_set, 0, 0, 1,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image, NULL, NULL };
    vkUpdateDescriptorSets(s->device, 1, &write, 0, NULL);
    return 1;
fail:
    if (s->snapshot_pool) vkDestroyDescriptorPool(s->device, s->snapshot_pool, NULL);
    s->snapshot_pool = VK_NULL_HANDLE;
    s->snapshot_set = VK_NULL_HANDLE;
    vks_image_destroy(s, &s->snapshot);
    return 0;
}

static int64_t vks_bound(int64_t value, uint32_t extent) {
    return value < 0 ? 0 : value > extent ? extent : value;
}

static VkRect2D vks_snapshot_bounds(const vk_scene *s, const vk_scene_tri *t) {
    VkRect2D rect = { { 0, 0 }, { s->width, s->height } };
    /* Use the retained post-projection v.xy regardless of the shader's input. Refuse to tighten
       bounds for extreme coordinates. Otherwise cover the shader's half-row shift plus two target
       pixels for viewport/clipping roundoff and subpixel snapping. */
    if (s->width > 65536 || s->height > 65536) return rect;
    int32_t lo[2] = { t->v[0][0], t->v[0][1] }, hi[2] = { lo[0], lo[1] };
    for (unsigned v = 1; v < 3; v++) for (unsigned axis = 0; axis < 2; axis++) {
        int32_t coord = t->v[v][axis];
        if (coord < lo[axis]) lo[axis] = coord;
        if (coord > hi[axis]) hi[axis] = coord;
    }
    if (lo[0] < -2 * (int64_t)s->guest_width || hi[0] > 2 * (int64_t)s->guest_width ||
        lo[1] < -2 * (int64_t)s->guest_height || hi[1] > 2 * (int64_t)s->guest_height) return rect;
    int64_t x0 = vks_bound((int64_t)lo[0] * s->scale - 2, s->width);
    int64_t y0 = vks_bound((int64_t)lo[1] * s->scale + s->scale / 2 - 2, s->height);
    int64_t x1 = vks_bound((int64_t)hi[0] * s->scale + 2, s->width);
    int64_t y1 = vks_bound((int64_t)hi[1] * s->scale + ((int64_t)s->scale + 1) / 2 + 2, s->height);
    return (VkRect2D){ { (int32_t)x0, (int32_t)y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
}

static void vks_snapshot_copy(const vk_scene *s, VkRect2D bounds, int reuse) {
    VkCommandBuffer cb = s->commands;
    vks_barrier(cb, s->snapshot.image, reuse ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, reuse ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT : 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, reuse ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT :
                                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    VkOffset3D offset = { bounds.offset.x, bounds.offset.y, 0 };
    VkImageCopy copy = { layers, offset, layers, offset, { bounds.extent.width, bounds.extent.height, 1 } };
    vkCmdCopyImage(cb, s->colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s->snapshot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vks_barrier(cb, s->snapshot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

/* Returns 1 on success. All output planes are tightly packed, width*height, and valid until the next
   render or destroy. Primary RGBA8 is writable; a nonzero composite also requests kind-6 RGBA8 and
   both D32 planes. Clear depth is 0; positive depth means covered even when kind-6 alpha is 0. */
static int vk_scene_render(vk_scene *s, const vk_scene_tri *tris, uint32_t count,
                           const vk_scene_texture *textures, uint32_t texture_count,
                           int composite, vk_scene_layers *out) {
    vks_cached_texture *transient = NULL;
    VkDescriptorPool transient_pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = s->commands;
    vks_texture_use *uses = NULL;
    uint32_t transient_count = 0, uploads = 0, used_count = 0;
    int result = 0, submitted = 0;
    VkDeviceSize plane_size = (VkDeviceSize)s->width * s->height * 4;
    uint32_t phases = composite ? 2 : 1;
    *out = (vk_scene_layers){ 0 };
    s->texture_uploads = 0;
    if (texture_count > s->texture_use_capacity) {
        vks_texture_use *grown = realloc(s->texture_uses, (size_t)texture_count * sizeof *grown);
        if (!grown) goto fail;
        s->texture_uses = grown; s->texture_use_capacity = texture_count;
    }
    if (count > s->run_capacity) {
        vks_run *grown = realloc(s->runs, (size_t)count * sizeof *grown);
        if (!grown) goto fail;
        s->runs = grown; s->run_capacity = count;
    }
    uses = s->texture_uses;
    if (texture_count) memset(uses, 0, (size_t)texture_count * sizeof *uses);

    /* The previous submission completed before returning. Reset before replacing referenced objects. */
    VKS_CHECK(vkResetCommandBuffer(cb, 0));
    if (!vks_buffer_reserve(s, &s->vertices, &s->vertex_capacity,
                            (VkDeviceSize)count * 3 * sizeof(vks_vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) goto fail;
    if (composite && !vks_buffer_reserve(s, &s->layers_readback, &s->layers_capacity,
                                        3 * plane_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) goto fail;

    /* Pack layers in submission order: signed (x, y, inv) and float (a, b, z), see scene.vert.
       z is the unbiased reciprocal depth (1/idx); the shader biases/scales D32 separately. */
    vks_vertex *data = s->vertices.mapped;
    uint32_t vertex_count = 0, runs = 0, run_bounds[3] = { 0 };
    int needs_snapshot = 0;
    for (uint32_t phase = 0; phase < phases; phase++) {
        for (uint32_t i = 0; i < count; i++) {
            const vk_scene_tri *t = &tris[i];
            if (phase ? t->kind != 6 : (t->kind > 1 && t->kind != 4 && t->kind != 5)) continue;
            if (t->texture >= texture_count) {
                if (t->kind == 4 || t->kind == 5) {
                    fprintf(stderr, "vk_scene: missing kind-%u texture\n", t->kind);
                    goto fail;
                }
                continue;
            }
            uint32_t x0 = (uint32_t)t->v[0][0] - (uint32_t)t->v[2][0];
            uint32_t y0 = (uint32_t)t->v[0][1] - (uint32_t)t->v[2][1];
            uint32_t x1 = (uint32_t)t->v[1][0] - (uint32_t)t->v[2][0];
            uint32_t y1 = (uint32_t)t->v[1][1] - (uint32_t)t->v[2][1];
            uint32_t area = x0 * y1 - x1 * y0;
            if (!area || area > INT32_MAX) continue; /* guest signed 32-bit backface/degenerate test */
            if (!vks_texture_valid(&textures[t->texture], t->kind)) {
                fprintf(stderr, "vk_scene: invalid kind-%u texture layout\n", t->kind);
                goto fail;
            }
            if (t->kind == 5) needs_snapshot = 1;
            if (!uses[t->texture].used) { uses[t->texture].used = 1; used_count++; }
            uint32_t affine = t->kind == 1;
            uint8_t alpha = phase ? t->alpha : 255;
            vks_vertex *vertex = data + vertex_count;
            for (int v = 0; v < 3; v++) {
                const int32_t *w = t->v[v];
                if ((t->kind == 4 || t->kind == 5) && w[2] <= 0) {
                    fprintf(stderr, "vk_scene: invalid kind-%u depth index\n", t->kind);
                    goto fail;
                }
                float z = w[2] > 0 ? 1.0f / (float)w[2] : 1.0f / 268435456.0f; /* RECIP[0] = 1 -> 2^-28 */
                if (t->projection.valid) memcpy(vertex[v].xy_inv, t->projection.xy_inv[v], sizeof vertex[v].xy_inv);
                else {
                    vertex[v].xy_inv[0] = w[0]; vertex[v].xy_inv[1] = w[1]; vertex[v].xy_inv[2] = 0;
                }
                vertex[v].abz[0] = affine ? (float)w[3] : (float)w[3] * z;
                vertex[v].abz[1] = affine ? (float)w[4] : (float)w[4] * z;
                vertex[v].abz[2] = z;
            }
            /* Each draw uses the projection push constants from the run's first triangle. */
            if (t->kind != 5 && runs > run_bounds[phase] && s->runs[runs - 1].texture == t->texture &&
                s->runs[runs - 1].kind == t->kind && s->runs[runs - 1].alpha == alpha &&
                vks_projection_matches(&tris[s->runs[runs - 1].triangle].projection, &t->projection))
                s->runs[runs - 1].count += 3;
            else s->runs[runs++] = (vks_run){ vertex_count, t->texture, t->kind, 3, i, alpha };
            vertex_count += 3;
        }
        run_bounds[phase + 1] = runs;
    }
    if (needs_snapshot && !vks_snapshot_prepare(s)) goto fail;
    /* Pin all hits before filling misses so that an early miss cannot evict a later hit. */
    uint64_t cutoff = s->texture_clock;
    for (uint32_t i = 0; i < texture_count; i++) {
        if (!uses[i].used) continue;
        const vk_scene_texture *tex = &textures[i];
        size_t pixels = (size_t)tex->width * tex->height;
        if (tex->direct) {
            uint64_t hash = vks_hash_bytes(tex->direct, pixels * sizeof(uint16_t), 0);
            uses[i].hash = vks_hash_bytes(tex->weights, pixels * sizeof(uint16_t), hash);
        } else {
            uint64_t hash = vks_hash_bytes(tex->palette, 512, 0);
            uses[i].hash = vks_hash_bytes(tex->texels, pixels, hash);
        }
        uses[i].entry = vks_texture_find(s, tex, uses[i].hash);
        if (uses[i].entry) uses[i].entry->last_used = ++s->texture_clock;
    }

    VkDeviceSize staging_size = 0;
    for (uint32_t i = 0; i < texture_count; i++) {
        vks_texture_use *use = &uses[i];
        if (!use->used || use->entry) continue;
        const vk_scene_texture *tex = &textures[i];
        vks_cached_texture *entry = vks_texture_find(s, tex, use->hash); /* another index may share the key */
        if (!entry) {
            VkDescriptorPool pool = s->texture_pool;
            entry = vks_texture_replace(s, tex, cutoff);
            if (!entry) {
                /* This submission exceeds the cache. Keep drawing in order with uncached textures. */
                if (!transient) {
                    uint32_t overflow = used_count - VKS_TEXTURE_CACHE_SIZE;
                    transient = calloc(overflow, sizeof *transient);
                    if (!transient) goto fail;
                    VKS_CHECK(vks_texture_pool_create(s, overflow, &transient_pool));
                }
                entry = &transient[transient_count++];
                pool = transient_pool;
            }
            use->entry = entry;
            use->upload = 1;
            use->reuse = vks_texture_reusable(entry, tex);
            if (!vks_texture_prepare(s, entry, tex, pool)) goto fail;
            entry->address = tex->address; entry->width = tex->width; entry->height = tex->height;
            entry->direct = tex->direct != NULL;
            entry->hash = use->hash;
            staging_size += vks_texture_upload_size(tex);
            uploads++;
        }
        use->entry = entry;
        entry->last_used = ++s->texture_clock;
    }
    if (!vks_buffer_reserve(s, &s->staging, &s->staging_capacity, staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) goto fail;

    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKS_CHECK(vkBeginCommandBuffer(cb, &begin));
    VkDeviceSize offset = 0;
    for (uint32_t i = 0; i < texture_count; i++) {
        if (!uses[i].upload) continue;
        if (!vks_texture_upload(s, &textures[i], uses[i].entry, offset, uses[i].reuse)) goto fail;
        offset += vks_texture_upload_size(&textures[i]);
    }

    VkClearValue clears[2];
    clears[0].color = (VkClearColorValue){ { 0, 0, 0, 0 } };
    clears[1].depthStencil = (VkClearDepthStencilValue){ 0.0f, 0 }; /* guest clears depth to 0; larger = nearer */
    VkRenderPassBeginInfo pass_begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    pass_begin.framebuffer = s->framebuffer;
    pass_begin.renderArea = (VkRect2D){ { 0, 0 }, { s->width, s->height } };
    pass_begin.clearValueCount = 2; pass_begin.pClearValues = clears;
    int snapshot_written = 0;
    for (uint32_t phase = 0; phase < phases; phase++) {
        pass_begin.renderPass = s->pass;
        vkCmdBeginRenderPass(cb, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        if (run_bounds[phase] != run_bounds[phase + 1]) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &s->vertices.buffer, &zero);
        }
        int blend_bound = 0;
        for (uint32_t r = run_bounds[phase]; r < run_bounds[phase + 1]; r++) {
            const vks_run *run = &s->runs[r];
            if (run->kind == 5) {
                VkRect2D bounds = vks_snapshot_bounds(s, &tris[run->triangle]);
                if (bounds.extent.width == 0 || bounds.extent.height == 0) continue;
                vkCmdEndRenderPass(cb);
                vks_snapshot_copy(s, bounds, snapshot_written);
                snapshot_written = 1;
                pass_begin.renderPass = s->load_pass;
                vkCmdBeginRenderPass(cb, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
                if (!blend_bound) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->blend_pipeline);
                vkCmdSetScissor(cb, 0, 1, &bounds); /* No fragment can read outside the copied rectangle. */
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline_layout, 1, 1,
                                        &s->snapshot_set, 0, NULL);
                blend_bound = 1;
            } else if (blend_bound) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
                blend_bound = 0;
            }
            const vk_scene_texture *tex = &textures[run->texture];
            const vk_scene_projection *projection = &tris[run->triangle].projection;
            vks_push push = { .view = { (float)s->guest_width, (float)s->guest_height },
                              .tex = { (int32_t)tex->width, (int32_t)tex->height }, .kind = (int32_t)run->kind,
                              .shift = 0.5f * (float)s->scale, .alpha = (float)run->alpha * (1.0f / 255.0f),
                              .projected = projection->valid != 0 };
            if (push.projected) {
                push.center[0] = projection->center[0]; push.center[1] = projection->center[1];
            }
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline_layout, 0, 1,
                                    &uses[run->texture].entry->set, 0, NULL);
            vkCmdPushConstants(cb, s->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
            vkCmdDraw(cb, run->count, 1, run->first, 0);
        }
        vkCmdEndRenderPass(cb);
        if (phase) {
            vks_copy_target(s, s->colour.image, VK_IMAGE_ASPECT_COLOR_BIT, s->layers_readback.buffer, plane_size);
            vks_copy_target(s, s->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, s->layers_readback.buffer, 2 * plane_size);
        } else {
            vks_copy_target(s, s->colour.image, VK_IMAGE_ASPECT_COLOR_BIT, s->readback.buffer, 0);
            if (composite) vks_copy_target(s, s->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, s->layers_readback.buffer, 0);
        }
    }
    VkMemoryBarrier host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    VKS_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
    VKS_CHECK(vkResetFences(s->device, 1, &s->fence));
    VKS_CHECK(vkQueueSubmit(s->queue, 1, &submit, s->fence));
    submitted = 1;
    VKS_CHECK(vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, UINT64_MAX));
    out->rgba = s->readback.mapped;
    if (composite) {
        const uint8_t *layers = s->layers_readback.mapped;
        out->depth = (const float *)layers;
        out->kind6_rgba = layers + plane_size;
        out->kind6_depth = (const float *)(layers + 2 * plane_size);
    }
    result = 1;
    s->texture_uploads = uploads;
fail:
    if (!result && submitted) vkDeviceWaitIdle(s->device);
    if (transient_pool) vkDestroyDescriptorPool(s->device, transient_pool, NULL);
    if (result) {
        for (uint32_t i = 0; i < transient_count; i++) vks_texture_clear(s, &transient[i]);
    } else if (uses) {
        /* Discard prepared images whose upload might not have run; retain their cache sets for reuse. */
        for (uint32_t i = 0; i < texture_count; i++) if (uses[i].upload) vks_texture_clear(s, uses[i].entry);
    }
    free(transient);
    return result;
}

#endif
