/* Test harness for headless smoke runs (Xvfb + lavapipe). Nothing here runs unless an environment
   variable asks for it, and it never changes game state except by queuing the same key transitions
   a keyboard would.

   ISLAND_AUTOKEYS="f120:0xa7,t4000:0x10:hold1500,..." queues keys: fN = after N presented guest
   frames, tN = after N ms of wall time; the scan code is a Symbian scancode (0xa7 OK, 0xa5 back,
   0xa4 items, 0x0e left, 0x0f right, 0x10 up, 0x11 down); ":holdM" keeps the key down M ms instead of
   a tap.
   ISLAND_DUMP_DIR=dir ISLAND_DUMP_FRAMES="100,200" writes dir/guest-<frame>.png of the guest
   framebuffer (the 2D/3D image exactly as the guest produced it) when that many frames were presented;
   ISLAND_DUMP_EVERY=N dumps every Nth frame. */
#ifndef PC_DEBUG_H
#define PC_DEBUG_H

#include <zlib.h>

enum { debug_max_keys = 64, debug_max_dumps = 64 };

static struct debug_key {
    uint32_t scan;
    unsigned by_time;
    uint64_t when;
    uint64_t hold_ms;
    int state; /* 0 pending, 1 down sent (hold), 2 done */
    uint64_t release_at;
} debug_keys[debug_max_keys];
static unsigned debug_key_count;
static uint32_t debug_dump_frames[debug_max_dumps];
static uint64_t debug_dump_times[debug_max_dumps];
static unsigned debug_dump_count, debug_dump_time_count, debug_dump_every;
static const char *debug_dump_dir;
static uint32_t debug_frame_count;
static int debug_configured;
static uint64_t debug_start_ms;

static uint64_t debug_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void debug_configure(void) {
    debug_configured = 1;
    debug_start_ms = debug_now_ms();
    const char *keys = getenv("ISLAND_AUTOKEYS");
    while (keys && *keys && debug_key_count < debug_max_keys) {
        struct debug_key key = {0};
        char *end;
        if (*keys == 't') key.by_time = 1;
        else if (*keys != 'f') break;
        key.when = strtoull(keys + 1, &end, 10);
        if (*end != ':') break;
        key.scan = (uint32_t)strtoul(end + 1, &end, 0);
        if (*end == ':' && !strncmp(end + 1, "hold", 4)) key.hold_ms = strtoull(end + 5, &end, 10);
        debug_keys[debug_key_count++] = key;
        if (*end != ',') break;
        keys = end + 1;
    }
    const char *frames = getenv("ISLAND_DUMP_FRAMES");
    while (frames && *frames && debug_dump_count < debug_max_dumps) {
        char *end;
        debug_dump_frames[debug_dump_count++] = (uint32_t)strtoul(frames, &end, 10);
        if (*end != ',') break;
        frames = end + 1;
    }
    const char *times = getenv("ISLAND_DUMP_MS");
    while (times && *times && debug_dump_time_count < debug_max_dumps) {
        char *end;
        debug_dump_times[debug_dump_time_count++] = strtoull(times, &end, 10);
        if (*end != ',') break;
        times = end + 1;
    }
    const char *every = getenv("ISLAND_DUMP_EVERY");
    if (every) debug_dump_every = (unsigned)strtoul(every, NULL, 10);
    debug_dump_dir = getenv("ISLAND_DUMP_DIR");
    if (debug_key_count || debug_dump_count || debug_dump_every)
        fprintf(stderr, "debug: %u autokeys, %u dump frames, dump every %u\n", debug_key_count,
                debug_dump_count, debug_dump_every);
}

/* Returns 1 and fills scan/type when an autokey transition is due. */
static int debug_next_key(uint32_t *scan, uint32_t *type, int *tap) {
    if (!debug_configured) debug_configure();
    uint64_t now = debug_now_ms() - debug_start_ms;
    for (unsigned i = 0; i < debug_key_count; i++) {
        struct debug_key *key = &debug_keys[i];
        if (key->state == 2) continue;
        if (key->state == 1) {
            if (now < key->release_at) continue;
            key->state = 2;
            *scan = key->scan; *type = 2; *tap = 0;
            return 1;
        }
        if ((key->by_time ? now : debug_frame_count) < key->when) continue;
        *scan = key->scan;
        if (key->hold_ms) {
            key->state = 1;
            key->release_at = now + key->hold_ms;
            *type = 3; *tap = 0;
        } else {
            key->state = 2;
            *type = 3; *tap = 1;
        }
        return 1;
    }
    return 0;
}

/* ISLAND_AUTOMOUSE="t40000:-300,t42000:300" adds that many relative mouse counts (x) at tN ms.
   Returns 1 and fills dx when one is due. */
