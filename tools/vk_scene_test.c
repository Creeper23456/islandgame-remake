/* Headless check of tools/vk_scene.h against a --trace capture (tools/pc_trace.h).
   Build: clang -std=c11 -O1 tools/vk_scene_test.c $(pkg-config --cflags --libs vulkan) -lm -o vk_scene_test
   Run:   vk_scene_test <tracedir> [last_batch_size=344] [scale=1]
   Compares kinds 0 and 1 from the last batch of tris.bin with color.bin at scale 1, then checks
   persistent texture cache, ordered material draws, independent kind-6 layer, and mixed projection. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk_scene_spirv.h"
#include "vk_scene.h"

static uint8_t *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n ? (size_t)n : 1);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static int check_pixel(vk_scene *scene, const vk_scene_tri *tris, const vk_scene_texture *textures,
                       uint32_t count, uint32_t uploads, const uint8_t expected[4]) {
    vk_scene_layers layers;
    return vk_scene_render(scene, tris, count, textures, count, 0, &layers) &&
           scene->texture_uploads == uploads && !memcmp(layers.rgba + (1 * 4 + 1) * 4, expected, 4);
}

static int check_cache(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family) {
    vk_scene scene;
    if (!vk_scene_create(&scene, physical, device, queue, family, 4, 4, 1)) return 0;
    uint16_t palette[256] = { 0xF81F, 0xF800 };
    uint8_t texel = 1;
    const uint16_t direct = 0x07E0, weight = 256;
    const uint8_t red[4] = { 255, 0, 0, 255 }, green[4] = { 0, 255, 0, 255 }, clear[4] = { 0 };
    const vk_scene_tri tri = { .v = { {0, 0, 2}, {4, 0, 2}, {0, 4, 2} }, .kind = 1 };
    vk_scene_texture textures[VKS_TEXTURE_CACHE_SIZE + 1];
    vk_scene_tri tris[VKS_TEXTURE_CACHE_SIZE + 1];
    for (uint32_t i = 0; i <= VKS_TEXTURE_CACHE_SIZE; i++) {
        textures[i] = (vk_scene_texture){ .address = i + 1, .width = 1, .height = 1, .palette = palette, .texels = &texel };
        tris[i] = tri; tris[i].texture = i;
    }

    int ok = check_pixel(&scene, &tri, textures, 1, 1, red);
    palette[1] = 0x07E0;
    ok = ok && check_pixel(&scene, &tri, textures, 1, 1, green);
    texel = 0;
    ok = ok && check_pixel(&scene, &tri, textures, 1, 1, clear);

    /* Only the last texture draws colour. A direct texture must work in the transient pool. */
    const vk_scene_tri blend_tri = { .v = { {0, 0, 2}, {4, 0, 2}, {0, 4, 2} }, .kind = 5 };
    textures[VKS_TEXTURE_CACHE_SIZE] = (vk_scene_texture){ .address = VKS_TEXTURE_CACHE_SIZE + 1,
                                                        .width = 1, .height = 1, .direct = &direct, .weights = &weight };
    tris[VKS_TEXTURE_CACHE_SIZE].kind = 5;
    ok = ok && check_pixel(&scene, tris, textures, VKS_TEXTURE_CACHE_SIZE + 1, VKS_TEXTURE_CACHE_SIZE, green);
    ok = ok && check_pixel(&scene, tris, textures, VKS_TEXTURE_CACHE_SIZE + 1, 1, green);

    /* Refresh the oldest entry, then insert a new texture: the next-oldest one should miss. */
    ok = ok && check_pixel(&scene, &tri, textures, 1, 0, clear);
    ok = ok && check_pixel(&scene, &blend_tri, textures + VKS_TEXTURE_CACHE_SIZE, 1, 1, green);
    ok = ok && check_pixel(&scene, &tri, textures, 1, 0, clear);
    ok = ok && check_pixel(&scene, &tri, textures + 1, 1, 1, clear);
    vk_scene_destroy(&scene);
    return ok;
}

