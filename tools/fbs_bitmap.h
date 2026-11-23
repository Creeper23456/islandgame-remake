/* Inlined into the generated standalone AppUi translation. Off-screen CFbsBitmap objects.
   Pixel data lives in the guest heap so TBitmapUtil and game code see the same guest addresses. */
#define FBS_EColor64K UINT32_C(7)
#define FBS_BITMAP_SLOTS 8
#define FBS_BITMAP_UTIL_SIZE 40

static struct fbs_bitmap_entry {
    uint32_t object, width, height, mode, stride, data;
} fbs_bitmaps[FBS_BITMAP_SLOTS];

static struct fbs_bitmap_entry *fbs_bitmap_find(uint32_t object) {
    for (unsigned i = 0; i < FBS_BITMAP_SLOTS; i++)
        if (fbs_bitmaps[i].data && fbs_bitmaps[i].object == object) return &fbs_bitmaps[i];
    return NULL;
}

/* CFbsBitmap() and Reset(): forget this object's pixel mapping. Guest heap memory is not reclaimed. */
static void fbs_bitmap_reset(uint32_t object) {
    struct fbs_bitmap_entry *entry = fbs_bitmap_find(object);
    if (entry) memset(entry, 0, sizeof *entry);
}

/* CFbsBitmap::Create(const TSize&, TDisplayMode). Returns 0 on any unsupported or failed request. */
static int fbs_bitmap_create(uint32_t *stack, uint32_t object, uint32_t size, uint32_t mode) {
    if (mode != FBS_EColor64K) {
        fprintf(stderr, "guest CFbsBitmap::Create: unsupported display mode %u\n", mode);
        return 0;
    }
    uint32_t width = guest_read(stack, size), height = guest_read(stack, size + 4);
    if (!width || !height || width > 4096 || height > 4096) return 0;
    fbs_bitmap_reset(object);
    struct fbs_bitmap_entry *slot = NULL;
    for (unsigned i = 0; i < FBS_BITMAP_SLOTS && !slot; i++)
        if (!fbs_bitmaps[i].data) slot = &fbs_bitmaps[i];
    if (!slot) return 0;
    uint32_t stride = (width * 2 + 3) & ~UINT32_C(3); /* EColor64K scan lines are word aligned */
    uint32_t data = heap_try_alloc(stride * height);
    if (!data) return 0;
    slot->object = object;
    slot->width = width;
    slot->height = height;
    slot->mode = mode;
    slot->stride = stride;
    slot->data = data;
    return 1;
}

/* CFbsBitmap::LockHeap(TInt) const and CFbsBitmap::DataAddress() const: both return the guest address of the pixel data. */
static int fbs_data_address(uint32_t bitmap, uint32_t *out) {
    struct fbs_bitmap_entry *entry = fbs_bitmap_find(bitmap);
    if (!entry) return 0;
    *out = entry->data;
    return 1;
}

/* CFbsBitmap::UnlockHeap(TInt) const. The heap lock keeps a block from moving or compressing; host pixel storage
   never moves, so unlocking only validates that the bitmap is one of ours. */
static int fbs_unlock_heap(uint32_t bitmap) {
    return fbs_bitmap_find(bitmap) != NULL;
}

/* CFbsBitmap::SizeInPixels() const returns TSize through a hidden result pointer (width, then height). */
static int fbs_size_in_pixels(uint32_t *stack, uint32_t result, uint32_t bitmap) {
    struct fbs_bitmap_entry *entry = fbs_bitmap_find(bitmap);
    if (!entry) return 0;
    guest_write(stack, result, entry->width);
    guest_write(stack, result + 4, entry->height);
    return 1;
}

/* TBitmapUtil(CFbsBitmap*): 40 bytes; iFbsBitmap is first, the cursor and state fields start cleared. */
static void fbs_util_init(uint32_t *stack, uint32_t util, uint32_t bitmap) {
    for (uint32_t offset = 0; offset < FBS_BITMAP_UTIL_SIZE; offset += 4) guest_write(stack, util + offset, 0);
    guest_write(stack, util, bitmap);
}

