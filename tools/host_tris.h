/* Host-side capture of the guest's per-frame triangle submission, and the Vulkan scene pass built on it.
   Normally the guest rasterizes everything itself; the entry hooks capture the arguments so the
   host renderer consumes the same submission set and order (docs/pc-rendering-design.md, invariant 2).

   Frame lifecycle: DrawSceneGeometry entry (0x63194) clears the list; every rasterizer entry appends; the
   return site in GameSceneCompositeFrame (0x68ebc) freezes it as host_tris_frame. DrawSceneGeometry runs
   more than once per presented frame, so a "frame" here is one geometry call.

   ISLAND_HOST_TRIS=1 enables capture (default off). ISLAND_HOST_TRIS_LOG=N prints per-kind counts every N
   geometry calls (default 60). ISLAND_HOST_TRIS_DUMP=<frame>:<path> writes that presented frame as raw
   records (struct host_tri) for comparison with the --trace tris.bin; <frame> uses the ISLAND_TRACE_FRAME
   numbering (presented frames + 1).

   ISLAND_VK_SCENE=1 (implies capture) renders kinds 0/1/4/5/6 with tools/vk_scene.h on the presentation
   device, at ISLAND_VK_SCENE_SCALE (1..8, default 1) times the guest scene size. ISLAND_VK_SCENE_CHECK=1
   compares the scale-1 result with the guest colour buffer at the same moment and logs the match counts.
   ISLAND_VK_PROJECT=1 opts into redoing the guest's projection on the GPU for verified compositor
   submissions; transform and clipping remain guest-side. Unmatched triangles keep screen-space XY.
   ISLAND_VK_MESH_CHECK=1 independently compares the first eligible pre-clip mesh per presented frame
   against Vulkan (up to eight samples per run). The guest transform and its output stay authoritative.
   ISLAND_VK_SKIP_RASTER=1 (requires ISLAND_VK_SCENE=1) can commit a complete forward batch's Vulkan
   output to the native render planes before the guest rasterizers run. Only their pixel loops are
   bypassed; the caller and rasterizer prefixes still execute. ISLAND_VK_PLAN_CHECK=1 independently
   compares planned calls against the actual entries, including entries whose pixel loops were bypassed.
   With the scene pass on, the presentation texture becomes scale times the guest size and the window
   shows the host-rendered 3D wherever the guest's final pixel is still the 3D pixel (see
   host_scene_compose); every other pixel is the guest's own, replicated. */
#ifndef HOST_TRIS_H
#define HOST_TRIS_H

#define HOST_TRIS_MAX 8192
#define HOST_SCENE_TEXTURES 512

#include <time.h>
/* ISLAND_VK_PROFILE=1: mean milliseconds per frame for each host scene stage, printed every 60 frames. */
static struct { double render, alpha, compose, commit, last; unsigned frames; int on; } host_prof;
static double host_prof_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}
static void host_prof_frame(void) {
    if (!host_prof.on) return;
    double now = host_prof_now();
    if (++host_prof.frames < 60) return;
    double wall = host_prof.last ? now - host_prof.last : 0, n = host_prof.frames;
    fprintf(stderr, "vk-profile: 60 frames wall=%.2f ms/frame (%.1f fps) vk_render=%.2f alpha=%.2f commit=%.2f compose=%.2f\n",
            wall / n, wall ? 1e3 * n / wall : 0, host_prof.render / n, host_prof.alpha / n, host_prof.commit / n,
            host_prof.compose / n);
    host_prof.render = host_prof.alpha = host_prof.commit = host_prof.compose = 0;
    host_prof.frames = 0; host_prof.last = now;
}
#define HOST_PROJECT_RECORD_STRIDE 0x58u
#define HOST_PROJECT_VERTEX_STRIDE 0x18u
#define HOST_PROJECT_FLAG 0x49u
#define HOST_PROJECT_TABLE 0x44u
#define HOST_PROJECT_TABLE_COUNT 70000u /* renderer initialization fills indices 0..69999 */
#define HOST_PRIMARY_COLOUR 0x447ecu
#define HOST_MESH_MAX 4096u
#define HOST_MESH_SAMPLES 8u

static const unsigned char *host_bytes(uint32_t address, uint32_t size) {
    uint32_t offset = address - heap_base;
    return address >= heap_base && (!size || size - 1 <= UINT32_MAX - address) &&
        size <= thread_heap.used && offset <= thread_heap.used - size
        ? thread_heap.backing + offset : NULL;
}

static int host_texture_words(uint32_t address, uint32_t size) {
    return !(address & 3) && host_bytes(address, size) != NULL;
}

/* pc_0007d928..944 and pc_0007f8d0..8f0: two offset-zero loads after reading
   the first stack arg reach the object. Kind 5 uses object+4 (RGB565) and
   second stack arg+4 (u16 weights); kind 4 uses object+0/+12 (palette/indices). */
static vk_scene_texture host_texture_binding(uint32_t *stack, uint32_t kind,
                                             uint32_t arg, uint32_t secondary) {
    uint32_t object = arg;
    if (kind == 4 || kind == 5) {
        for (int i = 0; i < 2; i++) {
            if (!host_texture_words(object, 4)) return (vk_scene_texture){0};
            object = guest_read(stack, object);
        }
    }
    if (!object || !host_texture_words(object, 24)) return (vk_scene_texture){0};
    uint32_t w = guest_read(stack, object + 0x10), h = guest_read(stack, object + 0x14);
    if (!w || !h || w > 4096 || h > 4096 || (w & (w - 1)) || (h & (h - 1)))
        return (vk_scene_texture){0};
    uint32_t n = w * h;
    vk_scene_texture tex = { .address = object, .width = w, .height = h };
    if (kind == 5) {
        if (!host_texture_words(secondary, 8)) return (vk_scene_texture){0};
        uint32_t direct = guest_read(stack, object + 4), weights = guest_read(stack, secondary + 4);
        if ((direct | weights) & 1) return (vk_scene_texture){0};
        tex.direct = (const uint16_t *)host_bytes(direct, n * 2);
        tex.weights = (const uint16_t *)host_bytes(weights, n * 2);
        if (!tex.direct || !tex.weights) return (vk_scene_texture){0};
    } else {
        uint32_t palette = guest_read(stack, object);
        if (palette & 1) return (vk_scene_texture){0};
        tex.palette = (const uint16_t *)host_bytes(palette, 512);
        tex.texels = host_bytes(guest_read(stack, object + 12), n);
        if (!tex.palette || !tex.texels) return (vk_scene_texture){0};
    }
    return tex;
}

/* kind: 0/6 renderer vtable slots 0x83fa8/0x85bb0, 4 FUN_0007d8ac, 5 FUN_0007f854,
   1 default FUN_00076ad4. stack[0] is the object or wrapper (4/5). */
struct host_tri {
    uint32_t kind, stack[4], renderer, vertex_base;
    uint32_t v[3][6]; /* x, y, idx, u, v, w5 per vertex, as the rasterizer reads them */
};

static struct {
    int configured, enabled, scene_enabled, scene_check, project_enabled;
    unsigned log_every, dump_frame, scene_scale;
    int plan_check;
    int skip_raster;
    const char *dump_path;
    struct host_tri live[HOST_TRIS_MAX];
    unsigned live_count, dropped;
    uint32_t composite_session;
} host_tris;

static struct {
    int enabled;
    unsigned samples, vertices, mismatched_vertices, mismatched_words;
    uint32_t frame;
    vk_scene scene; /* private 1x1 context when the Vulkan raster scene is not active */
    uint32_t xyz[HOST_MESH_MAX][3];
} host_mesh;

static struct { struct host_tri *tris; unsigned count, serial; } host_tris_frame;
static struct host_tri host_tris_frozen[HOST_TRIS_MAX];

static struct {
    struct host_tri tris[HOST_TRIS_MAX];
    vk_scene_texture textures[HOST_SCENE_TEXTURES];
    unsigned texture_index[HOST_TRIS_MAX], texture_count;
    const char *unavailable;
    unsigned count, bad_vertex_args;
    uint32_t bad_record;
    int entered;
} host_tris_plan;

