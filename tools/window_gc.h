/* Inlined into the generated standalone AppUi translation. */
#include <SDL.h>
#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
enum { window_scale = 3, screen_base_width = 176, screen_base_height = 208,
       screen_max_width = 640, screen_max_height = 480 };
/* Guest screen in RGB565; sized once by screen_configure() before the guest starts. */
static int window_width = screen_base_width, window_height = screen_base_height;
static uint16_t *pixels;
#define PIXEL(x, y) pixels[(size_t)(y) * (size_t)window_width + (size_t)(x)]
static struct { uint32_t window, font, pen_color, pen_style, brush_color, brush_style, draw_pc; } gc;
static int hold_first_frame;
/* The presentation texture is the guest size times present_scale. The optional Vulkan scene pass
   (tools/host_tris.h) can fill it alongside unmodified guest-drawn pixels. */
static unsigned present_scale = 1;
static unsigned host_scene_scale(void);
static int host_scene_has_blit(void);
static void host_scene_compose(uint32_t *out, const uint16_t *guest, unsigned width, unsigned height,
                               unsigned scale, const uint32_t *lut);

static uint32_t text_data(const uint32_t *stack, uint32_t desc, uint32_t *len) {
    uint32_t header = guest_read(stack, desc);
    *len = header & 0x0fffffffu;
    switch (header >> 28) {
    case 0: return desc + 4;
    case 1: return guest_read(stack, desc + 4);
    case 2: return guest_read(stack, desc + 8);
    case 3: return desc + 8;
    case 4: { /* EBufCPtr points to the owning EBufC header, not its text. */
        uint32_t owner = guest_read(stack, desc + 8);
        return owner && guest_read(stack, owner) == *len ? owner + 4 : 0;
    }
    default: return 0;
    }
}

static int copy_text(uint32_t *stack, uint32_t dest, uint32_t src) {
    uint32_t count, old, from = text_data(stack, src, &count), to = text_data(stack, dest, &old);
    uint32_t header = guest_read(stack, dest);
    if (!from || !to || (header >> 28 != 2 && header >> 28 != 3) || count > guest_read(stack, dest + 4)) return 0;
    for (uint32_t i = 0; i < count; i++)
        guest_write_small(stack, to + 2*i, guest_read_small(stack, from + 2*i, 2), 2);
    guest_write(stack, dest, (header & 0xf0000000u) | count);
    return 1;
}

static FT_Face get_face(const uint32_t *stack, uint32_t font) {
    static FT_Library library;
    static FT_Face face;
    if (!font) return NULL;
    if (!face) {
        if (FT_Init_FreeType(&library)) return NULL;
        FcPattern *request = FcNameParse((const FcChar8 *)"sans-serif");
        if (!request) return NULL;
        FcConfigSubstitute(NULL, request, FcMatchPattern);
        FcDefaultSubstitute(request);
        FcResult result;
        FcPattern *match = FcFontMatch(NULL, request, &result);
        FcPatternDestroy(request);
        if (!match) return NULL;
        FcChar8 *path;
        int index = 0;
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        int ok = FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch &&
                 !FT_New_Face(library, (const char *)path, index, &face);
        FcPatternDestroy(match);
        if (!ok) return NULL;
    }
    unsigned height = guest_read(stack, font + 56);
    if (!height || height > 1440 || FT_Set_Pixel_Sizes(face, 0, (height + 7) / 15)) return NULL;
    return face;
}

static int font_measure(const uint32_t *stack, uint32_t font, uint32_t text, uint32_t *out) {
    FT_Face face = get_face(stack, font);
    if (!face) return 0;
    if (!text) { *out = (uint32_t)((face->size->metrics.ascender + 63) >> 6); return 1; }
    uint32_t len, data = text_data(stack, text, &len);
    if (!data || len > 4096) return 0;
    *out = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (FT_Load_Char(face, guest_read_small(stack, data + 2*i, 2), FT_LOAD_DEFAULT)) return 0;
        *out += (uint32_t)(face->glyph->advance.x >> 6);
    }
    return 1;
}

static uint16_t rgb565(uint32_t color) {
    return (uint16_t)(((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 31));
}

static int draw_rect(const uint32_t *stack, uint32_t rect) {
    int x0 = (int32_t)guest_read(stack, rect), y0 = (int32_t)guest_read(stack, rect + 4);
    int x1 = (int32_t)guest_read(stack, rect + 8), y1 = (int32_t)guest_read(stack, rect + 12);
    if (gc.pen_style > 1 || gc.brush_style > 1) return 0;
    uint16_t pen = rgb565(gc.pen_color), brush = rgb565(gc.brush_color);
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < window_height; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < window_width; x++) {
            if (gc.pen_style && (x == x0 || x == x1-1 || y == y0 || y == y1-1)) PIXEL(x, y) = pen;
            else if (gc.brush_style) PIXEL(x, y) = brush;
        }
    return 1;
}

static int raster_text(const uint32_t *stack, uint32_t text, uint32_t point, uint32_t font, uint32_t color,
                       unsigned char *surface, unsigned width, unsigned height, unsigned stride, uint32_t pc) {
    FT_Face face = get_face(stack, font);
    uint32_t len, data = text_data(stack, text, &len);
    if (!face || !data || len > 4096) return 0;
    int64_t x = (int32_t)guest_read(stack, point), baseline = (int32_t)guest_read(stack, point + 4);
    /* Symbian's glyph drawing uses pen color even with ENullPen. */
    uint16_t pen = rgb565(color);
    unsigned changed = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (FT_Load_Char(face, guest_read_small(stack, data + 2*i, 2), FT_LOAD_RENDER)) return 0;
        FT_GlyphSlot glyph = face->glyph;
        FT_Bitmap *bitmap = &glyph->bitmap;
        if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || bitmap->pitch < 0) return 0;
        for (unsigned row = 0; row < bitmap->rows; row++) {
            int64_t dy = baseline - glyph->bitmap_top + row;
            if (dy < 0 || dy >= height) continue;
            for (unsigned col = 0; col < bitmap->width; col++) {
                int64_t dx = x + glyph->bitmap_left + col;
                if (dx < 0 || dx >= width) continue;
                unsigned a = bitmap->buffer[row * (unsigned)bitmap->pitch + col];
                if (!a) continue;
                unsigned char *pixel = surface + dy * stride + dx * 2;
                uint16_t bg, blended = 0;
                memcpy(&bg, pixel, sizeof bg);
                for (unsigned shift = 0; shift <= 11; shift += shift == 5 ? 6 : 5) {
                    unsigned mask = shift == 5 ? 63 : 31;
                    blended |= (uint16_t)(((((pen >> shift) & mask) * a + ((bg >> shift) & mask) * (255-a) + 127) / 255) << shift);
                }
                changed += blended != bg;
                memcpy(pixel, &blended, sizeof blended);
            }
        }
        x += glyph->advance.x >> 6;
    }
    if (changed && pc) {
        gc.draw_pc = pc;
        fprintf(stderr, "guest DrawText pc=%08x glyphs=%u changed_pixels=%u\n", pc, len, changed);
    }
    return 1;
}