static vk_scene_tri layer_tri(uint32_t kind, int32_t x, int32_t idx, int32_t u, uint8_t alpha) {
    return (vk_scene_tri){ .v = { {x, 0, idx, u}, {x + 4, 0, idx, u}, {x, 4, idx, u} },
                           .kind = kind, .alpha = alpha };
}

static int check_layers(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family) {
    vk_scene scene;
    if (!vk_scene_create(&scene, physical, device, queue, family, 8, 4, 1)) return 0;
    const uint16_t palette[256] = { 0xF81F, 0xF800, 0x07E0, 0x001F };
    const uint8_t texels[4] = { 0, 1, 2, 3 };
    const vk_scene_texture texture = { .address = 1, .width = 4, .height = 1, .palette = palette, .texels = texels };
    vk_scene_tri tris[] = {
        layer_tri(6, 0, 8, 3, 37),
        layer_tri(0, 0, 2, 1, 37),
        layer_tri(6, 0, 4, 2, 173), /* nearer in layer 6, but behind primary; same texture, different alpha */
        layer_tri(0, 0, 2, 3, 0),   /* equal depth must not replace red in primary */
        layer_tri(6, 0, 4, 3, 222), /* equal depth must not replace green in layer 6 */
        layer_tri(6, 0, 1, 0, 255), /* nearer but keyed: changes neither colour nor depth */
        layer_tri(4, 0, 2, 3, 255), /* biased depth wins against kind 0 at the same index */
        layer_tri(4, 4, 1, 0, 255), /* even nearer, but keyed: must not write depth */
        layer_tri(1, 4, 2, 1, 0),
        layer_tri(4, 4, 1, 0, 255), /* keyed after visible fragments, too */
        layer_tri(6, 4, 1, 0, 0),   /* keyed before visible fragments */
        layer_tri(6, 4, 8, 2, 37),
        layer_tri(6, 4, 4, 3, 0),   /* zero alpha still overwrites colour and depth */
        layer_tri(6, 4, 8, 2, 255), /* farther: cannot overwrite the zero-alpha winner */
        layer_tri(6, 4, 1, 1, 255), /* invalid texture index */
    };
    const uint32_t count = sizeof tris / sizeof tris[0];
    tris[count - 1].texture = 1;
    vk_scene_layers layers;
    int ok = vk_scene_render(&scene, tris, count, &texture, 1, 1, &layers) &&
             layers.rgba && layers.depth && layers.kind6_rgba && layers.kind6_depth;
    const unsigned sample[] = { 1 * 8 + 1, 1 * 8 + 5, 3 * 8 + 7 }; /* left, right, empty */
    const uint8_t primary[][4] = { {0, 0, 255, 255}, {255, 0, 0, 255}, {0} };
    const float primary_depth[] = { (0.5f + 150000.0f / 268435456.0f) * 0.5f, 0.25f, 0.0f };
    const uint8_t kind6[][4] = { {0, 255, 0, 173}, {0, 0, 255, 0}, {0} };
    for (unsigned i = 0; ok && i < sizeof sample / sizeof sample[0]; i++) {
        unsigned p = sample[i];
        ok = !memcmp(layers.rgba + 4 * p, primary[i], 4) && layers.depth[p] == primary_depth[i] &&
             !memcmp(layers.kind6_rgba + 4 * p, kind6[i], 4) && layers.kind6_depth[p] == (i < 2 ? 0.125f : 0.0f);
    }
    if (ok) {
        uint8_t rgba[8 * 4 * 4];
        memcpy(rgba, layers.rgba, sizeof rgba);
        ok = vk_scene_render(&scene, tris, count, &texture, 1, 0, &layers) &&
             !layers.depth && !layers.kind6_rgba && !layers.kind6_depth && !memcmp(layers.rgba, rgba, sizeof rgba);
    }
    vk_scene_destroy(&scene);
    return ok;
}

