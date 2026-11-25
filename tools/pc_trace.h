/* Debug-only guest entry tracing, compiled in by `recomp_start.py --trace`. It reads guest memory
   and prints; it never changes guest state.

   ISLAND_TRACE_STATS=1       per-scene-frame rasterizer call counts by mode
   ISLAND_TRACE_SCENE=1       one compact line per scene draw: player/camera object fields
   ISLAND_TRACE_FRAME=N       full detail of every rasterizer call, the camera/player fields and the
                              renderer block during the Nth presented guest frame
   ISLAND_TRACE_DIR=dir       with ISLAND_TRACE_FRAME: dir/tris.bin gets one 32-word record per
                              rasterizer call (kind, 4 stack words, renderer, v0 pointer, 3x6 vertex
                              words, 7 spare), dir/proj.bin the records before projection and
                              dir/tex-<B>.bin each texture (u32 W, u32 H, 256 u16 palette, W*H texel bytes).
                              Kind 5 instead uses tex5-<B>-<A2>.bin (u32 W,H, W*H RGB565 u16,
                              W*H weight u16), where A2 is the second stack argument.
                              At geometry return, color.bin = u32 W,H + W*H RGB565 words;
                              depth.bin = u32 W,H,4 + W*H raw signed reciprocal-depth words.
                              kind6-color.bin and kind6-depth.bin use the same headers for the
                              separate kind-6 targets; kind6-alpha.bin = u32 W,H + W*H bytes.
                              All rasters are little-endian, top-down, tightly packed. If a frame
                              draws twice, the planes are from the last draw (tris.bin retains both).
   ISLAND_TRACE_PLAYER=1      player heading/state at every UpdatePlayer call (with frame number) */
#ifndef PC_TRACE_H
#define PC_TRACE_H

static unsigned trace_counts[8];
static int trace_configured, trace_stats, trace_player, trace_scene;
static uint32_t trace_frame;
static FILE *trace_tri_file;
static uint32_t trace_dumped[512];
static unsigned trace_dumped_count;
static struct { uint32_t object, secondary; } trace_dumped_direct[512];
static unsigned trace_dumped_direct_count;

static void trace_configure(void) {
    trace_configured = 1;
    const char *value = getenv("ISLAND_TRACE_STATS");
    trace_stats = value && *value == '1';
    value = getenv("ISLAND_TRACE_FRAME");
    trace_frame = value ? (uint32_t)strtoul(value, NULL, 10) : 0;
    value = getenv("ISLAND_TRACE_PLAYER");
    trace_player = value && *value == '1';
    value = getenv("ISLAND_TRACE_SCENE");
    trace_scene = value && *value == '1';
}

static int trace_detail(void) {
    if (!trace_configured) trace_configure();
    return trace_frame && debug_frame_count + 1 == trace_frame;
}

static float trace_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}


static int host_mapped(uint32_t address) {
    return !(address & 3) && (heap_word(address) || address - 0x8000u < 0x400000u);
}

/* Preserve the legacy indexed trace lookup; kind 5 uses the exact scene binding. */
static uint32_t host_resolve_texture(uint32_t *stack, uint32_t word) {
    for (int level = 0; level < 3 && host_mapped(word); level++) {
        uint32_t width = guest_read(stack, word + 16), height = guest_read(stack, word + 20);
        uint32_t palette = guest_read(stack, word), texels = guest_read(stack, word + 12);
        if (width && height && width <= 4096 && height <= 4096 && !(width & (width - 1)) &&
            !(height & (height - 1)) && host_bytes(palette, 512) && host_bytes(texels, width * height))
            return word;
        word = guest_read(stack, word);
    }
    return 0;
}

static void trace_words(uint32_t *stack, const char *tag, uint32_t address, unsigned count) {
    fprintf(stderr, "TR %s %08x:", tag, address);
    for (unsigned i = 0; i < count; i++) {
        uint32_t word = host_mapped(address + 4 * i) ? guest_read(stack, address + 4 * i) : 0;
        fprintf(stderr, " %08x", word);
    }
    fputc('\n', stderr);
}