static int draw_text(const uint32_t *stack, uint32_t text, uint32_t point, uint32_t pc) {
    return raster_text(stack, text, point, gc.font, gc.pen_color, (unsigned char *)pixels,
                       window_width, window_height, (unsigned)window_width * sizeof *pixels, pc);
}

/* Presentation is host-only: the guest still draws into the fixed 176x208
   RGB565 surface above and this code only transfers it to the window. The
   slice is Vulkan: SDL supplies the instance extensions and the surface, the
   guest surface is uploaded through a staging buffer into an R5G6B5 image,
   and one triangle-strip pass samples that image onto the swapchain. The
   guest image keeps its 176:208 aspect and is centred between black bars, so
   cutscenes are never stretched. Presentation never falls back to another
   graphics API: every failed step is reported and the run stops.
   Environment entry points (all optional; effective values are logged once):
     ISLAND_WINDOW_SCALE=1..32             initial window scale (default 3)
     ISLAND_WINDOW_SIZE=WxH                initial window size, overrides scale
     ISLAND_HIGHDPI=0|1                    SDL_WINDOW_ALLOW_HIGHDPI (default 1)
     ISLAND_PRESENT_FILTER=nearest|linear  sampler filtering (default nearest)
   For example: ISLAND_WINDOW_SIZE=1280x720 ISLAND_PRESENT_FILTER=linear.
   This header calls the Vulkan loader and the surface/swapchain entry points
   directly, so the generated translation must also be linked with the
   Vulkan loader: add $(pkg-config --libs vulkan) to the compile line
   (pkg-config sdl2 does not imply it). */
typedef struct {
    int window_width, window_height, highdpi, linear_filter, colorspace_p3;
    const char *filter_name;
} present_settings;

enum { present_max_swapchain_images = 8, present_max_instance_extensions = 16 };
enum { present_color_srgb_hw, present_color_srgb_encoded, present_color_display_p3 };

static present_settings present;
static int present_settings_loaded;
static SDL_Window *present_window;

typedef struct {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    int color_mode;
    VkFormat texture_format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[present_max_swapchain_images];
    VkImageView views[present_max_swapchain_images];
    VkFramebuffer framebuffers[present_max_swapchain_images];
    VkRenderPass render_pass;
    VkDescriptorSetLayout descriptor_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkSampler sampler;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore image_available;
    VkSemaphore frame_finished;
    VkFence frame_fence;
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    void *staging_data;
    VkDeviceSize staging_size;
    VkImage texture;
    VkDeviceMemory texture_memory;
    VkImageView texture_view;
    VkImageLayout texture_layout;
    int logged_width, logged_height, logged_x, logged_y, logged_w, logged_h;
} present_vulkan;

static present_vulkan vk_present;

static uint32_t present_lut[65536];

static const char *present_color_name(void) {
    switch (vk_present.color_mode) {
    case present_color_srgb_hw: return "sRGB(hardware-encoded)";
    case present_color_display_p3: return "Display-P3";
    default: return "sRGB(encoded)";
    }
}

