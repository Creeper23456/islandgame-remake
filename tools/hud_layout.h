/* Widescreen HUD layout for the in-scene HUD, FUN_00038e80 in the original executable.

   recomp_start.py replaces that function (a pure draw routine called at the end of
   GameSceneCompositeFrame) with hud_draw(). hud_draw() draws exactly the original elements from
   the original bitmaps and the same guest state, only at different positions and size:

     top-left   HP frame (player+0x56c) at 3/4 size with its HP fill (hp/5 px, red; blue while
                player+0x5cd is set, as in the original)
     top-right  bullet icon (player+0x57c) and the "*NN" count from the original digit sheet,
                then the item hint (player+0x574 lower half) on its translucent bar
     bottom-right  translucent bar and the menu label (player+0x574 upper half), as before

   The original drew the HP frame at (0,2), the bullet icon at (0x87,1), the count at (0x91,2)
   and both softkey labels at the bottom corners. Nothing here changes game state, assets or
   scripts; bitmaps are blitted with the same RGB565 colour key (0xF81F) and alpha formula as
   the original blitters and scaled by nearest-neighbour sampling only. */

#define HUD_COLOR_KEY 0xf81fu
#define HUD_HP_SCALE_NUM 3
#define HUD_HP_SCALE_DEN 4

/* Bitmap object: +0 palette, +4 direct RGB565 pixels (else palette indexes at +12), +16 stride,
   +0x10/+0x14 are read below as width/height. Surface: +4 pixels, +0x10 width, +0x14 height. */
struct hud_surface {
    uint32_t pixels;
    uint8_t *scene_mask;
    int width, height;
};

static uint8_t *host_scene_hud_mask(uint32_t pixels, unsigned width, unsigned height);

static struct hud_surface hud_surface_of(const uint32_t *stack, uint32_t surface) {
    struct hud_surface result = { .pixels = guest_read(stack, surface + 4),
                                  .width = (int32_t)guest_read(stack, surface + 0x10),
                                  .height = (int32_t)guest_read(stack, surface + 0x14) };
    result.scene_mask = host_scene_hud_mask(result.pixels, (unsigned)result.width, (unsigned)result.height);
    return result;
}

static uint16_t hud_bitmap_pixel(const uint32_t *stack, uint32_t bitmap, uint32_t stride, int x, int y) {
    uint32_t direct = guest_read(stack, bitmap + 4);
    uint32_t offset = (uint32_t)y * stride + (uint32_t)x;
    if (direct) return (uint16_t)guest_read_small(stack, direct + offset * 2, 2);
    uint32_t indexes = guest_read(stack, bitmap + 12);
    if (!indexes) return HUD_COLOR_KEY;
    uint32_t index = guest_read_small(stack, indexes + offset, 1);
    return (uint16_t)guest_read_small(stack, guest_read(stack, bitmap) + index * 2, 2);
}

/* Draw source rectangle [l,r) x [t,b) of `bitmap` with its top-left at (dx,dy), scaled by
   num/den with nearest sampling. alpha is 0..256; 256 writes the source colour unchanged. */
static void hud_blit(uint32_t *stack, struct hud_surface surface, uint32_t bitmap,
                     int l, int t, int r, int b, int dx, int dy, int num, int den, int alpha) {
    if (!bitmap || !surface.pixels || r <= l || b <= t) return;
    uint32_t stride = guest_read(stack, bitmap + 16);
    int width = (int32_t)guest_read(stack, bitmap + 0x10), height = (int32_t)guest_read(stack, bitmap + 0x14);
    if (r > width) r = width;
    if (b > height) b = height;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r <= l || b <= t) return;
    int dw = ((r - l) * num + den / 2) / den, dh = ((b - t) * num + den / 2) / den;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    for (int oy = 0; oy < dh; oy++) {
        int y = dy + oy;
        if (y < 0 || y >= surface.height) continue;
        int sy = t + ((2 * oy + 1) * den) / (2 * num);
        if (sy >= b) sy = b - 1;
        for (int ox = 0; ox < dw; ox++) {
            int x = dx + ox;
            if (x < 0 || x >= surface.width) continue;
            int sx = l + ((2 * ox + 1) * den) / (2 * num);
            if (sx >= r) sx = r - 1;
            uint16_t source = hud_bitmap_pixel(stack, bitmap, stride, sx, sy);
            if (source == HUD_COLOR_KEY) continue;
            uint32_t at = (uint32_t)y * (uint32_t)surface.width + (uint32_t)x;
            uint32_t address = surface.pixels + at * 2;
            if (alpha < 256) {
                uint32_t below = guest_read_small(stack, address, 2);
                uint32_t keep = 256u - (uint32_t)alpha;
                source = (uint16_t)((((source >> 11) * (uint32_t)alpha + keep * (below >> 11)) >> 8) << 11 |
                                    ((((source >> 5) & 63u) * (uint32_t)alpha + keep * ((below >> 5) & 63u)) >> 8) << 5 |
                                    (((source & 31u) * (uint32_t)alpha + keep * (below & 31u)) >> 8));
            }
            guest_write_small(stack, address, source, 2);
            if (surface.scene_mask) surface.scene_mask[at] = 1;
        }
    }
}

static void hud_fill(uint32_t *stack, struct hud_surface surface, int x0, int y0, int x1, int y1, uint16_t color) {
    if (!surface.pixels) return;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < surface.height; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < surface.width; x++) {
            uint32_t at = (uint32_t)y * (uint32_t)surface.width + (uint32_t)x;
            guest_write_small(stack, surface.pixels + at * 2, color, 2);
            if (surface.scene_mask) surface.scene_mask[at] = 1;
        }
}