static void trace_dump_texture(uint32_t *stack, const char *dir, uint32_t tex) {
    for (unsigned i = 0; i < trace_dumped_count; i++)
        if (trace_dumped[i] == tex) return;
    if (trace_dumped_count < 512) trace_dumped[trace_dumped_count++] = tex;
    uint32_t palette = guest_read(stack, tex), texels = guest_read(stack, tex + 12);
    uint32_t width = guest_read(stack, tex + 16), height = guest_read(stack, tex + 20);
    const unsigned char *pal = host_bytes(palette, 512), *pix = host_bytes(texels, width * height);
    if (!pal || !pix) return;
    char path[512];
    snprintf(path, sizeof path, "%s/tex-%08x.bin", dir, tex);
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fwrite(&width, 4, 1, file);
    fwrite(&height, 4, 1, file);
    fwrite(pal, 1, 512, file);
    fwrite(pix, 1, (size_t)width * height, file);
    fclose(file);
}

static void trace_dump_direct(const char *dir, const vk_scene_texture *tex, uint32_t secondary) {
    for (unsigned i = 0; i < trace_dumped_direct_count; i++)
        if (trace_dumped_direct[i].object == tex->address &&
            trace_dumped_direct[i].secondary == secondary) return;
    char path[512];
    snprintf(path, sizeof path, "%s/tex5-%08x-%08x.bin", dir, tex->address, secondary);
    FILE *file = fopen(path, "wb");
    if (!file) return;
    size_t n = (size_t)tex->width * tex->height;
    fwrite(&tex->width, 4, 1, file);
    fwrite(&tex->height, 4, 1, file);
    fwrite(tex->direct, 2, n, file);
    fwrite(tex->weights, 2, n, file);
    fclose(file);
    if (trace_dumped_direct_count < 512) {
        trace_dumped_direct[trace_dumped_direct_count].object = tex->address;
        trace_dumped_direct[trace_dumped_direct_count++].secondary = secondary;
    }
}

static void trace_dump_triangle(uint32_t *stack, const uint32_t *r, int kind) {
    const char *dir = getenv("ISLAND_TRACE_DIR");
    if (!dir) return;
    if (!trace_tri_file) {
        char path[512];
        snprintf(path, sizeof path, "%s/tris.bin", dir);
        trace_tri_file = fopen(path, "wb");
        if (!trace_tri_file) return;
    }
    uint32_t sp = r[13], record[32] = { (uint32_t)kind };
    for (int i = 0; i < 4; i++) record[1 + i] = guest_read(stack, sp + 4 * i);
    record[5] = r[0];
    record[6] = r[1];
    for (int v = 0; v < 3; v++)
        for (int w = 0; w < 6; w++) record[7 + v * 6 + w] = guest_read(stack, r[1 + v] + 4 * w);
    vk_scene_texture direct = {0};
    if (kind == 5) direct = host_texture_binding(stack, 5, record[1], record[2]);
    uint32_t tex = kind == 5 ? direct.address : host_resolve_texture(stack, record[1]);
    record[25] = tex; /* resolved texture object (0 if none); words 26..31 unused */
    fwrite(record, 4, 32, trace_tri_file);
    fflush(trace_tri_file);
    if (tex) {
        if (kind == 5) trace_dump_direct(dir, &direct, record[2]);
        else trace_dump_texture(stack, dir, tex);
    }
}

/* kind: 0/6 renderer vtable slots, 5/4 FUN_0007f854/FUN_0007d8ac, 1 default FUN_00076ad4. */
static void trace_raster(uint32_t *stack, const uint32_t *r, int kind) {
    if (!trace_configured) trace_configure();
    trace_counts[kind & 7]++;
    if (!trace_detail()) return;
    uint32_t sp = r[13];
    fprintf(stderr, "TR TRI kind=%d renderer=%08x sp-args=%08x %08x %08x %08x\n", kind, r[0],
            guest_read(stack, sp), guest_read(stack, sp + 4), guest_read(stack, sp + 8), guest_read(stack, sp + 12));
    for (int v = 1; v <= 3; v++) trace_words(stack, "  v", r[v], 6);
    uint32_t tex = guest_read(stack, sp);
    if (host_mapped(tex)) {
        trace_words(stack, "  B", tex, 6);
        if (kind != 5) {
            uint32_t palette = guest_read(stack, tex);
            if (host_mapped(palette)) trace_words(stack, "  pal", palette, 4);
            uint32_t texels = guest_read(stack, tex + 12);
            if (host_mapped(texels)) trace_words(stack, "  texels", texels, 4);
        }
    }
    trace_dump_triangle(stack, r, kind);
}