static float present_srgb_decode(float v) {
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static float present_srgb_encode(float v) {
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

/* RGB565 -> RGBA8 for the guest image. Channels expand by exact scaling (5 bit x/31, 6 bit x/63).
   Display-P3 mode re-expresses the sRGB colour in P3 primaries (same transfer function). */
static void present_build_lut(void) {
    for (unsigned i = 0; i < 65536; i++) {
        unsigned r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
        unsigned r8 = (r * 255 + 15) / 31, g8 = (g * 255 + 31) / 63, b8 = (b * 255 + 15) / 31;
        if (vk_present.color_mode == present_color_display_p3) {
            float lr = present_srgb_decode(r8 / 255.0f), lg = present_srgb_decode(g8 / 255.0f),
                  lb = present_srgb_decode(b8 / 255.0f);
            float pr = 0.8224621f * lr + 0.1775380f * lg;
            float pg = 0.0331941f * lr + 0.9668058f * lg;
            float pb = 0.0170827f * lr + 0.0723974f * lg + 0.9105199f * lb;
            r8 = (unsigned)lrintf(present_srgb_encode(pr) * 255.0f);
            g8 = (unsigned)lrintf(present_srgb_encode(pg) * 255.0f);
            b8 = (unsigned)lrintf(present_srgb_encode(pb) * 255.0f);
        }
        present_lut[i] = r8 | (g8 << 8) | (b8 << 16) | UINT32_C(0xff000000);
    }
}

static int present_word_is(const char *text, const char *word) {
    while (*text && *word) {
        char letter = *text++;
        if (letter >= 'A' && letter <= 'Z') letter = (char)(letter - 'A' + 'a');
        if (letter != *word++) return 0;
    }
    return !*text && !*word;
}

static int present_env_int(const char *name, int fallback, int low, int high) {
    const char *text = getenv(name);
    if (!text || !*text) return fallback;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end || value < low || value > high) {
        fprintf(stderr, "presentation: %s=\"%s\" is invalid (want %d..%d), using %d\n",
                name, text, low, high, fallback);
        return fallback;
    }
    return (int)value;
}

static int present_env_bool(const char *name, int fallback) {
    const char *text = getenv(name);
    if (!text || !*text) return fallback;
    if (present_word_is(text, "1") || present_word_is(text, "on") ||
        present_word_is(text, "true") || present_word_is(text, "yes")) return 1;
    if (present_word_is(text, "0") || present_word_is(text, "off") ||
        present_word_is(text, "false") || present_word_is(text, "no")) return 0;
    fprintf(stderr, "presentation: %s=\"%s\" is invalid (want 0|1), using %d\n", name, text, fallback);
    return fallback;
}

/* The guest screen is a runtime size. The original UI centres its 176x208 art inside any larger
   screen and 3D uses the full viewport, so a wider screen is rendered by the guest itself;
   nothing here changes game logic. ISLAND_SCREEN=native|WxH|A:B (default 16:9, height 208). */
static void screen_configure(void) {
    if (pixels) return;
    int width = screen_base_width, height = screen_base_height;
    const char *spec = getenv("ISLAND_SCREEN");
    if (!spec || !*spec) spec = "16:9";
    int a = 0, b = 0;
    char tail = 0;
    if (present_word_is(spec, "native")) {
    } else if (sscanf(spec, "%dx%d%c", &a, &b, &tail) == 2 && a >= screen_base_width &&
               a <= screen_max_width && b >= screen_base_height && b <= screen_max_height) {
        width = a;
        height = b;
    } else if (sscanf(spec, "%d:%d%c", &a, &b, &tail) == 2 && a > 0 && b > 0) {
        width = (int)(((int64_t)height * a + b / 2) / b);
    } else {
        fprintf(stderr, "presentation: ISLAND_SCREEN=\"%s\" is invalid (want native|WxH|A:B), using native\n", spec);
    }
    if (width < screen_base_width) width = screen_base_width;
    if (width > screen_max_width) width = screen_max_width;
    width &= ~1;
    window_width = width;
    window_height = height;
    pixels = calloc((size_t)width * height, sizeof *pixels);
    if (!pixels) {
        fputs("guest screen: out of memory\n", stderr);
        exit(1);
    }
    fprintf(stderr, "guest screen: %dx%d RGB565\n", width, height);
}

static void present_load_settings(void) {
    if (present_settings_loaded) return;
    present_settings_loaded = 1;

    int scale = present_env_int("ISLAND_WINDOW_SCALE", window_scale, 1, 32);
    int width = window_width * scale, height = window_height * scale;
    const char *size = getenv("ISLAND_WINDOW_SIZE");
    if (size && *size) {
        int requested_width = 0, requested_height = 0;
        char tail = 0;
        if (sscanf(size, "%dx%d%c", &requested_width, &requested_height, &tail) == 2 &&
            requested_width > 0 && requested_height > 0) {
            width = requested_width;
            height = requested_height;
        } else {
            fprintf(stderr, "presentation: ISLAND_WINDOW_SIZE=\"%s\" is invalid (want WxH), using %dx%d\n",
                    size, width, height);
        }
    }
    present.window_width = width;
    present.window_height = height;
    present.highdpi = present_env_bool("ISLAND_HIGHDPI", 1);

    present.linear_filter = 0;
    present.filter_name = "nearest";
    const char *filter = getenv("ISLAND_PRESENT_FILTER");
    if (filter && *filter) {
        if (present_word_is(filter, "linear") || present_word_is(filter, "1")) {
            present.linear_filter = 1;
            present.filter_name = "linear";
        } else if (!present_word_is(filter, "nearest") && !present_word_is(filter, "0")) {
            fprintf(stderr, "presentation: ISLAND_PRESENT_FILTER=\"%s\" is invalid (want nearest|linear), using nearest\n", filter);
        }
    }
    present.colorspace_p3 = 0;
    const char *space = getenv("ISLAND_COLORSPACE");
    if (space && *space) {
        if (present_word_is(space, "p3") || present_word_is(space, "display-p3")) present.colorspace_p3 = 1;
        else if (!present_word_is(space, "srgb") && !present_word_is(space, "auto"))
            fprintf(stderr, "presentation: ISLAND_COLORSPACE=\"%s\" is invalid (want srgb|p3|auto), using srgb\n", space);
    }
}

/* Vulkan 1.0 core SPIR-V for the presentation pass: four strip vertices cover
   the dynamic viewport with uv = 0..1, and the fragment stage samples the
   guest image through descriptor set 0 binding 0 at implicit LOD. Embedding
   the modules keeps shader compilation out of the build. */
static const uint32_t present_vertex_spirv[] = {
    0x07230203, 0x00010000, 0x00000000, 0x00000022, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
    0x00000000, 0x00000001, 0x0008000f, 0x00000000, 0x0000000d, 0x6e69616d, 0x00000000, 0x0000000a,
    0x0000000b, 0x0000000c, 0x00040047, 0x0000000a, 0x0000000b, 0x0000002a, 0x00040047, 0x0000000b,
    0x0000001e, 0x00000000, 0x00040047, 0x0000000c, 0x0000000b, 0x00000000, 0x00020013, 0x00000001,
    0x00030021, 0x00000002, 0x00000001, 0x00030016, 0x00000003, 0x00000020, 0x00040017, 0x00000004,
    0x00000003, 0x00000002, 0x00040017, 0x00000005, 0x00000003, 0x00000004, 0x00040015, 0x00000006,
    0x00000020, 0x00000001, 0x00040020, 0x00000007, 0x00000001, 0x00000006, 0x00040020, 0x00000008,
    0x00000003, 0x00000004, 0x00040020, 0x00000009, 0x00000003, 0x00000005, 0x0004003b, 0x00000007,
    0x0000000a, 0x00000001, 0x0004003b, 0x00000008, 0x0000000b, 0x00000003, 0x0004003b, 0x00000009,
    0x0000000c, 0x00000003, 0x0004002b, 0x00000006, 0x0000000f, 0x00000001, 0x0004002b, 0x00000003,
    0x00000010, 0x00000000, 0x0004002b, 0x00000003, 0x00000011, 0x3f800000, 0x0004002b, 0x00000003,
    0x00000012, 0x40000000, 0x0005002c, 0x00000004, 0x00000013, 0x00000012, 0x00000012, 0x0005002c,
    0x00000004, 0x00000014, 0x00000011, 0x00000011, 0x00050036, 0x00000001, 0x0000000d, 0x00000000,
    0x00000002, 0x000200f8, 0x0000000e, 0x0004003d, 0x00000006, 0x00000015, 0x0000000a, 0x000500c7,
    0x00000006, 0x00000016, 0x00000015, 0x0000000f, 0x000500c3, 0x00000006, 0x00000017, 0x00000015,
    0x0000000f, 0x000500c7, 0x00000006, 0x00000018, 0x00000017, 0x0000000f, 0x0004006f, 0x00000003,
    0x00000019, 0x00000016, 0x0004006f, 0x00000003, 0x0000001a, 0x00000018, 0x00050050, 0x00000004,
    0x0000001b, 0x00000019, 0x0000001a, 0x0005008e, 0x00000004, 0x0000001c, 0x0000001b, 0x00000012,
    0x00050083, 0x00000004, 0x0000001d, 0x0000001c, 0x00000014, 0x00050051, 0x00000003, 0x0000001f,
    0x0000001d, 0x00000000, 0x00050051, 0x00000003, 0x00000020, 0x0000001d, 0x00000001, 0x00070050,
    0x00000005, 0x00000021, 0x0000001f, 0x00000020, 0x00000010, 0x00000011, 0x0003003e, 0x0000000c,
    0x00000021, 0x0003003e, 0x0000000b, 0x0000001b, 0x000100fd, 0x00010038,
};
static const uint32_t present_fragment_spirv[] = {
    0x07230203, 0x00010000, 0x00000000, 0x00000013, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
    0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x0000000e, 0x6e69616d, 0x00000000, 0x0000000c,
    0x0000000d, 0x00030010, 0x0000000e, 0x00000007, 0x00040047, 0x0000000b, 0x00000022, 0x00000000,
    0x00040047, 0x0000000b, 0x00000021, 0x00000000, 0x00040047, 0x0000000c, 0x0000001e, 0x00000000,
    0x00040047, 0x0000000d, 0x0000001e, 0x00000000, 0x00020013, 0x00000001, 0x00030021, 0x00000002,
    0x00000001, 0x00030016, 0x00000003, 0x00000020, 0x00040017, 0x00000004, 0x00000003, 0x00000002,
    0x00040017, 0x00000005, 0x00000003, 0x00000004, 0x00090019, 0x00000006, 0x00000003, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x00000007, 0x00000006,
    0x00040020, 0x00000008, 0x00000000, 0x00000007, 0x00040020, 0x00000009, 0x00000001, 0x00000004,
    0x00040020, 0x0000000a, 0x00000003, 0x00000005, 0x0004003b, 0x00000008, 0x0000000b, 0x00000000,
    0x0004003b, 0x00000009, 0x0000000c, 0x00000001, 0x0004003b, 0x0000000a, 0x0000000d, 0x00000003,
    0x00050036, 0x00000001, 0x0000000e, 0x00000000, 0x00000002, 0x000200f8, 0x0000000f, 0x0004003d,
    0x00000007, 0x00000010, 0x0000000b, 0x0004003d, 0x00000004, 0x00000011, 0x0000000c, 0x00050057,
    0x00000005, 0x00000012, 0x00000010, 0x00000011, 0x0003003e, 0x0000000d, 0x00000012, 0x000100fd,
    0x00010038,
};

static const char *present_result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "unlisted VkResult";
    }
}