static struct {
    int committed, forward_entry;
    unsigned bypassed, expected;
    uint64_t batches, loops;
    const char *refusal;
} host_raster;
static int host_tris_plan_tri_equal(const struct host_tri *a, const struct host_tri *b);

/* Address lookup is local to the most recent ProjectTriangleBatch. Submission metadata
   is copied out separately so reusing the batch buffer can't invalidate earlier draws. */
static struct {
    uint32_t renderer, base, count, center[2], colour[2], depth[2];
    struct {
        uint32_t xy_inv[3][3], idx[3];
        int valid;
    } records[HOST_TRIS_MAX];
} host_project_batch;
static vk_scene_projection host_project_live[HOST_TRIS_MAX];
static unsigned host_project_matched;

/* ARM arithmetic shift, expressed entirely as 32-bit unsigned bit operations. */
static uint32_t host_project_asr(uint32_t bits, unsigned shift) {
    return (bits >> shift) | ((0u - (bits >> 31)) << (32 - shift));
}

static void host_project_match(const struct host_tri *t, const uint32_t *r, vk_scene_projection *out) {
    out->valid = 0;
    if (!host_project_batch.count || r[0] != host_project_batch.renderer || r[1] < host_project_batch.base)
        return;
    uint32_t offset = r[1] - host_project_batch.base;
    if (offset % HOST_PROJECT_RECORD_STRIDE) return;
    uint32_t i = offset / HOST_PROJECT_RECORD_STRIDE;
    if (i >= host_project_batch.count || !host_project_batch.records[i].valid ||
        r[2] != r[1] + HOST_PROJECT_VERTEX_STRIDE || r[3] != r[1] + 2 * HOST_PROJECT_VERTEX_STRIDE) return;
    unsigned colour_arg = t->kind == 5 ? 2 : 1, layer = t->kind == 6;
    if (t->stack[colour_arg] != host_project_batch.colour[layer] ||
        t->stack[colour_arg + 1] != host_project_batch.depth[layer]) return;
    const unsigned char *flag = host_bytes(r[1] + HOST_PROJECT_FLAG, 1);
    if (!flag || *flag != 0) return;
    for (unsigned v = 0; v < 3; v++) {
        const uint32_t *raw = host_project_batch.records[i].xy_inv[v];
        if (host_project_batch.records[i].idx[v] != t->v[v][2]) return;
        for (unsigned axis = 0; axis < 2; axis++) {
            uint32_t projected = host_project_batch.center[axis] + host_project_asr(raw[2] * raw[axis], 16);
            if (projected != t->v[v][axis]) return;
        }
    }
    /* The scene ABI uses signed words, but it needs the original ARM bit patterns. */
    memcpy(out->xy_inv, host_project_batch.records[i].xy_inv, sizeof out->xy_inv);
    memcpy(out->center, host_project_batch.center, sizeof out->center);
    out->valid = 1;
}

static void host_tris_configure(void) {
    host_tris.configured = 1;
    const char *text = getenv("ISLAND_HOST_TRIS");
    host_tris.enabled = text && *text == '1';
    text = getenv("ISLAND_VK_SCENE");
    host_tris.scene_enabled = text && *text == '1';
    host_tris.enabled |= host_tris.scene_enabled;
    text = getenv("ISLAND_VK_PROJECT");
    host_tris.project_enabled = host_tris.scene_enabled && text && strcmp(text, "1") == 0;
    text = getenv("ISLAND_VK_MESH_CHECK");
    host_mesh.enabled = text && strcmp(text, "1") == 0;
    host_prof.on = (text = getenv("ISLAND_VK_PROFILE")) && strcmp(text, "1") == 0;
    text = getenv("ISLAND_VK_SKIP_RASTER");
    host_tris.skip_raster = host_tris.scene_enabled && text && strcmp(text, "1") == 0;
    text = getenv("ISLAND_VK_PLAN_CHECK");
    host_tris.plan_check = text && strcmp(text, "1") == 0;
    host_tris.enabled |= host_tris.plan_check;
    text = getenv("ISLAND_VK_SCENE_CHECK");
    host_tris.scene_check = text && *text == '1';
    host_tris.scene_scale = 1;
    if ((text = getenv("ISLAND_VK_SCENE_SCALE"))) {
        unsigned long scale = strtoul(text, NULL, 10);
        if (scale >= 1 && scale <= 8) host_tris.scene_scale = (unsigned)scale;
        else fprintf(stderr, "vk-scene: ISLAND_VK_SCENE_SCALE=%s is invalid (want 1..8), using 1\n", text);
    }
    host_tris.log_every = 60;
    host_tris.dump_frame = UINT32_MAX;
    if ((text = getenv("ISLAND_HOST_TRIS_LOG")) && strtoul(text, NULL, 10)) host_tris.log_every = (unsigned)strtoul(text, NULL, 10);
    if ((text = getenv("ISLAND_HOST_TRIS_DUMP"))) {
        char *end = NULL;
        unsigned long frame = strtoul(text, &end, 10);
        if (end && *end == ':' && end[1]) { host_tris.dump_frame = (unsigned)frame; host_tris.dump_path = end + 1; }
        else fprintf(stderr, "host-tris: ISLAND_HOST_TRIS_DUMP=%s is invalid (want <frame>:<path>)\n", text);
    }
}

static void host_tris_push(uint32_t *stack, const uint32_t *r, uint32_t kind) {
    if (!host_tris.configured) host_tris_configure();
    if (!host_tris.enabled) return;
    if (host_tris.live_count >= HOST_TRIS_MAX) {
        host_tris.dropped++;
        host_raster.committed = 0;
        host_raster.refusal = "raster capture limit exceeded";
        return;
    }
    struct host_tri *t = &host_tris.live[host_tris.live_count++];
    t->kind = kind;
    for (int i = 0; i < 4; i++) t->stack[i] = guest_read(stack, r[13] + 4 * i);
    t->renderer = r[0];
    t->vertex_base = r[1];
    if ((host_tris.plan_check || host_tris.skip_raster) && host_tris_plan.bad_vertex_args == UINT32_MAX &&
        (r[2] != r[1] + HOST_PROJECT_VERTEX_STRIDE || r[3] != r[1] + 2 * HOST_PROJECT_VERTEX_STRIDE))
        host_tris_plan.bad_vertex_args = host_tris.live_count - 1;
    for (int v = 0; v < 3; v++)
        for (int w = 0; w < 6; w++) t->v[v][w] = guest_read(stack, r[1 + v] + 4 * w);
    if (host_tris.project_enabled) {
        vk_scene_projection *projection = &host_project_live[host_tris.live_count - 1];
        host_project_match(t, r, projection);
    }
    if (host_raster.committed) {
        if (host_raster.expected >= host_tris_plan.count ||
            !host_tris_plan_tri_equal(t, &host_tris_plan.tris[host_raster.expected])) {
            host_raster.committed = 0;
            host_raster.forward_entry = 0;
            host_raster.refusal = "captured raster differs from committed plan";
        } else {
            host_raster.expected++;
            host_raster.forward_entry = (kind == 0 && r[14] == 0x70f4c) ||
                (kind == 1 && r[14] == 0x70eb8) || (kind == 6 && r[14] == 0x70ff4);
        }
    }
}

static int host_scene_skip_raster(void) {
    if (!host_raster.committed || !host_raster.forward_entry) return 0;
    host_raster.bypassed++;
    host_raster.loops++;
    host_raster.forward_entry = 0;
    return 1;
}

/* Vulkan scene pass. Runs on the presentation device, so it waits until the first publish created it. */
struct host_scene_target {
    uint32_t renderer, viewport, surface, bitmap, width, height, colour, depth, colour6, depth6, alpha;
};

static struct {
    vk_scene scene;
    int created, failed, render_valid, baseline_valid, blit_valid;
    uint32_t width, height, session;
    unsigned serial;
    unsigned flash;
    struct host_scene_target target;
    vk_scene_layers layers;
    uint16_t *c0;   /* guest RGB565 immediately after the guest alpha pass, before the HUD */
    uint16_t *blit; /* pixels copied by the matching window BitBlt */
    uint8_t *hud_mask; /* one byte per guest pixel; only HUD writes are marked explicitly */
} host_scene;