/* ProjectTriangleBatch(renderer, triangles, count): snapshot the records before projection.
   proj.bin: u32 count, scale, cx, cy, triangles base, sort payload pointer, then count*22 record words,
   then count sort-payload words. */
static void trace_project(uint32_t *stack, const uint32_t *r) {
    if (!trace_configured) trace_configure();
    const char *dir = getenv("ISLAND_TRACE_DIR");
    if (!dir || !trace_detail()) return;
    char path[512];
    snprintf(path, sizeof path, "%s/proj.bin", dir);
    FILE *file = fopen(path, "ab");
    if (!file) return;
    uint32_t count = r[2], base = r[1], scene = guest_read(stack, r[0] + 4);
    uint32_t payload = guest_read(stack, scene + 0xf8);
    uint32_t head[8] = { count, guest_read(stack, scene + 0x130), guest_read(stack, r[0] + 0x20),
                         guest_read(stack, r[0] + 0x24), base, payload, scene, r[0] };
    fwrite(head, 4, 8, file);
    for (uint32_t i = 0; i < count * 22 && count < 20000; i++) {
        uint32_t word = guest_read(stack, base + 4 * i);
        fwrite(&word, 4, 1, file);
    }
    for (uint32_t i = 0; i < count && count < 20000; i++) {
        uint32_t word = host_mapped(payload + 4 * i) ? guest_read(stack, payload + 4 * i) : 0;
        fwrite(&word, 4, 1, file);
    }
    fclose(file);
}

static void trace_frame_begin(uint32_t *stack, const uint32_t *r) {
    if (!trace_configured) trace_configure();
    if (trace_stats && (trace_counts[0] | trace_counts[1] | trace_counts[4] | trace_counts[5] | trace_counts[6]))
        fprintf(stderr, "TR STATS t=%llu frame=%u mode0=%u mode6=%u mode5=%u mode4=%u default=%u\n",
                (unsigned long long)(debug_now_ms() - debug_start_ms), debug_frame_count,
                trace_counts[0], trace_counts[6], trace_counts[5], trace_counts[4], trace_counts[1]);
    memset(trace_counts, 0, sizeof trace_counts);
    uint32_t session = r[0];
    uint32_t player = guest_read(stack, session + 0x2cc);
    uint32_t target = guest_read(stack, session + 0x22c);
    uint32_t camera = guest_read(stack, session + 0x12c);
    if (trace_scene)
        fprintf(stderr, "TR SC t=%llu f=%u head=%u pitch=%u roll=%u ppos=(%g,%g,%g) cpos=(%g,%g,%g) cang=(%u,%u,%u) cdir=(%g,%g,%g)\n",
                (unsigned long long)(debug_now_ms() - debug_start_ms), debug_frame_count,
                guest_read(stack, player + 0x278), guest_read(stack, player + 0x274), guest_read(stack, player + 0x27c),
                trace_float(guest_read(stack, player + 0x25c)), trace_float(guest_read(stack, player + 0x260)),
                trace_float(guest_read(stack, player + 0x264)),
                trace_float(guest_read(stack, target + 0x25c)), trace_float(guest_read(stack, target + 0x260)),
                trace_float(guest_read(stack, target + 0x264)),
                guest_read(stack, target + 0x274), guest_read(stack, target + 0x278), guest_read(stack, target + 0x27c),
                trace_float(guest_read(stack, camera + 0x14c)), trace_float(guest_read(stack, camera + 0x150)),
                trace_float(guest_read(stack, camera + 0x154)));
    if (!trace_detail()) return;
    fprintf(stderr, "TR SCENE session=%08x player=%08x\n", session, player);
    trace_words(stack, "player+0x220", player + 0x220, 16);
    trace_words(stack, "player+0x25c", player + 0x25c, 12);
    trace_words(stack, "camera", camera + 0x130, 16);
    trace_words(stack, "camera+0x14c", camera + 0x14c, 4);
    trace_words(stack, "p22c", target + 0x25c, 4);
    trace_words(stack, "p22c-ang", target + 0x270, 8);
    trace_words(stack, "pose2a0", guest_read(stack, session + 0x2a0), 12);
    trace_words(stack, "session+0x130", session + 0x130, 24);
}

