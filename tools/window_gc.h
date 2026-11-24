/* Inlined into the generated standalone AppUi translation. */
#include <SDL.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

enum { window_width = 176, window_height = 208 };
static uint16_t pixels[window_height][window_width];
static struct { uint32_t window, font, pen_color, pen_style, brush_color, brush_style, draw_pc; } gc;
static int hold_first_frame;

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
            if (gc.pen_style && (x == x0 || x == x1-1 || y == y0 || y == y1-1)) pixels[y][x] = pen;
            else if (gc.brush_style) pixels[y][x] = brush;
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
                       window_width, window_height, sizeof pixels[0], pc);
}

static int publish_window(uint32_t pc) {
    static SDL_Window *window;
    static SDL_Renderer *renderer;
    static SDL_Texture *texture;
    if (!gc.draw_pc) return 1;
    if (!window && (SDL_Init(SDL_INIT_VIDEO) ||
        SDL_CreateWindowAndRenderer(window_width * 3, window_height * 3, 0, &window, &renderer) ||
        !(texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                      window_width, window_height)))) goto error;
    SDL_SetWindowTitle(window, "Lament Island — AOT");
    if (SDL_UpdateTexture(texture, NULL, pixels, sizeof pixels[0]) ||
        SDL_RenderCopy(renderer, texture, NULL, NULL)) goto error;
    SDL_RenderPresent(renderer);
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned y = 0; y < window_height; y++)
        for (unsigned x = 0; x < window_width; x++) {
            hash = (hash ^ (pixels[y][x] & 255u)) * UINT32_C(16777619);
            hash = (hash ^ (pixels[y][x] >> 8)) * UINT32_C(16777619);
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
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED) {
                if (SDL_RenderCopy(renderer, texture, NULL, NULL)) goto error;
                SDL_RenderPresent(renderer);
            }
        }
        goto error;
    }
    gc.draw_pc = 0;
    return 1;
error:
    fprintf(stderr, "window presentation: %s\n", SDL_GetError());
    return 0;
}