static void host_scene_invalidate(void) {
    host_scene.render_valid = host_scene.baseline_valid = host_scene.blit_valid = 0;
}

static uint8_t *host_scene_hud_mask(uint32_t pixels, unsigned width, unsigned height) {
    const struct host_scene_target *t = &host_scene.target;
    return host_scene.baseline_valid && pixels == t->colour && width == t->width && height == t->height
        ? host_scene.hud_mask : NULL;
}

static int host_scene_has_blit(void) {
    return host_scene.blit_valid;
}

static void host_tris_begin(const uint32_t *r) {
    if (!host_tris.configured) host_tris_configure();
    host_scene_invalidate();
    host_tris.live_count = 0;
    host_tris.dropped = 0;
    host_tris.composite_session = r[14] == 0x68ebc ? r[0] : 0;
    host_project_batch.count = 0;
    host_project_matched = 0;
    if (host_tris.skip_raster) {
        host_raster.committed = host_raster.forward_entry = 0;
        host_raster.bypassed = host_raster.expected = 0;
        host_raster.refusal = NULL;
    }
    if (host_tris.plan_check || host_tris.skip_raster) {
        host_tris_plan.entered = 0;
        host_tris_plan.unavailable = "forward dispatch not reached";
        host_tris_plan.count = 0;
        host_tris_plan.texture_count = 0;
        host_tris_plan.bad_vertex_args = UINT32_MAX;
    }
}

static unsigned host_scene_scale(void) {
    if (!host_tris.configured) host_tris_configure();
    return host_tris.scene_enabled ? host_tris.scene_scale : 1;
}

static void host_scene_release(void) {
    vk_scene_destroy(&host_scene.scene);
    free(host_scene.c0); free(host_scene.blit);
    free(host_scene.hud_mask);
    host_scene.c0 = host_scene.blit = NULL;
    host_scene.hud_mask = NULL;
    host_scene.created = 0;
    host_scene_invalidate();
}

/* Other DrawSceneGeometry callers can use a scratch surface. Only the compositor's primary
   surface and its own auxiliary targets qualify for replacing a window frame. */
static int host_scene_target(uint32_t *stack, uint32_t session, struct host_scene_target *t) {
    if (!host_texture_words(session, 0x2a4)) return 0;
    t->renderer = guest_read(stack, session + 0x130);
    t->surface = guest_read(stack, session + 0x2a0);
    t->bitmap = guest_read(stack, session + 0x10);
    if (!t->bitmap || !host_texture_words(t->renderer, 0x447f0) || !host_texture_words(t->surface, 8)) return 0;
    t->viewport = guest_read(stack, t->renderer + 4);
    if (t->viewport != guest_read(stack, session + 0x12c) || !host_texture_words(t->viewport, 0xe0)) return 0;
    t->width = guest_read(stack, t->viewport + 0xd8);
    t->height = guest_read(stack, t->viewport + 0xdc);
    if (!t->width || !t->height || t->width > 2048 || t->height > 2048) return 0;
    t->colour = guest_read(stack, t->surface + 4);
    t->depth = guest_read(stack, t->renderer + 0x34);
    t->colour6 = guest_read(stack, t->renderer + 0x38);
    t->depth6 = guest_read(stack, t->renderer + 0x3c);
    t->alpha = guest_read(stack, t->renderer + 0x40);
    uint32_t n = t->width * t->height;
    return t->colour == guest_read(stack, t->renderer + HOST_PRIMARY_COLOUR) &&
        !((t->colour | t->colour6) & 1) && !((t->depth | t->depth6) & 3) &&
        host_bytes(t->colour, n * 2) && host_bytes(t->depth, n * 4) &&
        host_bytes(t->colour6, n * 2) && host_bytes(t->depth6, n * 4) && host_bytes(t->alpha, n);
}

static void host_mesh_release(void) {
    vk_scene_destroy(&host_mesh.scene);
}

static int host_mesh_overlap(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
    return a < (uint64_t)b + b_size && b < (uint64_t)a + a_size;
}

/* 0x8a250 is reached after the final XYZ store at 0x8a23c; the <=0 path
   goes directly to 0x8a254. r8 is the mesh, r7 the matrix, r9 the output
   base, r10 the shift, r1/r2 the finished loop count and mesh count.
   r5/r6 are the last XYZ byte offset and destination. 0x8a250 itself
   only stores the matrix cursor; clipping runs after the call returns. */
static void host_tris_mesh_check(uint32_t *stack, const uint32_t *r) {
    uint32_t session = host_tris.composite_session;
    if (!host_mesh.enabled || !session || !vk_present.device || host_mesh.samples >= HOST_MESH_SAMPLES ||
        (host_mesh.samples && host_mesh.frame == debug_frame_count)) return;

    uint32_t mesh = r[8], count = r[2], shift = r[10];
    if (!count || count > HOST_MESH_MAX || count != r[1] || shift > 31 ||
        !host_texture_words(mesh, 0x150)) return;
    uint32_t rows_addr = guest_read(stack, mesh + 0x134), input_addr = guest_read(stack, mesh + 0x138);
    uint32_t output_addr = guest_read(stack, mesh + 0x13c), last = 12 * (count - 1);
    if (rows_addr != r[7] || output_addr != r[9] || r[5] != last || r[6] - output_addr != last ||
        count != guest_read(stack, mesh + 0x14c) || shift != guest_read_small(stack, mesh + 0x7c, 1) ||
        ((rows_addr | output_addr) & 3) || (input_addr & 1)) return;
    /* The emitter can receive a matrix from the renderer's stack frame. */
    int rows_mapped = rows_addr - (uint32_t)stack_base <= stack_size - 48 || host_bytes(rows_addr, 48);
    const unsigned char *input = host_bytes(input_addr, 6 * count);
    const unsigned char *output = host_bytes(output_addr, 12 * count);
    if (!rows_mapped || !input || !output ||
        host_mesh_overlap(output_addr, 12 * count, rows_addr, 48) ||
        host_mesh_overlap(output_addr, 12 * count, input_addr, 6 * count) ||
        host_mesh_overlap(output_addr, 12 * count, mesh, 0x150) ||
        host_mesh_overlap(mesh + 0x90, 12, rows_addr, 48) ||
        host_mesh_overlap(mesh + 0x90, 12, input_addr, 6 * count)) return;

    struct host_scene_target target;
    if (!host_texture_words(session, 0x1ce0) || guest_read(stack, session + 0x1cdc) == 0x1a ||
        guest_read(stack, session + 0x7fc) != 0 || !host_scene_target(stack, session, &target) ||
        guest_read(stack, mesh + 0xa0) != target.viewport) return;
    const unsigned char *player = host_bytes(guest_read(stack, session + 0x2cc), 0x5ec);
    if (!player || player[0x5eb]) return;

    vk_scene *scene = host_scene.created ? &host_scene.scene : &host_mesh.scene;
    if (!scene->device) {
        if (!vk_scene_create(scene, vk_present.physical, vk_present.device, vk_present.queue,
                             vk_present.queue_family, 1, 1, 1) || atexit(host_mesh_release)) {
            host_mesh_release();
            fputs("vk-mesh: cannot initialize parity check; disabled\n", stderr);
            host_mesh.enabled = 0;
            return;
        }
    }
    uint32_t rows[3][4];
    for (unsigned y = 0; y < 3; y++)
        for (unsigned x = 0; x < 4; x++) rows[y][x] = guest_read(stack, rows_addr + 4 * (4 * y + x));
    for (uint32_t i = 0; i < count; i++)
        for (unsigned axis = 0; axis < 3; axis++) {
            uint32_t word = le16(input + 6 * i + 2 * axis);
            host_mesh.xyz[i][axis] = word | ((0u - (word >> 15)) << 16);
        }
    if (!vk_scene_transform_mesh(scene, rows, host_mesh.xyz, host_mesh.xyz, count, shift)) {
        fputs("vk-mesh: compute unavailable or dispatch failed; disabled\n", stderr);
        host_mesh.enabled = 0;
        host_mesh_release();
        return;
    }

    unsigned bad_vertices = 0, bad_words = 0, first_axis = 0;
    uint32_t first_vertex = 0, first_guest = 0;
    for (uint32_t i = 0; i < count; i++) {
        unsigned bad = 0;
        for (unsigned axis = 0; axis < 3; axis++) {
            uint32_t guest = le32(output + 12 * i + 4 * axis);
            if (guest == host_mesh.xyz[i][axis]) continue;
            if (!bad_words) { first_vertex = i; first_axis = axis; first_guest = guest; }
            bad_words++;
            bad = 1;
        }
        bad_vertices += bad;
    }
    host_mesh.frame = debug_frame_count;
    host_mesh.samples++;
    host_mesh.vertices += count;
    host_mesh.mismatched_vertices += bad_vertices;
    host_mesh.mismatched_words += bad_words;
    fprintf(stderr, "vk-mesh: frame=%" PRIu32 " sample=%u/%u shift=%" PRIu32 " vertices=%" PRIu32
            " mismatched_vertices=%u mismatched_words=%u total_vertices=%u total_mismatched_vertices=%u"
            " total_mismatched_words=%u", host_mesh.frame + 1, host_mesh.samples, HOST_MESH_SAMPLES,
            shift, count, bad_vertices, bad_words, host_mesh.vertices, host_mesh.mismatched_vertices,
            host_mesh.mismatched_words);
    if (bad_words) fprintf(stderr, " first_vertex=%" PRIu32 " axis=%u guest=%08" PRIx32 " gpu=%08" PRIx32,
                           first_vertex, first_axis, first_guest, host_mesh.xyz[first_vertex][first_axis]);
    fputc('\n', stderr);
    if (host_mesh.samples == HOST_MESH_SAMPLES) host_mesh_release();
}