static void trace_dump_plane(const char *dir, const char *name, const uint32_t head[3],
                             uint32_t address, uint32_t entry_bytes) {
    size_t head_words = entry_bytes == 4 ? 3 : 2;
    uint32_t size = head[0] * head[1] * entry_bytes;
    const unsigned char *pixels = host_bytes(address, size);
    if (!pixels) {
        fprintf(stderr, "TR %s: unmapped buffer %08x (%u bytes)\n", name, address, size);
        return;
    }
    char path[512];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
        fprintf(stderr, "TR %s: path too long\n", name);
        return;
    }
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); return; }
    int ok = fwrite(head, 4, head_words, file) == head_words && fwrite(pixels, 1, size, file) == size;
    if (fclose(file) != 0 || !ok) fprintf(stderr, "TR %s: incomplete write\n", path);
}

static void trace_frame_end(uint32_t *stack, uint32_t session) {
    if (!trace_detail()) return;
    const char *dir = getenv("ISLAND_TRACE_DIR");
    if (!dir) return;
    /* DrawSceneGeometry copies surface[1] to renderer+0x447ec. At 0x70f18..0x70f40 this
       and renderer+0x34 supply the rasterizer's color and depth arguments (sp+4,sp+8).
       Both are cleared at scene dimensions, and the rasterizers index y*width+x. */
    uint32_t renderer = guest_read(stack, session + 0x130), scene = guest_read(stack, renderer + 4);
    uint32_t w = guest_read(stack, scene + 0xd8), h = guest_read(stack, scene + 0xdc);
    uint32_t color = guest_read(stack, renderer + 0x447ec), depth = guest_read(stack, renderer + 0x34);
    if (!w || !h || (uint64_t)w * h > UINT32_MAX / 4) {
        fprintf(stderr, "TR invalid geometry dimensions %ux%u\n", w, h);
        return;
    }
    uint32_t head[3] = {w, h, 4};
    trace_dump_plane(dir, "color.bin", head, color, 2);
    trace_dump_plane(dir, "depth.bin", head, depth, 4);
    /* 0x70fbc..0x70fe0 passes the kind-6 rasterizer its own color/depth pair;
       that rasterizer also writes its per-pixel byte argument to the plane at +0x40. */
    trace_dump_plane(dir, "kind6-color.bin", head, guest_read(stack, renderer + 0x38), 2);
    trace_dump_plane(dir, "kind6-depth.bin", head, guest_read(stack, renderer + 0x3c), 4);
    trace_dump_plane(dir, "kind6-alpha.bin", head, guest_read(stack, renderer + 0x40), 1);
    fprintf(stderr, "TR GEOMETRY frame=%u %ux%u color=%08x depth=%08x\n", trace_frame, w, h, color, depth);
}

static void trace_player_tick(uint32_t *stack, const uint32_t *r) {
    if (!trace_configured) trace_configure();
    if (!trace_player) return;
    uint32_t player = guest_read(stack, r[0] + 0x2cc);
    fprintf(stderr, "TR PLAYER t=%llu frame=%u state=%04x heading=%u pitch=%u acc=%d pos=(%g,%g,%g)\n",
            (unsigned long long)(debug_now_ms() - debug_start_ms), debug_frame_count,
            guest_read(stack, player + 0x22c) >> 16 & 0xffff, guest_read(stack, player + 0x278),
            guest_read(stack, player + 0x274), (int32_t)guest_read(stack, player + 0x2c8),
            trace_float(guest_read(stack, player + 0x25c)), trace_float(guest_read(stack, player + 0x260)),
            trace_float(guest_read(stack, player + 0x264)));
}

#endif