/* CFbsBitmapDevice::NewL(CFbsBitmap*): the object and its vtable live in the guest heap. Every slot is a trap address,
   so the first call that the original game makes is reported by slot and receiver until it is implemented. */
static int fbs_device_new(uint32_t *stack, uint32_t bitmap, uint32_t *out) {
    if (!fbs_bitmap_find(bitmap)) return 0;
    uint32_t object = heap_try_alloc(8), vtable = heap_try_alloc(0xa0);
    if (!object || !vtable) return 0;
    for (uint32_t slot = 0; slot < 0xa0; slot += 4) guest_write(stack, vtable + slot, DEVICE_BASE + slot);
    guest_write(stack, object, vtable);
    guest_write(stack, object + 4, bitmap);
    *out = object;
    return 1;
}

/* CFbsScreenDevice::NewL(const TDesC& aName, TDisplayMode): the name is not retained yet (no trace consumes it), and
   the display mode is recorded at +8 for the first slot that needs it. Trap slots as in fbs_device_new. */
static int fbs_screen_device_new(uint32_t *stack, uint32_t name, uint32_t mode, uint32_t *out) {
    (void)name;
    uint32_t object = heap_try_alloc(12), vtable = heap_try_alloc(0xa0);
    if (!object || !vtable) return 0;
    for (uint32_t slot = 0; slot < 0xa0; slot += 4) guest_write(stack, vtable + slot, DEVICE_BASE + slot);
    guest_write(stack, object, vtable);
    guest_write(stack, object + 8, mode);
    *out = object;
    return 1;
}

/* CFbsBitGc object layout (guest heap): +0 vtable, +4 device, +8 pen colour, +12 pen style, +16 brush colour,
   +20 brush style, +24 draw mode. Slots come from GDI.H (CGraphicsContext, CBitmapContext) and BITSTD.H (CFbsBitGc). */
#define BITGC_SIZE 32
static int fbs_bitgc_new(uint32_t *stack, uint32_t *out) {
    uint32_t object = heap_try_alloc(BITGC_SIZE), vtable = heap_try_alloc(0x11c);
    if (!object || !vtable) return 0;
    for (uint32_t slot = 0; slot < 0x11c; slot += 4) guest_write(stack, vtable + slot, BITGC_BASE + slot);
    guest_write(stack, object, vtable);
    *out = object;
    return 1;
}

/* Implemented CFbsBitGc slots. Returns 0 for slots that are not implemented yet, so the caller reports them. */
static int bitgc_slot(uint32_t *stack, uint32_t slot, uint32_t *r) {
    switch (slot) {
    case 0x24: /* CGraphicsContext::UseFont(const CFont*): the font is recorded; glyph drawing consumes it. */
        guest_write(stack, r[0] + 28, r[1]);
        return 1;
    case 0x3c: /* CGraphicsContext::SetPenColor(const TRgb&): TRgb is one 32-bit word */
        guest_write(stack, r[0] + 8, guest_read(stack, r[1]));
        return 1;
    case 0x40: /* CGraphicsContext::SetPenStyle(TPenStyle) */
        guest_write(stack, r[0] + 12, r[1]);
        return 1;
    case 0x48: /* CGraphicsContext::SetBrushColor(const TRgb&) */
        guest_write(stack, r[0] + 16, guest_read(stack, r[1]));
        return 1;
    case 0x4c: /* CGraphicsContext::SetBrushStyle(TBrushStyle) */
        guest_write(stack, r[0] + 20, r[1]);
        return 1;
    default:
        return 0;
    }
}

static int trap_slot(const char *class_name, uint32_t slot, uint32_t self) {
    fprintf(stderr, "guest %s slot +0x%02x on object 0x%08x is not implemented\n", class_name, slot, self);
    return 3;
}