/* Only the application's own image or heap can supply a recognized renderer
   vtable. Mirror guest_read's exact image_8000 bounds, not a guessed map. */
static int host_plan_vtable_word(uint32_t address) {
    return !(address & 3) && address >= 0x1000u &&
        (heap_word(address) || address - 0x8000u <= sizeof image_8000 - 4);
}

static int host_scene_ranges_distinct(const struct host_scene_target *t) {
    uint32_t address[5] = { t->colour, t->depth, t->colour6, t->depth6, t->alpha };
    uint32_t size[5] = { t->width * t->height * 2u, t->width * t->height * 4u,
                         t->width * t->height * 2u, t->width * t->height * 4u,
                         t->width * t->height };
    for (unsigned i = 0; i < 5; i++) {
        if (UINT32_MAX - address[i] < size[i] || !host_bytes(address[i], size[i])) return 0;
        for (unsigned j = 0; j < i; j++) {
            uint32_t end = address[j] + size[j];
            if (address[i] < end && address[j] < address[i] + size[i]) return 0;
        }
    }
    return 1;
}

static int host_tris_plan_tri_equal(const struct host_tri *a, const struct host_tri *b) {
    unsigned args = (a->kind == 6) ? 4 : 3;
    return a->kind == b->kind && a->renderer == b->renderer && a->vertex_base == b->vertex_base &&
        !memcmp(a->stack, b->stack, args * sizeof a->stack[0]) && !memcmp(a->v, b->v, sizeof a->v);
}

/* Snapshot the prospective calls, not framebuffer contents. No guest state is
   changed: the ordinary entry hooks will independently capture the real calls. */