static int present_vk_check(VkResult result, const char *what) {
    if (result == VK_SUCCESS) return 1;
    fprintf(stderr, "presentation: vulkan: %s failed: %s (%d)\n",
            what, present_result_name(result), (int)result);
    return 0;
}

static int present_sdl_fail(const char *what) {
    fprintf(stderr, "presentation: %s failed: %s\n", what, SDL_GetError());
    return 0;
}

static int present_memory_type(uint32_t candidates, VkMemoryPropertyFlags wanted, uint32_t *index) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(vk_present.physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
        if ((candidates & (UINT32_C(1) << i)) != 0 &&
            (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            *index = i;
            return 1;
        }
    return 0;
}

static int present_create_instance(void) {
    unsigned int count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(present_window, &count, NULL))
        return present_sdl_fail("SDL_Vulkan_GetInstanceExtensions");
    if (!count || count > (unsigned)present_max_instance_extensions) {
        fprintf(stderr, "presentation: vulkan: SDL requested %u instance extensions (limit %d)\n",
                count, (int)present_max_instance_extensions);
        return 0;
    }
    const char *extensions[present_max_instance_extensions];
    if (!SDL_Vulkan_GetInstanceExtensions(present_window, &count, extensions))
        return present_sdl_fail("SDL_Vulkan_GetInstanceExtensions");
    /* VK_EXT_swapchain_colorspace lists non-sRGB surface colour spaces such as Display-P3. */
    uint32_t available = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &available, NULL) == VK_SUCCESS && available) {
        VkExtensionProperties properties[256];
        if (available > 256) available = 256;
        VkResult listed = vkEnumerateInstanceExtensionProperties(NULL, &available, properties);
        if (listed == VK_SUCCESS || listed == VK_INCOMPLETE)
            for (uint32_t i = 0; i < available && i < 256; i++)
                if (!strcmp(properties[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) &&
                    count < (unsigned)present_max_instance_extensions) {
                    extensions[count++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
                    break;
                }
    }
    VkApplicationInfo application = {0};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Lament Island — AOT";
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    create.enabledExtensionCount = count;
    create.ppEnabledExtensionNames = extensions;
    return present_vk_check(vkCreateInstance(&create, NULL, &vk_present.instance), "vkCreateInstance");
}

static int present_supports_swapchain(VkPhysicalDevice device) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS || !count) return 0;
    VkExtensionProperties extensions[256];
    if (count > 256) count = 256;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return 0;
    if (count > 256) count = 256;
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(extensions[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return 1;
    return 0;
}

static int present_find_queue_family(VkPhysicalDevice device, uint32_t *family) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    if (!count) return 0;
    VkQueueFamilyProperties families[32];
    if (count > 32) count = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);
    for (uint32_t i = 0; i < count; i++) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 presentable = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, vk_present.surface, &presentable) != VK_SUCCESS) continue;
        if (presentable) {
            *family = i;
            return 1;
        }
    }
    return 0;
}

static int present_pick_physical_device(void) {
    uint32_t count = 0;
    if (!present_vk_check(vkEnumeratePhysicalDevices(vk_present.instance, &count, NULL),
                          "vkEnumeratePhysicalDevices"))
        return 0;
    if (!count) {
        fprintf(stderr, "presentation: vulkan: no physical devices\n");
        return 0;
    }
    VkPhysicalDevice devices[16];
    if (count > 16) count = 16;
    VkResult result = vkEnumeratePhysicalDevices(vk_present.instance, &count, devices);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return present_vk_check(result, "vkEnumeratePhysicalDevices");
    if (count > 16) count = 16;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t family = 0;
        if (!present_find_queue_family(devices[i], &family)) continue;
        if (!present_supports_swapchain(devices[i])) continue;
        vk_present.physical = devices[i];
        vk_present.queue_family = family;
        return 1;
    }
    fprintf(stderr, "presentation: vulkan: no device with a graphics/present queue and %s\n",
            VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    return 0;
}