/* The original digit sheet has glyphs 7x8 in the order 1..9, 0, '*'. */
static void hud_digits(uint32_t *stack, struct hud_surface surface, uint32_t font, const char *text, int x, int y) {
    uint32_t sheet = guest_read(stack, font + 0x2c);
    for (; *text; text++, x += 7) {
        int glyph;
        if (*text == '0') glyph = 9;
        else if (*text == '*') glyph = 10;
        else if (*text >= '1' && *text <= '9') glyph = *text - '1';
        else continue;
        hud_blit(stack, surface, sheet, glyph * 7, 0, glyph * 7 + 7, 8, x, y, 1, 1, 256);
    }
}

/* Replacement for FUN_00038e80(engine). Field offsets are those of the original function:
   engine+0x1c surface, +0x10 digit font, +0x38 session, +0x5bc/+0x5c0 screen size, +0xd4 HUD off;
   session+0x2cc player, +0x1cd8/+0x1cdc state; player+0x282 hp, +0x5cd hp colour, +0x5ec bullets,
   +0x56c HP frame, +0x570 label bar, +0x574 labels, +0x57c bullet icon. */
static void hud_draw(uint32_t *stack, uint32_t engine) {
    uint32_t session = guest_read(stack, engine + 0x38), player = guest_read(stack, session + 0x2cc);
    struct hud_surface surface = hud_surface_of(stack, guest_read(stack, engine + 0x1c));
    int screen_w = (int32_t)guest_read(stack, engine + 0x5bc), screen_h = (int32_t)guest_read(stack, engine + 0x5c0);
    int margin = screen_w / 50 > 4 ? screen_w / 50 : 4;
    int hud_off = guest_read_small(stack, engine + 0xd4, 1) != 0;
    int ammo_bottom = margin;

    if (!hud_off) {
        uint32_t frame = guest_read(stack, player + 0x56c);
        int frame_w = frame ? (int32_t)guest_read(stack, frame + 0x10) : 0;
        int frame_h = frame ? (int32_t)guest_read(stack, frame + 0x14) : 0;
        hud_blit(stack, surface, frame, 0, 0, frame_w, frame_h, margin, margin,
                 HUD_HP_SCALE_NUM, HUD_HP_SCALE_DEN, 256);
        int hp = (int16_t)guest_read_small(stack, player + 0x282, 2);
        if (hp > 0) {
            /* Original fill: rows 5..6 of the screen from x = 0x1a, hp/5 px long, with the frame
               drawn at y = 2, i.e. frame rows 3..4 from frame column 26. */
            int fill = hp / 5;
            uint16_t color = guest_read_small(stack, player + 0x5cd, 1) == 0 ? 0xf800u : 0x001fu;
            hud_fill(stack, surface,
                     margin + (26 * HUD_HP_SCALE_NUM + HUD_HP_SCALE_DEN / 2) / HUD_HP_SCALE_DEN,
                     margin + (3 * HUD_HP_SCALE_NUM + HUD_HP_SCALE_DEN / 2) / HUD_HP_SCALE_DEN,
                     margin + ((26 + fill) * HUD_HP_SCALE_NUM + HUD_HP_SCALE_DEN / 2) / HUD_HP_SCALE_DEN,
                     margin + (5 * HUD_HP_SCALE_NUM + HUD_HP_SCALE_DEN / 2) / HUD_HP_SCALE_DEN, color);
        }

        /* Bullet icon plus "*NN" (the original format string is "*%d%d" with n/10 and n%10). */
        uint32_t icon = guest_read(stack, player + 0x57c);
        int icon_h = icon ? (int32_t)guest_read(stack, icon + 0x14) : 0;
        int bullets = (int32_t)guest_read(stack, player + 0x5ec);
        char text[24];
        snprintf(text, sizeof text, "*%d%d", bullets / 10, bullets % 10);
        int block_x = screen_w - margin - (10 + 7 * 3);
        hud_blit(stack, surface, icon, 0, 0, icon ? (int32_t)guest_read(stack, icon + 0x10) : 0, icon_h,
                 block_x, margin, 1, 1, 256);
        hud_digits(stack, surface, guest_read(stack, engine + 0x10), text, block_x + 10, margin + 1);
        ammo_bottom = margin + (icon_h > 9 ? icon_h : 9);
    }

    if (guest_read(stack, session + 0x1cd8) == 0) {
        uint32_t state = guest_read(stack, session + 0x1cdc);
        if (state == 0x12 || state == 0x13 || state == 0x15 || state == 0x17) return;
    }
    if (hud_off) return;

    uint32_t bar = guest_read(stack, player + 0x570), labels = guest_read(stack, player + 0x574);
    int bar_w = bar ? (int32_t)guest_read(stack, bar + 0x10) : 0, bar_h = bar ? (int32_t)guest_read(stack, bar + 0x14) : 0;
    /* Item hint (lower half of the label sheet) moves to the top-right under the bullet count. */
    int hint_bar_y = ammo_bottom + 2;
    hud_blit(stack, surface, bar, 0, 0, bar_w, bar_h, screen_w - 0x20, hint_bar_y, 1, 1, 0x40);
    hud_blit(stack, surface, labels, 0, 0xc, 0x17, 0x18, screen_w - 0x1b, hint_bar_y + 1, 1, 1, 256);
    /* Menu hint (upper half) stays at the bottom-right. */
    hud_blit(stack, surface, bar, 0, 0, bar_w, bar_h, screen_w - 0x20, screen_h - 14, 1, 1, 0x40);
    hud_blit(stack, surface, labels, 0, 0, 0x17, 0xc, screen_w - 0x1b, screen_h - 0xd, 1, 1, 256);
}