static void host_tris_plan_check(uint32_t *stack, const uint32_t *r) {
    if ((!host_tris.plan_check && !host_tris.skip_raster) || !host_tris.composite_session) return;
    if (host_tris_plan.entered) {
        host_tris_plan.unavailable = "multiple forward batches";
        host_tris_plan.bad_record = 0;
        return;
    }
    host_tris_plan.entered = 1;
    /* 70e44 loads [fp-588]; the renderer is reloaded from [fp-536]. */
    host_tris_plan.unavailable = "unmapped forward frame locals";
    if (r[11] < 588) return;
    uint32_t locals = r[11] - 588;
    if ((locals & 3) || (!host_bytes(locals, 56) &&
        locals - (uint32_t)stack_base > stack_size - 56u)) return;
    uint32_t renderer = guest_read(stack, locals + 52);
    uint32_t viewport = guest_read(stack, locals);
    struct host_scene_target target;
    host_tris_plan.unavailable = "invalid compositor surface or render planes";
    if (!host_scene_target(stack, host_tris.composite_session, &target)) return;
    host_tris_plan.unavailable = "renderer or viewport does not match compositor";
    if (renderer != target.renderer || viewport != target.viewport) return;
    host_tris_plan.unavailable = "unmapped viewport record metadata";
    if (!host_texture_words(viewport, 0x138)) return;
    /* 70db0..70db8 and 70dd8..70dec establish count, sorted uint32 indices
       and projected record base. 70ee0..70eec uses base + index * 0x58. */
    uint32_t count = guest_read(stack, viewport + 0xf0);
    uint32_t base = r[6], order = r[7];
    host_tris_plan.unavailable = "forward register bases do not match viewport";
    if (base != guest_read(stack, viewport + 0x134) || order != guest_read(stack, viewport + 0xf8)) return;
    host_tris_plan.unavailable = "record count exceeds capture limit";
    if (count > HOST_TRIS_MAX) return;
    host_tris_plan.unavailable = "unmapped sorted indices or projected record span";
    if (count && (!host_texture_words(order, count * 4) ||
                  !host_texture_words(base, count * HOST_PROJECT_RECORD_STRIDE))) return;

    /* Match the scene's texture budget and validate each distinct binding once. */
    host_tris_plan.texture_count = 0;
    static struct { uint32_t arg, secondary, layout; } bindings[HOST_SCENE_TEXTURES];
    unsigned binding_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t index = guest_read(stack, order + i * 4);
        if (index >= count) {
            host_tris_plan.unavailable = "sorted index outside projected record span";
            host_tris_plan.bad_record = order + i * 4;
            return;
        }
        uint32_t record = base + index * HOST_PROJECT_RECORD_STRIDE;
        const unsigned char *bytes = host_bytes(record, HOST_PROJECT_RECORD_STRIDE);
        if (bytes[HOST_PROJECT_FLAG]) continue; /* 70ef0..70ef8 */
        host_tris_plan.bad_record = record;
        uint32_t kind = bytes[0x48];
        if (kind == 0 || kind == 6) {
            /* 70f48: slot 0; 70ff0: slot 1. Resolve the actual indirect target. */
            uint32_t table = guest_read(stack, renderer), slot = kind == 6 ? 4 : 0;
            host_tris_plan.unavailable = "unmapped renderer vtable slot";
            if (table > UINT32_MAX - slot || !host_plan_vtable_word(table + slot)) return;
            host_tris_plan.unavailable = kind == 6 ? "unexpected kind-6 vtable target" : "unexpected kind-0 vtable target";
            if (guest_read(stack, table + slot) != (kind == 6 ? 0x85bb0u : 0x83fa8u)) return;
        } else if (kind != 4 && kind != 5) {
            kind = 1; /* 70e84..70eb4: default direct call */
        }

        struct host_tri *t = &host_tris_plan.tris[host_tris_plan.count];
        t->kind = kind;
        t->renderer = renderer;
        t->vertex_base = record;
        uint32_t arg = guest_read(stack, record + 0x4c);
        if (kind != 4 && kind != 5) {
            for (unsigned j = 0; j < 2; j++) {
                host_tris_plan.unavailable = j ? "unmapped texture wrapper inner pointer" : "unmapped texture wrapper";
                if (!host_texture_words(arg, 4)) return;
                arg = guest_read(stack, arg);
            }
        }
        t->stack[0] = arg;
        /* 70e5c..70e64 sets 0x44000+0x7e0+0xc = 0x447ec, the very
           same primary slot checked by host_scene_target. Kind 6 takes the
           pair at renderer+0x38/+0x3c; kind 5 inserts its weight wrapper. */
        uint32_t secondary = kind == 5 ? guest_read(stack, record + 0x54) : 0;
        if (kind == 5) {
            t->stack[1] = secondary;
            t->stack[2] = target.colour;
            t->stack[3] = target.depth;
        } else {
            t->stack[1] = kind == 6 ? target.colour6 : target.colour;
            t->stack[2] = kind == 6 ? target.depth6 : target.depth;
            t->stack[3] = kind == 6 ? bytes[0x4a] : 0;
        }
        for (unsigned v = 0; v < 3; v++) {
            for (unsigned w = 0; w < 6; w++)
                t->v[v][w] = guest_read(stack, record + v * HOST_PROJECT_VERTEX_STRIDE + w * 4);
            host_tris_plan.unavailable = "invalid vertex depth index";
            uint32_t depth_index = t->v[v][2];
            if (depth_index >= HOST_PROJECT_TABLE_COUNT || ((kind == 4 || kind == 5) && !depth_index)) return;
        }

        uint32_t layout = kind == 4 || kind == 5 ? kind : 0;
        unsigned j = 0;
        while (j < binding_count && (bindings[j].arg != arg ||
               bindings[j].secondary != secondary || bindings[j].layout != layout)) j++;
        if (j == binding_count) {
            host_tris_plan.unavailable = "texture bindings exceed scene limit";
            if (binding_count == HOST_SCENE_TEXTURES) return;
            vk_scene_texture tex = host_texture_binding(stack, layout, arg, secondary);
            host_tris_plan.unavailable = kind == 5 ? "invalid direct/weight texture binding" : "invalid indexed texture binding";
            if (!tex.address) return;
            if (tex.weights) {
                host_tris_plan.unavailable = "kind-5 texture weight exceeds 256";
                for (uint32_t w = 0; w < tex.width * tex.height; w++)
                    if (tex.weights[w] > 256) return;
            }
            host_tris_plan.textures[j] = tex;
            bindings[j].arg = arg;
            bindings[j].secondary = secondary;
            bindings[j].layout = layout;
            binding_count++;
            host_tris_plan.texture_count = binding_count;
        }
        host_tris_plan.texture_index[host_tris_plan.count] = j;
        host_tris_plan.count++;
    }
    host_tris_plan.bad_record = 0;
    host_tris_plan.unavailable = NULL;
}
static int host_scene_plan_render(uint32_t *stack, uint32_t session, const struct host_scene_target *target,
                                  unsigned serial) {
    if (!vk_present.device || host_scene.failed || !host_tris_plan.count) return 0;
    if (host_scene.created && (host_scene.width != target->width || host_scene.height != target->height ||
                               host_scene.scene.scale != host_tris.scene_scale)) host_scene_release();
    if (!host_scene.created) {
        if (!vk_scene_create(&host_scene.scene, vk_present.physical, vk_present.device, vk_present.queue,
                             vk_present.queue_family, target->width, target->height, host_tris.scene_scale)) return 0;
        host_scene.created = 1;
        host_scene.width = target->width; host_scene.height = target->height;
        size_t pixels = (size_t)target->width * target->height;
        host_scene.c0 = malloc(pixels * 2); host_scene.blit = malloc(pixels * 2); host_scene.hud_mask = malloc(pixels);
        if (!host_scene.c0 || !host_scene.blit || !host_scene.hud_mask) { host_scene_release(); return 0; }
    }
    static vk_scene_tri tris[HOST_TRIS_MAX];
    for (unsigned i = 0; i < host_tris_plan.count; i++) {
        const struct host_tri *t = &host_tris_plan.tris[i];
        tris[i].kind = t->kind;
        tris[i].texture = host_tris_plan.texture_index[i];
        tris[i].alpha = (uint8_t)t->stack[3];
        memcpy(tris[i].v, t->v, sizeof tris[i].v);
        tris[i].projection.valid = 0;
        if (host_tris.project_enabled && host_project_live[i].valid) tris[i].projection = host_project_live[i];
    }
    vk_scene_layers layers;
    double render_t = host_prof_now();
    int render_ok = vk_scene_render(&host_scene.scene, tris, host_tris_plan.count, host_tris_plan.textures,
                                    host_tris_plan.texture_count, 1, &layers);
    host_prof.render += host_prof_now() - render_t;
    if (!render_ok) {
        host_scene_release();
        return 0;
    }
    host_scene.target = *target;
    host_scene.session = session;
    host_scene.serial = serial;
    host_scene.flash = 0;
    host_scene.layers = layers;
    host_scene.render_valid = 1;
    host_raster.committed = 1;
    host_raster.forward_entry = 0;
    host_raster.expected = host_raster.bypassed = 0;
    host_raster.batches++;
    (void)stack;
    return 1;
}

static void host_tris_plan_forward(uint32_t *stack, const uint32_t *r) {
    host_tris_plan_check(stack, r);
    if (!host_tris.skip_raster || host_tris_plan.unavailable) return;
    struct host_scene_target target;
    host_raster.refusal = NULL;
    if (!host_scene_target(stack, host_tris.composite_session, &target) || !host_scene_ranges_distinct(&target)) {
        host_tris_plan.unavailable = "native target ranges alias";
        host_raster.refusal = host_tris_plan.unavailable;
        return;
    }
    uint32_t player = guest_read(stack, host_tris.composite_session + 0x2cc);
    if (guest_read(stack, host_tris.composite_session + 0x1cdc) == 0x1a) {
        host_tris_plan.unavailable = "grayscale effect state is active";
        host_raster.refusal = host_tris_plan.unavailable;
        return;
    }
    if (guest_read(stack, host_tris.composite_session + 0x7fc) != 0) {
        host_tris_plan.unavailable = "scene clear state is active";
        host_raster.refusal = host_tris_plan.unavailable;
        return;
    }
    if (!host_bytes(player, 0x5ec)) {
        host_tris_plan.unavailable = "player state is unmapped";
        host_raster.refusal = host_tris_plan.unavailable;
        return;
    }
    if (guest_read_small(stack, player + 0x5eb, 1)) {
        host_tris_plan.unavailable = "flash effect state is active";
        host_raster.refusal = host_tris_plan.unavailable;
        return;
    }
    for (unsigned i = 0; i < host_tris_plan.count; i++) {
        struct host_tri *t = &host_tris_plan.tris[i];
        if ((t->kind != 0 && t->kind != 1 && t->kind != 6) ||
            (int32_t)t->v[0][2] <= 0 || (int32_t)t->v[1][2] <= 0 || (int32_t)t->v[2][2] <= 0) {
            host_tris_plan.unavailable = "unsupported kind or non-positive depth";
            host_raster.refusal = host_tris_plan.unavailable;
            return;
        }
    }
    unsigned serial = host_tris_frame.serial + 1;
    if (!host_scene_plan_render(stack, host_tris.composite_session, &target, serial)) {
        host_tris_plan.unavailable = "GPU scene render failed";
        host_raster.refusal = host_tris_plan.unavailable;
    }
}