static int present_create_device(void) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {0};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = vk_present.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char *extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = 1;
    create.ppEnabledExtensionNames = extensions;
    if (!present_vk_check(vkCreateDevice(vk_present.physical, &create, NULL, &vk_present.device),
                          "vkCreateDevice"))
        return 0;
    vkGetDeviceQueue(vk_present.device, vk_present.queue_family, 0, &vk_present.queue);
    return 1;
}

/* The guest frame is sRGB-encoded RGB565 (a handset LCD). Pick the swapchain so the
   compositor and the display see the right primaries and transfer function:
   - sRGB_NONLINEAR + *_SRGB format: the sampler decodes the guest image, the swapchain
     re-encodes it, so linear filtering blends in linear light.
   - sRGB_NONLINEAR + UNORM: guest values are written as already-encoded sRGB.
   - Display-P3 (ISLAND_COLORSPACE=p3, when the surface advertises it): the LUT converts
     sRGB primaries to P3 so colours are not over-saturated on a wide-gamut surface. */
static int present_choose_format(void) {
    uint32_t count = 0;
    if (!present_vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_present.physical, vk_present.surface,
                                                               &count, NULL),
                          "vkGetPhysicalDeviceSurfaceFormatsKHR"))
        return 0;
    if (!count) {
        fprintf(stderr, "presentation: vulkan: surface reports no formats\n");
        return 0;
    }
    VkSurfaceFormatKHR formats[64];
    if (count > 64) count = 64;
    VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(vk_present.physical, vk_present.surface,
                                                           &count, formats);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return present_vk_check(result, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (count > 64) count = 64;
    int best = -1, mode = present_color_srgb_encoded;
    if (present.colorspace_p3)
        for (uint32_t i = 0; i < count && best < 0; i++)
            if (formats[i].colorSpace == VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT &&
                (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
                 formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)) {
                best = (int)i;
                mode = present_color_display_p3;
            }
    if (present.colorspace_p3 && best < 0)
        fprintf(stderr, "presentation: surface does not advertise Display-P3; using sRGB\n");
    for (uint32_t i = 0; i < count && best < 0; i++)
        if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
             formats[i].format == VK_FORMAT_R8G8B8A8_SRGB)) {
            best = (int)i;
            mode = present_color_srgb_hw;
        }
    for (uint32_t i = 0; i < count && best < 0; i++)
        if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
             formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)) {
            best = (int)i;
            mode = present_color_srgb_encoded;
        }
    if (best < 0) {
        fprintf(stderr, "presentation: no 8-bit sRGB surface format; using format %d as sRGB-encoded\n",
                (int)formats[0].format);
        best = 0;
    }
    vk_present.format = formats[best].format;
    vk_present.color_space = formats[best].colorSpace;
    vk_present.color_mode = mode;
    present_build_lut();
    return 1;
}

static int present_create_render_pass(void) {
    VkAttachmentDescription attachment = {0};
    attachment.format = vk_present.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color = {0};
    color.attachment = 0;
    color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency dependency = {0};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create.attachmentCount = 1;
    create.pAttachments = &attachment;
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    create.dependencyCount = 1;
    create.pDependencies = &dependency;
    return present_vk_check(vkCreateRenderPass(vk_present.device, &create, NULL, &vk_present.render_pass),
                            "vkCreateRenderPass");
}

static VkShaderModule present_shader_module(const uint32_t *code, size_t size, const char *what) {
    VkShaderModuleCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create.codeSize = size;
    create.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (!present_vk_check(vkCreateShaderModule(vk_present.device, &create, NULL, &module), what))
        return VK_NULL_HANDLE;
    return module;
}

static int present_create_pipeline(void) {
    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set = {0};
    set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set.bindingCount = 1;
    set.pBindings = &binding;
    if (!present_vk_check(vkCreateDescriptorSetLayout(vk_present.device, &set, NULL,
                                                      &vk_present.descriptor_layout),
                          "vkCreateDescriptorSetLayout"))
        return 0;
    VkPipelineLayoutCreateInfo layout = {0};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &vk_present.descriptor_layout;
    if (!present_vk_check(vkCreatePipelineLayout(vk_present.device, &layout, NULL,
                                                 &vk_present.pipeline_layout),
                          "vkCreatePipelineLayout"))
        return 0;

    VkShaderModule vertex = present_shader_module(present_vertex_spirv, sizeof present_vertex_spirv,
                                                  "vkCreateShaderModule(vertex)");
    if (!vertex) return 0;
    VkShaderModule fragment = present_shader_module(present_fragment_spirv, sizeof present_fragment_spirv,
                                                    "vkCreateShaderModule(fragment)");
    if (!fragment) return 0;

    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input = {0};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly = {0};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport = {0};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {0};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment = {0};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {0};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {0};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    create.stageCount = 2;
    create.pStages = stages;
    create.pVertexInputState = &vertex_input;
    create.pInputAssemblyState = &assembly;
    create.pViewportState = &viewport;
    create.pRasterizationState = &raster;
    create.pMultisampleState = &multisample;
    create.pColorBlendState = &blend;
    create.pDynamicState = &dynamic;
    create.layout = vk_present.pipeline_layout;
    create.renderPass = vk_present.render_pass;
    VkResult result = vkCreateGraphicsPipelines(vk_present.device, VK_NULL_HANDLE, 1, &create, NULL,
                                               &vk_present.pipeline);
    vkDestroyShaderModule(vk_present.device, vertex, NULL);
    vkDestroyShaderModule(vk_present.device, fragment, NULL);
    return present_vk_check(result, "vkCreateGraphicsPipelines");
}