static int check_blends(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family) {
    vk_scene scene;
    const unsigned scale = 2;
    if (!vk_scene_create(&scene, physical, device, queue, family, 20, 4, scale)) return 0;
    const uint16_t palette[256] = { 0xF800, 0x07E0 };
    const uint8_t texels[] = { 0, 1, 0, 0 };
    uint16_t direct[] = { 0x001F, 0xF81F, 0x07E0, 0x001F };
    uint16_t weights[] = { 0, 256, 128, 128 };
    /* Same address and dimensions, but indexed and direct layouts are distinct cache identities. */
    const vk_scene_texture textures[] = {
        { .address = 1, .width = 4, .height = 1, .palette = palette, .texels = texels },
        { .address = 1, .width = 4, .height = 1, .direct = direct, .weights = weights },
    };
    vk_scene_tri tris[] = {
        layer_tri(0, 0, 8, 0, 255),
        layer_tri(5, 0, 4, 0, 255),   /* weight 0 retains red, including alpha */
        layer_tri(5, 0, 16, 1, 255),  /* behind the opaque depth: weight 256 cannot replace red */
        layer_tri(0, 4, 8, 0, 255),
        layer_tri(5, 4, 8, 1, 255),   /* biased depth passes same idx; 0xF81F is not keyed */
        layer_tri(0, 8, 8, 0, 255),
        layer_tri(5, 8, 2, 2, 255),   /* half green over red: (15,31,0) in RGB565 */
        layer_tri(5, 8, 4, 3, 255),   /* half blue over that: (7,15,15); still passes when farther */
        layer_tri(0, 12, 8, 0, 255),
        layer_tri(5, 12, 2, 2, 255),
        layer_tri(0, 12, 4, 1, 255),  /* opaque write between blends; also catches accidental depth writes */
        layer_tri(5, 12, 4, 3, 255),  /* half blue over green: (0,31,15) */
        layer_tri(5, 16, 4, 0, 255),  /* weight 0 leaves a previously clear pixel clear */
    };
    const uint32_t count = sizeof tris / sizeof tris[0];
    for (unsigned i = 0; i < count; i++) tris[i].texture = tris[i].kind == 5;
    vk_scene_layers layers;
    uint16_t expected[] = { 0xF800, 0xF81F, (7 << 11) | (15 << 5) | 15, (31 << 5) | 15, 0 };
    const uint8_t alpha[] = { 255, 255, 255, 255, 0 };
    const float depth[] = { 0.0625f, 0.0625f, 0.0625f, 0.125f, 0.0f };
    const uint32_t uploads[] = { 2, 0, 1, 1 };
    int ok = 1;
    for (unsigned frame = 0; ok && frame < sizeof uploads / sizeof uploads[0]; frame++) {
        if (frame == 2) {
            weights[2] = 256; /* changing either direct plane must invalidate exactly one upload */
            expected[2] = expected[3];
        } else if (frame == 3) {
            direct[3] = 0xF800;
            expected[2] = expected[3] = (15 << 11) | (31 << 5);
        }
        ok = vk_scene_render(&scene, tris, count, textures, 2, 1, &layers) && scene.texture_uploads == uploads[frame];
        for (unsigned i = 0; ok && i < sizeof expected / sizeof expected[0]; i++) {
            unsigned p = scale * scene.width + (4 * i + 1) * scale;
            const uint8_t *rgba = layers.rgba + 4 * p;
            unsigned rgb565 = ((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) | (rgba[2] >> 3);
            ok = rgb565 == expected[i] && rgba[3] == alpha[i] && layers.depth[p] == depth[i] &&
                 !memcmp(layers.kind6_rgba + 4 * p, (const uint8_t[4]){0}, 4) && layers.kind6_depth[p] == 0.0f;
        }
    }
    vk_scene_destroy(&scene);
    return ok;
}

static int check_projection(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family) {
    vk_scene scene;
    enum { width = 20, height = 10, scale = 2, plane_size = width * height * scale * scale * 4 };
    if (!vk_scene_create(&scene, physical, device, queue, family, width, height, scale)) return 0;
    const uint16_t palette[256] = { 0xF81F, 0xF800 };
    const uint8_t texel = 1;
    const uint16_t direct = 0x001F, weight = 128;
    const vk_scene_texture textures[] = {
        { .address = 1, .width = 1, .height = 1, .palette = palette, .texels = &texel },
        { .address = 2, .width = 1, .height = 1, .direct = &direct, .weights = &weight },
    };
    /* Both raw triples produce relative XY (-4,-2), (0,-2), (-4,2). The first
       exercises signed factors, fractional shifts, and wrap in the 32-bit products. */
    const int32_t wrapped_raw[3][3] = { {65532, -2, 65536}, {-1, 2, -65535}, {-32770, 32769, 131072} };
    const int32_t blend_raw[3][3] = { {-4, -2, 65536}, {0, -2, 65536}, {-4, 2, 65536} };
    const int32_t centers[][2] = { {12, 8}, {4, 2}, {12, 2}, {12, 8} }; /* index 0 disables projection */
    const struct { uint32_t kind; int32_t x, y; unsigned project; } cases[] = {
        {0, 0, 0, 1}, {6, 0, 0, 1},
        {0, 8, 0, 2}, {6, 8, 0, 2},   /* primary center changes only in X */
        {0, 8, 6, 3},                 /* primary center changes only in Y */
        {0, 16, 0, 0}, {6, 16, 0, 0}, /* center unchanged from last primary draw, but projection off */
        {5, 8, 0, 2},                 /* raw XY lie outside the required snapshot rectangle */
    };
    enum { count = sizeof cases / sizeof cases[0] };
    vk_scene_tri legacy[count], mixed[count];
    for (unsigned i = 0; i < count; i++) {
        legacy[i] = layer_tri(cases[i].kind, cases[i].x, cases[i].kind == 5 ? 4 : 8, 0, 173);
        legacy[i].texture = cases[i].kind == 5;
        for (unsigned v = 0; v < 3; v++) legacy[i].v[v][1] += cases[i].y;
        mixed[i] = legacy[i];
        vk_scene_projection *p = &mixed[i].projection;
        p->valid = cases[i].project != 0;
        memcpy(p->center, centers[cases[i].project], sizeof p->center);
        memcpy(p->xy_inv, cases[i].kind == 5 ? blend_raw : wrapped_raw, sizeof p->xy_inv);
    }

    uint8_t reference[4][plane_size];
    vk_scene_layers layers;
    int ok = vk_scene_render(&scene, legacy, count, textures, 2, 1, &layers);
    if (ok) {
        const unsigned blend = scale * (scene.width + 9), fallback = scale * (scene.width + 17);
        const uint8_t *blended = layers.rgba + 4 * blend;
        /* Half blue over red gives (15, 0, 15) in RGB565 without writing depth. */
        ok = (blended[0] >> 3) == 15 && blended[1] == 0 && (blended[2] >> 3) == 15 &&
             blended[3] == 255 && layers.depth[blend] == 0.0625f &&
             !memcmp(layers.rgba + 4 * fallback, (const uint8_t[]){255, 0, 0, 255}, 4) &&
             !memcmp(layers.kind6_rgba + 4 * fallback, (const uint8_t[]){255, 0, 0, 173}, 4);
        memcpy(reference[0], layers.rgba, plane_size);
        memcpy(reference[1], layers.depth, plane_size);
        memcpy(reference[2], layers.kind6_rgba, plane_size);
        memcpy(reference[3], layers.kind6_depth, plane_size);
        ok = ok && vk_scene_render(&scene, mixed, count, textures, 2, 1, &layers) &&
             !memcmp(reference[0], layers.rgba, plane_size) && !memcmp(reference[1], layers.depth, plane_size) &&
             !memcmp(reference[2], layers.kind6_rgba, plane_size) && !memcmp(reference[3], layers.kind6_depth, plane_size);
    }
    vk_scene_destroy(&scene);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <tracedir> [batch] [scale]\n", argv[0]); return 2; }
    const char *dir = argv[1];
    unsigned batch = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 344;
    unsigned scale = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 1;
    char path[512];
    size_t size;
    snprintf(path, sizeof path, "%s/tris.bin", dir);
    uint32_t *records = (uint32_t *)slurp(path, &size);
    snprintf(path, sizeof path, "%s/color.bin", dir);
    uint8_t *colour_file = slurp(path, &size);
    if (!records || !colour_file) return 1;
    size_t total = 0;
    { snprintf(path, sizeof path, "%s/tris.bin", dir); FILE *f = fopen(path, "rb"); fseek(f, 0, SEEK_END); total = (size_t)ftell(f) / 128; fclose(f); }
    if (batch > total) batch = (unsigned)total;
    const uint32_t *guest_dims = (const uint32_t *)colour_file;
    uint32_t gw = guest_dims[0], gh = guest_dims[1];
    const uint16_t *guest = (const uint16_t *)(colour_file + 8);

    /* Unique textures in first-use order. */
    vk_scene_texture textures[512]; uint8_t *texture_files[512]; uint32_t texture_count = 0;
    vk_scene_tri *tris = calloc(batch, sizeof *tris);
    uint32_t tri_count = 0, kind_counts[8] = {0};
    for (unsigned i = 0; i < batch; i++) {
        const uint32_t *r = records + (total - batch + i) * 32;
        if (r[0] != 0 && r[0] != 1) continue;
        if (!r[25]) { fprintf(stderr, "record %u has no resolved texture\n", i); continue; }
        uint32_t t = 0;
        while (t < texture_count && textures[t].address != r[25]) t++;
        if (t == texture_count) {
            if (texture_count == 512) { fprintf(stderr, "too many textures\n"); return 1; }
            snprintf(path, sizeof path, "%s/tex-%08x.bin", dir, r[25]);
            uint8_t *file = slurp(path, &size);
            if (!file) return 1;
            const uint32_t *h = (const uint32_t *)file;
            textures[t] = (vk_scene_texture){ .address = r[25], .width = h[0], .height = h[1],
                                             .palette = (const uint16_t *)(file + 8), .texels = file + 8 + 512 };
            texture_files[texture_count++] = file;
        }
        vk_scene_tri *out = &tris[tri_count++];
        out->kind = r[0]; out->texture = t;
        memcpy(out->v, r + 7, sizeof out->v);
        kind_counts[r[0]]++;
    }
    printf("batch %u: kind0=%u kind1=%u textures=%u guest %ux%u\n", batch, kind_counts[0], kind_counts[1], texture_count, gw, gh);

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ic = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ic.pApplicationInfo = &app;
    VkInstance instance;
    if (vkCreateInstance(&ic, NULL, &instance) != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance failed\n"); return 1; }
    uint32_t n = 1; VkPhysicalDevice physical;
    if (vkEnumeratePhysicalDevices(instance, &n, &physical) < 0 || n == 0) { fprintf(stderr, "no Vulkan device\n"); return 1; }
    uint32_t family = 0, qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, NULL);
    VkQueueFamilyProperties *qs = calloc(qn, sizeof *qs);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qs);
    for (family = 0; family < qn && !(qs[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); family++);
    free(qs);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qc = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &priority };
    VkDeviceCreateInfo dc = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qc;
    VkDevice device; VkQueue queue;
    if (vkCreateDevice(physical, &dc, NULL, &device) != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice failed\n"); return 1; }
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props);
    printf("device: %s\n", props.deviceName);

    vk_scene scene;
    int status = 1;
    if (!vk_scene_create(&scene, physical, device, queue, family, gw, gh, scale)) goto cleanup;
    vk_scene_layers layers;
    if (!vk_scene_render(&scene, tris, tri_count, textures, texture_count, 0, &layers)) goto cleanup;
    uint32_t first_uploads = scene.texture_uploads;
    printf("first render uploaded %u textures\n", first_uploads);
    size_t rgba_size = (size_t)scene.width * scene.height * 4;
    uint8_t *first_rgba = malloc(rgba_size);
    if (!first_rgba) goto cleanup;
    memcpy(first_rgba, layers.rgba, rgba_size);
    int rendered = vk_scene_render(&scene, tris, tri_count, textures, texture_count, 0, &layers);
    const uint8_t *rgba = layers.rgba;
    int unchanged = rendered && !memcmp(first_rgba, rgba, rgba_size);
    free(first_rgba);
    if (!rendered) goto cleanup;
    printf("second render uploaded %u textures\n", scene.texture_uploads);
    if (!unchanged || (first_uploads <= VKS_TEXTURE_CACHE_SIZE && scene.texture_uploads != 0)) {
        fprintf(stderr, "warm render did not preserve pixels or reuse cached textures\n");
        goto cleanup;
    }
    if (scale != 1) {
        printf("scale %u rendered (%ux%u); comparison only at scale 1\n", scale, scene.width, scene.height);
        goto cache_checks;
    }

    unsigned covered_both = 0, guest_only = 0, gpu_only = 0, colour_diff = 0, exact = 0;
    FILE *diff = NULL;
    snprintf(path, sizeof path, "%s/vk-diff.ppm", dir);
    diff = fopen(path, "wb");
    if (diff) fprintf(diff, "P6\n%u %u\n255\n", gw, gh);
    for (uint32_t i = 0; i < gw * gh; i++) {
        uint16_t g = guest[i];
        uint16_t p = (uint16_t)(((rgba[i * 4] >> 3) << 11) | ((rgba[i * 4 + 1] >> 2) << 5) | (rgba[i * 4 + 2] >> 3));
        int gpu_drawn = rgba[i * 4 + 3] != 0, guest_drawn = g != 0;
        uint8_t shade[3] = { 0, 0, 0 };
        if (gpu_drawn && guest_drawn) { covered_both++; if (g == p) { exact++; shade[1] = 90; } else { colour_diff++; shade[0] = 255; } }
        else if (guest_drawn) { guest_only++; shade[2] = 255; }
        else if (gpu_drawn) { gpu_only++; shade[0] = 255; shade[1] = 255; }
        if (diff) fwrite(shade, 1, 3, diff);
    }
    if (diff) fclose(diff);
    printf("both drawn %u (exact colour %u, different colour %u), guest-only %u, gpu-only %u\n",
           covered_both, exact, colour_diff, guest_only, gpu_only);
cache_checks:
    if (!check_cache(physical, device, queue, family)) {
        fprintf(stderr, "cache invalidation, overflow or LRU check failed\n");
        goto cleanup;
    }
    puts("palette and texel invalidation, overflow rendering, and LRU eviction passed");
    if (!check_layers(physical, device, queue, family)) {
        fprintf(stderr, "independent layers, kind-4 depth bias, key discard or output alpha check failed\n");
        goto cleanup;
    }
    puts("independent layers, strict depth, kind-4 bias, key discard, and output alpha passed");
    if (!check_blends(physical, device, queue, family)) {
        fprintf(stderr, "ordered kind-5 blending, depth or direct texture cache check failed\n");
        goto cleanup;
    }
    puts("ordered kind-5 RGB565 blends, depth preservation, and direct texture cache passed");
    if (!check_projection(physical, device, queue, family)) {
        fprintf(stderr, "mixed projection did not match legacy colour and depth layers\n");
        goto cleanup;
    }
    puts("mixed projection, fallback, and kind-5 snapshot match legacy layers");
    status = 0;
cleanup:
    vk_scene_destroy(&scene);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    for (uint32_t i = 0; i < texture_count; i++) free(texture_files[i]);
    free(tris); free(records); free(colour_file);
    return status;
}