static void host_tris_plan_report(uint32_t session, unsigned serial) {
    if (!host_tris.plan_check) return;
    unsigned actual = host_tris.live_count, planned = host_tris_plan.count;
    unsigned matched = 0, word = 0;
    uint32_t record = 0;
    const char *mismatch = NULL, *unavailable = host_tris_plan.unavailable;
    int log = host_tris.log_every && serial % host_tris.log_every == 0;
    if (!session || session != host_tris.composite_session) unavailable = "not compositor geometry";
    else if (host_tris.dropped) unavailable = "raster captures dropped";
    if (unavailable) {
        if (log) {
            fprintf(stderr, "vk-plan: call %u matched=0 fallback=%u unavailable: %s", serial,
                    actual + host_tris.dropped, unavailable);
            if (host_tris_plan.bad_record) fprintf(stderr, " at 0x%08" PRIx32, host_tris_plan.bad_record);
            fputc('\n', stderr);
        }
        return;
    }

    for (; matched < planned && matched < actual; matched++) {
        const struct host_tri *p = &host_tris_plan.tris[matched], *a = &host_tris.live[matched];
        record = p->vertex_base;
        if (p->vertex_base != a->vertex_base) mismatch = "record address/order";
        else if (p->renderer != a->renderer) mismatch = "renderer";
        else if (p->kind != a->kind) mismatch = "material kind";
        else if (matched == host_tris_plan.bad_vertex_args) mismatch = "vertex pointer stride";
        else {
            unsigned args = (p->kind == 5 || p->kind == 6) ? 4 : 3;
            for (word = 0; word < args && p->stack[word] == a->stack[word]; word++) {}
            if (word < args) mismatch = "stack argument";
            else if (memcmp(p->v, a->v, sizeof p->v)) {
                mismatch = "vertex word";
                for (word = 0; p->v[word / 6][word % 6] == a->v[word / 6][word % 6]; word++) {}
            }
        }
        if (mismatch) break;
    }
    if (!mismatch && planned != actual) {
        mismatch = planned > actual ? "missing raster submission" : "extra raster submission";
        record = planned > actual ? host_tris_plan.tris[matched].vertex_base : host_tris.live[matched].vertex_base;
    }
    if (!log && !mismatch) return;
    fprintf(stderr, "vk-plan: call %u matched=%u fallback=%u planned=%u captured=%u", serial,
            matched, (planned > actual ? planned : actual) - matched, planned, actual);
    if (mismatch) {
        fprintf(stderr, " mismatch: %s at submission %u record=0x%08" PRIx32, mismatch, matched, record);
        if (matched < actual && matched < planned) {
            const struct host_tri *p = &host_tris_plan.tris[matched], *a = &host_tris.live[matched];
            if (p->vertex_base != a->vertex_base) fprintf(stderr, " captured=0x%08" PRIx32, a->vertex_base);
            else if (p->renderer != a->renderer) fprintf(stderr, " expected=0x%08" PRIx32 " actual=0x%08" PRIx32, p->renderer, a->renderer);
            else if (p->kind != a->kind) fprintf(stderr, " expected=%" PRIu32 " actual=%" PRIu32, p->kind, a->kind);
            else if (matched != host_tris_plan.bad_vertex_args) {
                unsigned args = (p->kind == 5 || p->kind == 6) ? 4 : 3;
                if (memcmp(p->stack, a->stack, args * sizeof p->stack[0]))
                    fprintf(stderr, " slot=%u expected=0x%08" PRIx32 " actual=0x%08" PRIx32,
                            word, p->stack[word], a->stack[word]);
                else
                    fprintf(stderr, " vertex=%u word=%u expected=0x%08" PRIx32 " actual=0x%08" PRIx32,
                            word / 6, word % 6, p->v[word / 6][word % 6], a->v[word / 6][word % 6]);
            }
        }
    }
    fputc('\n', stderr);
}

static void host_tris_project(uint32_t *stack, const uint32_t *r) {
    if (!host_tris.project_enabled) return;
    host_project_batch.count = 0;
    if (!host_tris.composite_session || !r[2] || r[2] > HOST_TRIS_MAX) return;
    uint32_t base = r[1], count = r[2];
    const unsigned char *records = host_bytes(base, count * HOST_PROJECT_RECORD_STRIDE);
    if ((base & 3) || !records) return;
    struct host_scene_target target;
    if (!host_scene_target(stack, host_tris.composite_session, &target) || target.renderer != r[0] ||
        !host_texture_words(target.viewport, 0x134)) return;

    /* 0x711f0 refreshes renderer+0x1c from the viewport before using it as the scale. */
    uint32_t renderer = target.renderer, scale = guest_read(stack, target.viewport + 0x130);
    host_project_batch.renderer = renderer;
    host_project_batch.base = base;
    host_project_batch.center[0] = guest_read(stack, renderer + 0x20);
    host_project_batch.center[1] = guest_read(stack, renderer + 0x24);
    host_project_batch.colour[0] = target.colour;
    host_project_batch.colour[1] = target.colour6;
    host_project_batch.depth[0] = target.depth;
    host_project_batch.depth[1] = target.depth6;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t record = base + i * HOST_PROJECT_RECORD_STRIDE;
        host_project_batch.records[i].valid = 0;
        if (records[i * HOST_PROJECT_RECORD_STRIDE + HOST_PROJECT_FLAG]) continue;
        unsigned v;
        for (v = 0; v < 3; v++) {
            uint32_t addr = record + v * HOST_PROJECT_VERTEX_STRIDE;
            uint32_t idx = guest_read(stack, addr + 8);
            if (idx >= HOST_PROJECT_TABLE_COUNT) break;
            uint32_t reciprocal = guest_read(stack, renderer + HOST_PROJECT_TABLE + idx * 4);
            uint32_t *raw = host_project_batch.records[i].xy_inv[v];
            raw[0] = guest_read(stack, addr);
            raw[1] = guest_read(stack, addr + 4);
            raw[2] = host_project_asr(reciprocal, 12) * scale;
            host_project_batch.records[i].idx[v] = idx;
        }
        host_project_batch.records[i].valid = v == 3;
    }
    host_project_batch.count = count;
}

static int host_scene_same_target(const struct host_scene_target *a, const struct host_scene_target *b) {
    return a->renderer == b->renderer && a->viewport == b->viewport && a->surface == b->surface &&
        a->bitmap == b->bitmap && a->width == b->width && a->height == b->height &&
        a->colour == b->colour && a->depth == b->depth && a->colour6 == b->colour6 &&
        a->depth6 == b->depth6 && a->alpha == b->alpha;
}