static int present_create_swapchain(void) {
    VkSurfaceCapabilitiesKHR capabilities = {0};
    if (!present_vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_present.physical,
                                                                    vk_present.surface, &capabilities),
                          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return 0;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        int width = 0, height = 0;
        SDL_Vulkan_GetDrawableSize(present_window, &width, &height);
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        extent.width = (uint32_t)width;
        extent.height = (uint32_t)height;
        if (extent.width < capabilities.minImageExtent.width) extent.width = capabilities.minImageExtent.width;
        if (extent.width > capabilities.maxImageExtent.width) extent.width = capabilities.maxImageExtent.width;
        if (extent.height < capabilities.minImageExtent.height) extent.height = capabilities.minImageExtent.height;
        if (extent.height > capabilities.maxImageExtent.height) extent.height = capabilities.maxImageExtent.height;
    }
    if (!extent.width || !extent.height) {
        fprintf(stderr, "presentation: vulkan: window drawable is empty\n");
        return 0;
    }
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
    if (image_count > (uint32_t)present_max_swapchain_images) {
        fprintf(stderr, "presentation: vulkan: swapchain asks for %u images (limit %d)\n",
                image_count, (int)present_max_swapchain_images);
        return 0;
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & alpha)) {
        if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
            alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
            alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
            alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        else {
            fprintf(stderr, "presentation: vulkan: surface supports no composite alpha mode\n");
            return 0;
        }
    }
    VkSwapchainCreateInfoKHR create = {0};
    create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create.surface = vk_present.surface;
    create.minImageCount = image_count;
    create.imageFormat = vk_present.format;
    create.imageColorSpace = vk_present.color_space;
    create.imageExtent = extent;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = alpha;
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    create.clipped = VK_TRUE;
    if (!present_vk_check(vkCreateSwapchainKHR(vk_present.device, &create, NULL, &vk_present.swapchain),
                          "vkCreateSwapchainKHR"))
        return 0;
    vk_present.extent = extent;
    image_count = 0;
    if (!present_vk_check(vkGetSwapchainImagesKHR(vk_present.device, vk_present.swapchain,
                                                  &image_count, NULL),
                          "vkGetSwapchainImagesKHR"))
        return 0;
    if (!image_count || image_count > (uint32_t)present_max_swapchain_images) {
        fprintf(stderr, "presentation: vulkan: swapchain reports %u images (limit %d)\n",
                image_count, (int)present_max_swapchain_images);
        return 0;
    }
    if (!present_vk_check(vkGetSwapchainImagesKHR(vk_present.device, vk_present.swapchain,
                                                  &image_count, vk_present.images),
                          "vkGetSwapchainImagesKHR"))
        return 0;
    vk_present.image_count = image_count;
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view = {0};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = vk_present.images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = vk_present.format;
        view.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (!present_vk_check(vkCreateImageView(vk_present.device, &view, NULL, &vk_present.views[i]),
                              "vkCreateImageView(swapchain)"))
            return 0;
        VkFramebufferCreateInfo framebuffer = {0};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = vk_present.render_pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &vk_present.views[i];
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        if (!present_vk_check(vkCreateFramebuffer(vk_present.device, &framebuffer, NULL,
                                                  &vk_present.framebuffers[i]),
                              "vkCreateFramebuffer"))
            return 0;
    }
    return 1;
}

static void present_destroy_swapchain(void) {
    for (uint32_t i = 0; i < vk_present.image_count; i++) {
        if (vk_present.framebuffers[i]) vkDestroyFramebuffer(vk_present.device, vk_present.framebuffers[i], NULL);
        if (vk_present.views[i]) vkDestroyImageView(vk_present.device, vk_present.views[i], NULL);
        vk_present.framebuffers[i] = VK_NULL_HANDLE;
        vk_present.views[i] = VK_NULL_HANDLE;
        vk_present.images[i] = VK_NULL_HANDLE;
    }
    vk_present.image_count = 0;
    if (vk_present.swapchain) {
        vkDestroySwapchainKHR(vk_present.device, vk_present.swapchain, NULL);
        vk_present.swapchain = VK_NULL_HANDLE;
    }
}

static int present_recreate_swapchain(void) {
    if (!present_vk_check(vkDeviceWaitIdle(vk_present.device), "vkDeviceWaitIdle")) return 0;
    present_destroy_swapchain();
    if (!present_create_swapchain()) return 0;
    vk_present.logged_width = 0;
    vk_present.logged_height = 0;
    return 1;
}

static int present_create_texture(void) {
    VkFormatProperties properties;
    vk_present.texture_format = vk_present.color_mode == present_color_srgb_hw
                                    ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    vkGetPhysicalDeviceFormatProperties(vk_present.physical, vk_present.texture_format, &properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
        fprintf(stderr, "presentation: vulkan: guest image format %d cannot be sampled and uploaded\n",
                (int)vk_present.texture_format);
        return 0;
    }
    VkImageCreateInfo image = {0};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = vk_present.texture_format;
    image.extent.width = (uint32_t)window_width * present_scale;
    image.extent.height = (uint32_t)window_height * present_scale;
    image.extent.depth = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!present_vk_check(vkCreateImage(vk_present.device, &image, NULL, &vk_present.texture),
                          "vkCreateImage(RGB565)"))
        return 0;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk_present.device, vk_present.texture, &requirements);
    uint32_t type = 0;
    if (!present_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        fprintf(stderr, "presentation: vulkan: no device-local memory for the RGB565 image\n");
        return 0;
    }
    VkMemoryAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (!present_vk_check(vkAllocateMemory(vk_present.device, &allocate, NULL, &vk_present.texture_memory),
                          "vkAllocateMemory(RGB565)"))
        return 0;
    if (!present_vk_check(vkBindImageMemory(vk_present.device, vk_present.texture,
                                            vk_present.texture_memory, 0),
                          "vkBindImageMemory(RGB565)"))
        return 0;
    VkImageViewCreateInfo view = {0};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = vk_present.texture;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = vk_present.texture_format;
    view.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (!present_vk_check(vkCreateImageView(vk_present.device, &view, NULL, &vk_present.texture_view),
                          "vkCreateImageView(RGB565)"))
        return 0;
    vk_present.texture_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    vk_present.staging_size = (VkDeviceSize)window_width * window_height * present_scale * present_scale * sizeof(uint32_t);
    VkBufferCreateInfo buffer = {0};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = vk_present.staging_size;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!present_vk_check(vkCreateBuffer(vk_present.device, &buffer, NULL, &vk_present.staging_buffer),
                          "vkCreateBuffer(staging)"))
        return 0;
    vkGetBufferMemoryRequirements(vk_present.device, vk_present.staging_buffer, &requirements);
    if (!present_memory_type(requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &type)) {
        fprintf(stderr, "presentation: vulkan: no host-visible memory for the staging buffer\n");
        return 0;
    }
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (!present_vk_check(vkAllocateMemory(vk_present.device, &allocate, NULL, &vk_present.staging_memory),
                          "vkAllocateMemory(staging)"))
        return 0;
    if (!present_vk_check(vkBindBufferMemory(vk_present.device, vk_present.staging_buffer,
                                             vk_present.staging_memory, 0),
                          "vkBindBufferMemory(staging)"))
        return 0;
    return present_vk_check(vkMapMemory(vk_present.device, vk_present.staging_memory, 0,
                                        VK_WHOLE_SIZE, 0, &vk_present.staging_data),
                            "vkMapMemory(staging)");
}