static int debug_next_mouse(int32_t *dx) {
    static struct { uint64_t when; int32_t dx; int done; } moves[16];
    static unsigned count;
    static int parsed;
    if (!debug_configured) debug_configure();
    if (!parsed) {
        parsed = 1;
        const char *text = getenv("ISLAND_AUTOMOUSE");
        while (text && *text && count < 16) {
            char *end;
            if (*text != 't') break;
            moves[count].when = strtoull(text + 1, &end, 10);
            if (*end != ':') break;
            moves[count++].dx = (int32_t)strtol(end + 1, &end, 10);
            if (*end != ',') break;
            text = end + 1;
        }
    }
    uint64_t now = debug_now_ms() - debug_start_ms;
    for (unsigned i = 0; i < count; i++)
        if (!moves[i].done && now >= moves[i].when) {
            moves[i].done = 1;
            *dx = moves[i].dx;
            return 1;
        }
    return 0;
}

static void debug_png_chunk(FILE *file, const char *tag, const unsigned char *data, uint32_t size) {
    unsigned char head[8] = { size >> 24, size >> 16, size >> 8, size, tag[0], tag[1], tag[2], tag[3] };
    fwrite(head, 1, 8, file);
    if (size) fwrite(data, 1, size, file);
    uLong crc = crc32(0, (const Bytef *)tag, 4);
    if (size) crc = crc32(crc, data, size);
    unsigned char tail[4] = { crc >> 24, crc >> 16, crc >> 8, crc };
    fwrite(tail, 1, 4, file);
}

/* rgb is w*h*3 bytes, row 0 first. */
static void debug_write_png(const char *path, unsigned w, unsigned h, const unsigned char *rgb) {
    size_t stride = (size_t)w * 3 + 1;
    size_t raw_size = stride * h;
    unsigned char *raw = malloc(raw_size);
    uLongf packed_size = compressBound((uLong)raw_size);
    unsigned char *packed = malloc(packed_size);
    FILE *file = fopen(path, "wb");
    if (!raw || !packed || !file) { free(raw); free(packed); if (file) fclose(file); return; }
    for (unsigned y = 0; y < h; y++) {
        raw[y * stride] = 0;
        memcpy(raw + y * stride + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    compress2(packed, &packed_size, raw, (uLong)raw_size, 6);
    unsigned char ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, file);
    debug_png_chunk(file, "IHDR", ihdr, 13);
    debug_png_chunk(file, "IDAT", packed, (uint32_t)packed_size);
    debug_png_chunk(file, "IEND", NULL, 0);
    fclose(file);
    free(raw);
    free(packed);
    fprintf(stderr, "debug: dumped %s\n", path);
}

/* guest-NNNNN.png: the guest RGB565 surface. present-NNNNN.png: the RGBA8 image uploaded to the
   presentation texture (window size times present_scale), i.e. what the window shows before scaling. */
static void debug_dump_png(uint32_t frame) {
    if (!debug_dump_dir) return;
    char path[512];
    size_t count = (size_t)window_width * window_height;
    unsigned char *rgb = malloc(count * 3);
    if (!rgb) return;
    for (size_t i = 0; i < count; i++) {
        uint16_t p = pixels[i];
        rgb[i * 3] = (unsigned char)(((p >> 11) & 31) * 255 / 31);
        rgb[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63);
        rgb[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31);
    }
    snprintf(path, sizeof path, "%s/guest-%05u.png", debug_dump_dir, frame);
    debug_write_png(path, (unsigned)window_width, (unsigned)window_height, rgb);
    free(rgb);
    if (!vk_present.staging_data) return;
    unsigned w = (unsigned)window_width * present_scale, h = (unsigned)window_height * present_scale;
    rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    const unsigned char *rgba = vk_present.staging_data;
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) memcpy(rgb + i * 3, rgba + i * 4, 3);
    snprintf(path, sizeof path, "%s/present-%05u.png", debug_dump_dir, frame);
    debug_write_png(path, w, h, rgb);
    free(rgb);
}

/* Called once per presented guest frame. */
static void debug_frame_presented(void) {
    if (!debug_configured) debug_configure();
    debug_frame_count++;
    int dump = debug_dump_every && debug_frame_count % debug_dump_every == 0;
    for (unsigned i = 0; i < debug_dump_count && !dump; i++) dump = debug_dump_frames[i] == debug_frame_count;
    uint64_t now = debug_now_ms() - debug_start_ms;
    for (unsigned i = 0; i < debug_dump_time_count; i++)
        if (debug_dump_times[i] && now >= debug_dump_times[i]) { debug_dump_times[i] = 0; dump = 1; }
    if (dump) debug_dump_png(debug_frame_count);
}

#endif