static uint16_t host_scene_rgb565(const uint8_t *rgba) {
    return (uint16_t)(((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) | (rgba[2] >> 3));
}

static void host_scene_frame(uint32_t *stack, uint32_t session, unsigned serial) {
    if (!vk_present.device || host_scene.failed || session != host_tris.composite_session ||
        !session || host_tris.dropped) return;
    /* A later full-surface effect or clear can write the same RGB565 value as
       the baseline. Equality-based composition cannot protect those writes. */
    if (!host_texture_words(session, 0x1ce0) || guest_read(stack, session + 0x1cdc) == 0x1a ||
        guest_read(stack, session + 0x7fc) != 0) return;
    struct host_scene_target target;
    if (!host_scene_target(stack, session, &target)) return;
    uint32_t w = target.width, h = target.height;
    int composite = guest_read_small(stack, session + 0x1cc0, 1) != 0;
    int check = host_tris.scene_check && host_tris.scene_scale == 1 && host_tris.log_every &&
                serial % host_tris.log_every == 0;
    uint32_t player = guest_read(stack, session + 0x2cc);
    /* The guest decrements this byte between geometry return and the alpha hook. */
    if (!host_bytes(player, 0x5ec)) return;
    unsigned flash = guest_read_small(stack, player + 0x5eb, 1);
    if (host_scene.created && (w != host_scene.width || h != host_scene.height)) host_scene_release();
    if (!host_scene.created) {
        if (!vk_scene_create(&host_scene.scene, vk_present.physical, vk_present.device, vk_present.queue,
                             vk_present.queue_family, w, h, host_tris.scene_scale)) {
            host_scene.failed = 1;
            fprintf(stderr, "vk-scene: creation failed, scene pass disabled\n");
            return;
        }
        host_scene.created = 1; host_scene.width = w; host_scene.height = h;
        host_scene.c0 = malloc((size_t)w * h * 2); host_scene.blit = malloc((size_t)w * h * 2);
        host_scene.hud_mask = malloc((size_t)w * h);
        if (!host_scene.c0 || !host_scene.blit || !host_scene.hud_mask) {
            host_scene_release();
            host_scene.failed = 1;
            fprintf(stderr, "vk-scene: out of memory\n");
            return;
        }
        fprintf(stderr, "vk-scene: %ux%u target at scale %u\n", w, h, host_tris.scene_scale);
    }
    static vk_scene_tri tris[HOST_TRIS_MAX];
    static vk_scene_texture textures[HOST_SCENE_TEXTURES];
    static struct { uint32_t arg, secondary, layout; } keys[HOST_SCENE_TEXTURES];
    unsigned tri_count = 0, texture_count = 0, skipped = 0, unresolved = 0;
    for (unsigned i = 0; i < host_tris_frame.count; i++) {
        const struct host_tri *t = &host_tris_frozen[i];
        /* Lifted frame pointer is entry SP-4: 4 loads targets at fp+8/+12,
           5 at fp+12/+16 (its extra stack argument is the weight wrapper). */
        unsigned colour_arg = t->kind == 5 ? 2 : 1;
        if ((t->kind > 1 && t->kind != 4 && t->kind != 5 && t->kind != 6) ||
            t->renderer != target.renderer ||
            t->stack[colour_arg] != (t->kind == 6 ? target.colour6 : target.colour) ||
            t->stack[colour_arg + 1] != (t->kind == 6 ? target.depth6 : target.depth)) {
            skipped++; continue;
        }
        if ((t->kind == 4 || t->kind == 5) &&
            ((int32_t)t->v[0][2] <= 0 || (int32_t)t->v[1][2] <= 0 || (int32_t)t->v[2][2] <= 0)) {
            skipped++; continue;
        }
        composite |= t->kind == 6;
        uint32_t layout = t->kind == 4 || t->kind == 5 ? t->kind : 0;
        uint32_t secondary = layout == 5 ? t->stack[1] : 0;
        unsigned index = 0;
        while (index < texture_count && (keys[index].arg != t->stack[0] ||
               keys[index].secondary != secondary || keys[index].layout != layout)) index++;
        if (index == texture_count) {
            if (texture_count == HOST_SCENE_TEXTURES) { skipped++; continue; }
            keys[index].arg = t->stack[0];
            keys[index].secondary = secondary;
            keys[index].layout = layout;
            vk_scene_texture tex = host_texture_binding(stack, layout, t->stack[0], secondary);
            /* Validate the complete weight plane once per binding, including failed bindings. */
            if (tex.weights) {
                uint32_t n = tex.width * tex.height;
                for (uint32_t j = 0; j < n; j++) {
                    if (tex.weights[j] > 256) { tex = (vk_scene_texture){0}; break; }
                }
            }
            textures[index] = tex;
            texture_count++;
        }
        if (!textures[index].address) { unresolved++; continue; }
        vk_scene_tri *out = &tris[tri_count++];
        out->kind = t->kind; out->texture = index;
        out->alpha = (uint8_t)t->stack[3];
        memcpy(out->v, t->v, sizeof out->v);
        out->projection.valid = 0;
        /* Rendering is synchronous here, before the next geometry call can reuse this array. */
        if (host_tris.project_enabled && host_project_live[i].valid)
            out->projection = host_project_live[i];
    }
    /* Partial draws are useful for diagnostics only, not a complete high-resolution frame. */
    if ((skipped || unresolved) && !check) return;
    vk_scene_layers layers;
    double render_t = host_prof_now();
    int render_ok = vk_scene_render(&host_scene.scene, tris, tri_count, textures, texture_count, composite, &layers);
    host_prof.render += host_prof_now() - render_t;
    if (!render_ok) {
        host_scene_release();
        host_scene.failed = 1;
        fprintf(stderr, "vk-scene: render failed, scene pass disabled\n");
        return;
    }
    if (!skipped && !unresolved) {
        host_scene.target = target;
        host_scene.session = session;
        host_scene.serial = serial;
        host_scene.flash = flash;
        host_scene.layers = layers;
        host_scene.render_valid = 1;
    }
    if (!check) return;
    const uint16_t *guest = (const uint16_t *)host_bytes(target.colour, w * h * 2);
    const uint8_t *rgba = layers.rgba;
    unsigned both = 0, exact = 0, guest_only = 0, gpu_only = 0;
    for (uint32_t i = 0; i < w * h; i++) {
        uint16_t p = host_scene_rgb565(rgba + i * 4);
        int gpu_drawn = rgba[i * 4 + 3] != 0, guest_drawn = guest[i] != 0; /* black texels read as undrawn */
        if (gpu_drawn && guest_drawn) { both++; exact += guest[i] == p; }
        else if (guest_drawn) guest_only++;
        else if (gpu_drawn) gpu_only++;
    }
    fprintf(stderr, "vk-scene: call %u tris=%u skipped=%u unresolved=%u textures=%u both=%u exact=%u guest_only=%u gpu_only=%u\n",
            serial, tri_count, skipped, unresolved, texture_count, both, exact, guest_only, gpu_only);
}

/* Vulkan interpolates normalized reciprocal depth; convert back to the nearest guest word. */
static uint32_t host_scene_depth(float depth) {
    return (uint32_t)((double)depth * 536870912.0 + 0.5);
}
static int host_scene_commit_native_run(void) {
    if (!host_raster.committed || host_raster.expected != host_tris_plan.count || !host_scene.render_valid) {
        host_raster.committed = 0;
        return 0;
    }
    struct host_scene_target *t = &host_scene.target;
    uint16_t *colour = (uint16_t *)host_bytes(t->colour, t->width * t->height * 2u);
    uint32_t *depth = (uint32_t *)host_bytes(t->depth, t->width * t->height * 4u);
    uint16_t *colour6 = (uint16_t *)host_bytes(t->colour6, t->width * t->height * 2u);
    uint32_t *depth6 = (uint32_t *)host_bytes(t->depth6, t->width * t->height * 4u);
    uint8_t *alpha = (uint8_t *)host_bytes(t->alpha, t->width * t->height);
    if (!colour || !depth || !colour6 || !depth6 || !alpha) {
        host_raster.committed = 0;
        host_raster.refusal = "native plane became unavailable";
        return 0;
    }
    unsigned scale = host_scene.scene.scale;
    size_t row = (size_t)t->width * scale;
    for (unsigned y = 0; y < t->height; y++) for (unsigned x = 0; x < t->width; x++) {
        size_t at = ((size_t)y * scale + scale / 2) * row + (size_t)x * scale + scale / 2;
        size_t out = (size_t)y * t->width + x;
        colour[out] = host_scene_rgb565(host_scene.layers.rgba + at * 4);
        depth[out] = host_scene_depth(host_scene.layers.depth[at]);
        colour6[out] = host_scene_rgb565(host_scene.layers.kind6_rgba + at * 4);
        depth6[out] = host_scene_depth(host_scene.layers.kind6_depth[at]);
        alpha[out] = host_scene.layers.kind6_rgba[at * 4 + 3];
    }
    return 1;
}

static int host_scene_commit_native(void) {
    double t = host_prof_now();
    int result = host_scene_commit_native_run();
    host_prof.commit += host_prof_now() - t;
    return result;
}

static uint32_t host_scene_pack(uint16_t p) {
    return ((uint32_t)p | ((uint32_t)p << 16)) & UINT32_C(0x07e0f81f);
}

/* These packed lanes give the truncated RGB565 blend for k=0..32. Keep the ARM
   wrap and arithmetic shift even for negative dim weights from an overflowing threshold. */
static uint16_t host_scene_mix(uint16_t p, uint16_t s, uint32_t k) {
    uint32_t dst = host_scene_pack(p), src = host_scene_pack(s);
    uint32_t v = (dst + arm_shift((src - dst) * k, 2, 5)) & UINT32_C(0x07e0f81f);
    return (uint16_t)(v | (v >> 16));
}

/* 0x68f08..0x68f50: add to each RGB565 lane, clamp to 255 (not 31/63/31),
   then add the shifted lanes and keep the low halfword, including carries. */
static uint16_t host_scene_flash(uint16_t p, unsigned flash) {
    unsigned r = (p >> 11) + flash, g = ((p >> 5) & 63u) + flash, b = (p & 31u) + flash;
    return (uint16_t)(((r > 255 ? 255u : r) << 11) + ((g > 255 ? 255u : g) << 5) + (b > 255 ? 255u : b));
}

static void host_scene_alpha_run(uint32_t *stack, uint32_t session) {
    int rendered = host_scene.render_valid;
    host_scene_invalidate();
    if (!rendered || session != host_scene.session || host_scene.serial != host_tris_frame.serial) return;
    struct host_scene_target target;
    if (!host_scene_target(stack, session, &target) || !host_scene_same_target(&target, &host_scene.target)) return;

    const vk_scene_layers *layers = &host_scene.layers;
    int dim = guest_read_small(stack, session + 0x1cc0, 1) != 0;
    if (dim && !layers->depth) return;
    unsigned flash = host_scene.flash;
    if (flash || layers->depth) {
        uint32_t threshold = dim ? guest_read(stack, session + 0x1cd4) : 0;
        uint32_t shift = dim ? guest_read(stack, session + 0x1cd0) : 0;
        size_t count = (size_t)target.width * target.height * host_scene.scene.scale * host_scene.scene.scale;
        for (size_t i = 0; i < count; i++) {
            uint8_t *rgba = layers->rgba + i * 4;
            int main = rgba[3] != 0;
            uint16_t p = main ? host_scene_rgb565(rgba) : 0;
            if (flash) p = host_scene_flash(p, flash);
            if (layers->depth) {
                const uint8_t *src = layers->kind6_rgba + i * 4;
                uint32_t z = host_scene_depth(layers->depth[i]);
                uint32_t z6 = host_scene_depth(layers->kind6_depth[i]);
                uint32_t biased = z6 + UINT32_C(50000);
                if ((int32_t)(dim ? biased : z6) > 0 && (int32_t)z < (int32_t)biased) {
                    p = host_scene_mix(p, host_scene_rgb565(src), src[3] >> 3);
                    if (dim) z = biased; /* Even alpha 0..7 (or the cleared plane) can promote depth. */
                }
                if (dim && z && (int32_t)z < (int32_t)threshold) {
                    uint32_t q = arm_shift((threshold - z) << 5, 2, shift);
                    p = host_scene_mix(p, 0, (int32_t)q > 32 ? 32u : q);
                }
            }
            rgba[0] = (uint8_t)((p >> 11) << 3);
            rgba[1] = (uint8_t)(((p >> 5) & 63) << 2);
            rgba[2] = (uint8_t)((p & 31) << 3);
            rgba[3] = main || flash || p ? 255 : 0;
        }
    }

    uint32_t size = target.width * target.height * 2;
    memcpy(host_scene.c0, host_bytes(target.colour, size), size);
    memset(host_scene.hud_mask, 0, (size_t)target.width * target.height);
    host_scene.baseline_valid = 1;
}

static void host_scene_alpha(uint32_t *stack, uint32_t session) {
    double t = host_prof_now();
    host_scene_alpha_run(stack, session);
    host_prof.alpha += host_prof_now() - t;
}

/* Match only the compositor's bitmap at TitleScreenPresent. Each baseline can be used once;
   another blit (including an unsupported one) conservatively cancels the replacement. */
static void host_scene_blit(uint32_t pc, uint32_t bitmap, uint32_t data, uint32_t width, uint32_t height,
                            uint32_t stride, int64_t left, int64_t top) {
    int baseline = host_scene.baseline_valid;
    host_scene_invalidate();
    const struct host_scene_target *t = &host_scene.target;
    if (!baseline || pc != 0x59480 || bitmap != t->bitmap || data != t->colour || left || top ||
        width != t->width || height != t->height || stride != width * 2) return;
    const unsigned char *source = host_bytes(data, width * height * 2);
    if (!source) return;
    memcpy(host_scene.blit, source, (size_t)width * height * 2);
    host_scene.blit_valid = 1;
}

static uint32_t host_scene_lut(const uint32_t *lut, const uint8_t *rgba) {
    return lut[host_scene_rgb565(rgba)];
}

/* Protect explicitly marked HUD writes even when their colour equals the baseline. For all other
   overlays, keep the guest pixel if it changed between post-alpha, BitBlt and presentation.
   Uncovered host subpixels also keep the guest colour. */
static void host_scene_compose_run(uint32_t *out, const uint16_t *guest, unsigned width, unsigned height,
                                   unsigned scale, const uint32_t *lut) {
    const uint8_t *rgba = host_scene.created && host_scene.blit_valid && host_scene.width == width &&
                          host_scene.height == height && host_scene.scene.scale == scale
                          ? host_scene.layers.rgba : NULL;
    host_scene_invalidate();
    size_t row = (size_t)width * scale;
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            size_t i = (size_t)y * width + x;
            uint16_t g = guest[i];
            uint32_t guest_colour = lut[g];
            int host = rgba && !host_scene.hud_mask[i] && g == host_scene.blit[i] && host_scene.c0[i] == g;
            for (unsigned j = 0; j < scale; j++) {
                size_t at = ((size_t)y * scale + j) * row + (size_t)x * scale;
                for (unsigned k = 0; k < scale; k++)
                    out[at + k] = host && rgba[(at + k) * 4 + 3] ? host_scene_lut(lut, rgba + (at + k) * 4) : guest_colour;
            }
        }
}