static int present_create_sampler(void) {
    VkSamplerCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    create.magFilter = present.linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    create.minFilter = present.linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    create.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.maxLod = 0.0f;
    create.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    return present_vk_check(vkCreateSampler(vk_present.device, &create, NULL, &vk_present.sampler),
                            "vkCreateSampler");
}

static int present_create_descriptor(void) {
    VkDescriptorPoolSize size = {0};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!present_vk_check(vkCreateDescriptorPool(vk_present.device, &pool, NULL, &vk_present.descriptor_pool),
                          "vkCreateDescriptorPool"))
        return 0;
    VkDescriptorSetAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = vk_present.descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &vk_present.descriptor_layout;
    if (!present_vk_check(vkAllocateDescriptorSets(vk_present.device, &allocate, &vk_present.descriptor_set),
                          "vkAllocateDescriptorSets"))
        return 0;
    VkDescriptorImageInfo image = {0};
    image.sampler = vk_present.sampler;
    image.imageView = vk_present.texture_view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vk_present.descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(vk_present.device, 1, &write, 0, NULL);
    return 1;
}

static int present_create_commands(void) {
    VkCommandPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = vk_present.queue_family;
    if (!present_vk_check(vkCreateCommandPool(vk_present.device, &pool, NULL, &vk_present.command_pool),
                          "vkCreateCommandPool"))
        return 0;
    VkCommandBufferAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = vk_present.command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (!present_vk_check(vkAllocateCommandBuffers(vk_present.device, &allocate, &vk_present.command_buffer),
                          "vkAllocateCommandBuffers"))
        return 0;
    VkSemaphoreCreateInfo semaphore = {0};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (!present_vk_check(vkCreateSemaphore(vk_present.device, &semaphore, NULL, &vk_present.image_available),
                          "vkCreateSemaphore(image)"))
        return 0;
    if (!present_vk_check(vkCreateSemaphore(vk_present.device, &semaphore, NULL, &vk_present.frame_finished),
                          "vkCreateSemaphore(frame)"))
        return 0;
    VkFenceCreateInfo fence = {0};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return present_vk_check(vkCreateFence(vk_present.device, &fence, NULL, &vk_present.frame_fence),
                            "vkCreateFence");
}

static int present_create(void) {
    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) && SDL_Init(SDL_INIT_VIDEO))
        return present_sdl_fail("SDL_Init(SDL_INIT_VIDEO)");
    if (SDL_Vulkan_LoadLibrary(NULL)) return present_sdl_fail("SDL_Vulkan_LoadLibrary");
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (present.highdpi) flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    present_window = SDL_CreateWindow("Lament Island — AOT", SDL_WINDOWPOS_CENTERED,
                                      SDL_WINDOWPOS_CENTERED, present.window_width,
                                      present.window_height, flags);
    if (!present_window) return present_sdl_fail("SDL_CreateWindow(SDL_WINDOW_VULKAN)");
    if (!present_create_instance()) return 0;
    if (!SDL_Vulkan_CreateSurface(present_window, vk_present.instance, &vk_present.surface))
        return present_sdl_fail("SDL_Vulkan_CreateSurface");
    if (!present_pick_physical_device()) return 0;
    if (!present_create_device()) return 0;
    if (!present_choose_format()) return 0;
    if (!present_create_render_pass()) return 0;
    if (!present_create_pipeline()) return 0;
    if (!present_create_swapchain()) return 0;
    present_scale = host_scene_scale();
    if (!present_create_texture()) return 0;
    if (!present_create_sampler()) return 0;
    if (!present_create_descriptor()) return 0;
    if (!present_create_commands()) return 0;
    return 1;
}

static void present_log_settings(void) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(vk_present.physical, &properties);
    int window_width_now = 0, window_height_now = 0, drawable_width = 0, drawable_height = 0;
    SDL_GetWindowSize(present_window, &window_width_now, &window_height_now);
    SDL_Vulkan_GetDrawableSize(present_window, &drawable_width, &drawable_height);
    fprintf(stderr,
            "presentation: window=%dx%d drawable=%dx%d swapchain=%ux%u requested=%dx%d highdpi=%d "
            "filter=%s format=%d colorspace=%s guest=%dx%d present_mode=fifo device=%s\n",
            window_width_now, window_height_now, drawable_width, drawable_height,
            vk_present.extent.width, vk_present.extent.height,
            present.window_width, present.window_height, present.highdpi,
            present.filter_name, (int)vk_present.format, present_color_name(),
            window_width, window_height, properties.deviceName);
}

static void present_destination(int width, int height, int *x, int *y, int *w, int *h) {
    int destination_width, destination_height;
    int64_t height_at_full_width = (int64_t)width * window_height;
    int64_t width_at_full_height = (int64_t)height * window_width;
    if (height_at_full_width <= width_at_full_height) {
        destination_width = width;
        destination_height = (int)((height_at_full_width + window_width / 2) / window_width);
    } else {
        destination_height = height;
        destination_width = (int)((width_at_full_height + window_height / 2) / window_height);
    }
    if (destination_width > width) destination_width = width;
    if (destination_height > height) destination_height = height;
    if (destination_width < 1) destination_width = 1;
    if (destination_height < 1) destination_height = 1;
    *x = (width - destination_width) / 2;
    *y = (height - destination_height) / 2;
    *w = destination_width;
    *h = destination_height;
}

static int present_frame_run(void) {
    if (!present_vk_check(vkWaitForFences(vk_present.device, 1, &vk_present.frame_fence,
                                          VK_TRUE, UINT64_MAX),
                          "vkWaitForFences"))
        return 0;
    uint32_t image_index = 0;
    VkResult result = vkAcquireNextImageKHR(vk_present.device, vk_present.swapchain, UINT64_MAX,
                                            vk_present.image_available, VK_NULL_HANDLE, &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!present_recreate_swapchain()) return 0;
        result = vkAcquireNextImageKHR(vk_present.device, vk_present.swapchain, UINT64_MAX,
                                       vk_present.image_available, VK_NULL_HANDLE, &image_index);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return present_vk_check(result, "vkAcquireNextImageKHR");
    if (!present_vk_check(vkResetFences(vk_present.device, 1, &vk_present.frame_fence), "vkResetFences"))
        return 0;
    if (!present_vk_check(vkResetCommandBuffer(vk_present.command_buffer, 0), "vkResetCommandBuffer"))
        return 0;

    VkCommandBuffer command = vk_present.command_buffer;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!present_vk_check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer")) return 0;
    {
        /* Guest RGB565 goes through the colour LUT while it is copied into staging. */
        uint32_t *out = vk_present.staging_data;
        if (present_scale > 1 || host_scene_has_blit())
            host_scene_compose(out, pixels, (unsigned)window_width, (unsigned)window_height, present_scale, present_lut);
        else
            for (size_t i = 0, n = (size_t)window_width * window_height; i < n; i++)
                out[i] = present_lut[pixels[i]];
    }

    /* Reuse the guest image: every frame converts the RGB565 surface into the
       host-visible staging buffer, uploads it, then samples it. */
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk_present.texture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = vk_present.texture_layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = vk_present.texture_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0 : VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    VkPipelineStageFlags before = vk_present.texture_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                      ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                      : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    vkCmdPipelineBarrier(command, before, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)window_width * present_scale;
    copy.imageExtent.height = (uint32_t)window_height * present_scale;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(command, vk_present.staging_buffer, vk_present.texture,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    vk_present.texture_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkClearValue clear;
    clear.color.float32[0] = 0.0f;
    clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 0.0f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass = {0};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = vk_present.render_pass;
    pass.framebuffer = vk_present.framebuffers[image_index];
    pass.renderArea.extent = vk_present.extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_present.pipeline);
    int x = 0, y = 0, width = 0, height = 0;
    present_destination((int)vk_present.extent.width, (int)vk_present.extent.height,
                        &x, &y, &width, &height);
    VkViewport viewport;
    viewport.x = (float)x;
    viewport.y = (float)y;
    viewport.width = (float)width;
    viewport.height = (float)height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command, 0, 1, &viewport);
    VkRect2D scissor;
    scissor.offset.x = x;
    scissor.offset.y = y;
    scissor.extent.width = (uint32_t)width;
    scissor.extent.height = (uint32_t)height;
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_present.pipeline_layout, 0, 1, &vk_present.descriptor_set, 0, NULL);
    vkCmdDraw(command, 4, 1, 0, 0);
    vkCmdEndRenderPass(command);
    if (!present_vk_check(vkEndCommandBuffer(command), "vkEndCommandBuffer")) return 0;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &vk_present.image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk_present.command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &vk_present.frame_finished;
    if (!present_vk_check(vkQueueSubmit(vk_present.queue, 1, &submit, vk_present.frame_fence),
                          "vkQueueSubmit"))
        return 0;
    VkPresentInfoKHR info = {0};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &vk_present.frame_finished;
    info.swapchainCount = 1;
    info.pSwapchains = &vk_present.swapchain;
    info.pImageIndices = &image_index;
    result = vkQueuePresentKHR(vk_present.queue, &info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        return present_recreate_swapchain();
    if (!present_vk_check(result, "vkQueuePresentKHR")) return 0;
    if ((int)vk_present.extent.width != vk_present.logged_width ||
        (int)vk_present.extent.height != vk_present.logged_height ||
        x != vk_present.logged_x || y != vk_present.logged_y ||
        width != vk_present.logged_w || height != vk_present.logged_h) {
        fprintf(stderr, "presentation: drawable=%ux%u destination=%d,%d %dx%d filter=%s\n",
                vk_present.extent.width, vk_present.extent.height, x, y, width, height,
                present.filter_name);
        vk_present.logged_width = (int)vk_present.extent.width;
        vk_present.logged_height = (int)vk_present.extent.height;
        vk_present.logged_x = x;
        vk_present.logged_y = y;
        vk_present.logged_w = width;
        vk_present.logged_h = height;
    }
    return 1;
}

/* ISLAND_TIMING_LOG=1: mean/max milliseconds spent inside one presentation (fence wait, acquire, compose, present). */
static int present_frame(void) {
    static int enabled = -1;
    static double sum, peak;
    static unsigned frames;
    if (enabled < 0) { const char *text = getenv("ISLAND_TIMING_LOG"); enabled = text && !strcmp(text, "1"); }
    if (!enabled) return present_frame_run();
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int ok = present_frame_run();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6;
    sum += ms;
    if (ms > peak) peak = ms;
    if (++frames == 60) {
        fprintf(stderr, "timing: present mean=%.2f ms max=%.2f ms over 60\n", sum / 60, peak);
        sum = peak = 0; frames = 0;
    }
    return ok;
}

static void debug_frame_presented(void); /* tools/pc_debug.h */
static int publish_window(uint32_t pc) {
    if (!gc.draw_pc) return 1;
    if (!present_window) {
        present_load_settings();
        if (!present_create()) goto error;
        present_log_settings();
    }
    if (!present_frame()) goto error;
    debug_frame_presented();
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < (size_t)window_width * window_height; i++) {
        hash = (hash ^ (pixels[i] & 255u)) * UINT32_C(16777619);
        hash = (hash ^ (pixels[i] >> 8)) * UINT32_C(16777619);
    }
    fprintf(stderr, "guest frame: draw=%08x present=%08x RGB565 FNV-1a=%08x\n", gc.draw_pc, pc, hash);
    if (hold_first_frame) {
        SDL_Event event;
        while (SDL_WaitEvent(&event)) {
            if (event.type == SDL_QUIT) {
                fprintf(stderr, "first frame: exit on SDL event type 0x%x\n", event.type);
                SDL_Quit();
                exit(0);
            }
            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                 event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                 event.window.event == SDL_WINDOWEVENT_RESIZED)) {
                /* Re-present the same guest image: only the swapchain extent
                   and the aspect-correct destination change. */
                if (!present_frame()) goto error;
            }
        }
        goto error;
    }
    gc.draw_pc = 0;
    return 1;
error:
    fprintf(stderr, "window presentation: Vulkan presentation failed\n");
    return 0;
}