static void host_scene_compose(uint32_t *out, const uint16_t *guest, unsigned width, unsigned height,
                               unsigned scale, const uint32_t *lut) {
    double t = host_prof_now();
    host_scene_compose_run(out, guest, width, height, scale, lut);
    host_prof.compose += host_prof_now() - t;
}

static void host_tris_end(uint32_t *stack, uint32_t session) {
    if (!host_raster.committed) host_scene_invalidate();
    if (!host_tris.configured) host_tris_configure();
    if (!host_tris.enabled) {
        host_tris.composite_session = 0;  /* mesh-only mode still closes geometry scope */
        return;
    }
    host_prof_frame();
    memcpy(host_tris_frozen, host_tris.live, host_tris.live_count * sizeof host_tris.live[0]);
    host_tris_frame.tris = host_tris_frozen;
    host_tris_frame.count = host_tris.live_count;
    unsigned serial = ++host_tris_frame.serial;
    if (host_tris.dropped) fprintf(stderr, "host-tris: frame %u dropped %u triangles (limit %d)\n", serial, host_tris.dropped, HOST_TRIS_MAX);
    if (host_tris.log_every && serial % host_tris.log_every == 0) {
        unsigned counts[8] = {0};
        for (unsigned i = 0; i < host_tris_frame.count; i++) counts[host_tris_frozen[i].kind & 7]++;
        fprintf(stderr, "host-tris: frame %u total=%u kind0=%u kind1=%u kind4=%u kind5=%u kind6=%u\n", serial,
                host_tris_frame.count, counts[0], counts[1], counts[4], counts[5], counts[6]);
        if (host_tris.project_enabled)
            fprintf(stderr, "vk-project: call %u matched=%u fallback=%u\n", serial,
                    host_project_matched, host_tris_frame.count - host_project_matched);
    }
    host_tris_plan_report(session, serial);
    if (host_tris.dump_path && debug_frame_count + 1 == host_tris.dump_frame) {
        FILE *file = fopen(host_tris.dump_path, "wb");
        if (!file) perror(host_tris.dump_path);
        else {
            if (fwrite(host_tris_frozen, sizeof host_tris_frozen[0], host_tris_frame.count, file) != host_tris_frame.count)
                fprintf(stderr, "host-tris: short write to %s\n", host_tris.dump_path);
            fclose(file);
        }
    }
    if (host_tris.scene_enabled) {
        if (host_tris.skip_raster && host_raster.committed) {
            int committed = host_scene_commit_native();
            if (host_tris.log_every && serial % host_tris.log_every == 0)
                fprintf(stderr, "vk-raster: call %u committed=%u batches=%" PRIu64 " bypassed=%u loops=%" PRIu64 "\n",
                        serial, committed, host_raster.batches, host_raster.bypassed, host_raster.loops);
            if (!committed && !host_raster.bypassed) host_scene_frame(stack, session, serial);
        } else if (host_tris.skip_raster && host_raster.bypassed) {
            fprintf(stderr, "vk-raster: call %u partial refusal after bypassed=%u (%s)\n", serial,
                    host_raster.bypassed, host_raster.refusal ? host_raster.refusal : "unknown");
        } else host_scene_frame(stack, session, serial);
    }
    host_tris.composite_session = 0;
    host_project_batch.count = 0;
}

#endif
