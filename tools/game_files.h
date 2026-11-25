#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <math.h>
#include <zlib.h>

#define RFS_SESSION_TOKEN UINT32_C(0x0000f5f5) /* the in-process RFs session handle */

static const char *simlife_root;
/* The installation drive (E:) holding what the SIS installed: application resources and \system\apps\island\. */
static const char *install_root;
static FILE *streams[32];

static int guest_string(const uint32_t *stack, uint32_t address, char *text, size_t cap) {
    for (size_t i = 0; i < cap; i++)
        if (!(text[i] = (char)guest_read_small(stack, address + (uint32_t)i, 1))) return 1;
    return 0;
}

/* Match each component case-insensitively without following symlinks. The original data tree is read-only;
   only existing files under the installation drive's loadsave directory may be opened for writing. */
static FILE *open_game_path(char *name, int writable) {
    const char *root;
    char *part;
    if (writable && strncasecmp(name, "e:\\system\\apps\\island\\loadsave\\", 31)) return NULL;
    if (!strncasecmp(name, "e:\\simlife\\", 11) || !strncasecmp(name, "c:\\simlife\\", 11)) {
        /* The distribution flattens the Symbian <drive>:\\SIMLIFE\\ISLAND directory. */
        root = simlife_root;
        part = name + 11;
        if (!strncasecmp(part, "island\\", 7)) part += 7;
    } else if (!strncasecmp(name, "e:\\system\\apps\\island\\", 22)) {
        root = install_root;
        part = name + 3;
    } else {
        return NULL;
    }
    if (!root) return NULL;
    int base = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (;;) {
        if (base < 0) return NULL;
        char *next = strchr(part, '\\');
        if (next) *next++ = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..") || strpbrk(part, "/:")) { close(base); return NULL; }
        DIR *dir = fdopendir(base);
        if (!dir) { close(base); return NULL; }
        int fd = -1;
        for (struct dirent *entry; (entry = readdir(dir));)
            if (!strcasecmp(part, entry->d_name)) {
                if (fd >= 0) { close(fd); fd = -1; break; }
                int access = writable && !next ? O_RDWR : O_RDONLY;
                fd = openat(base, entry->d_name, access | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | (next ? O_DIRECTORY : 0));
            }
        closedir(dir);
        if (next) { base = fd; part = next; continue; }
        struct stat st;
        if (fd < 0) return NULL;
        FILE *file = !fstat(fd, &st) && S_ISREG(st.st_mode) && (!writable || st.st_nlink == 1)
            ? fdopen(fd, writable ? "r+b" : "rb") : NULL;
        if (!file) close(fd);
        return file;
    }
}

static FILE *open_game_file(const uint32_t *stack, uint32_t path, uint32_t mode) {
    char name[512], flags[8];
    if (!guest_string(stack, path, name, sizeof name) ||
        !guest_string(stack, mode, flags, sizeof flags) || (strcmp(flags, "rb") && strcmp(flags, "r"))) return NULL;
    return open_game_path(name, 0);
}

static uint32_t stream_slot(FILE *file) {
    for (uint32_t i = 1; i < sizeof streams / sizeof streams[0]; i++)
        if (!streams[i]) { streams[i] = file; return i; }
    fclose(file);
    return 0;
}

/* RFile is RSubSessionBase: RFs session handle at +0, subsession handle at +4 (here a streams[] index). */
static FILE *rfile_stream(const uint32_t *stack, uint32_t self) {
    uint32_t handle = guest_read(stack, self + 4);
    return handle && handle < sizeof streams / sizeof streams[0] ? streams[handle] : NULL;
}

/* RFile::Open(RFs&, const TDesC16&, TUint aMode). EFileWrite is 0x200. Returns
   KErrNone, KErrNotFound (-1), or 1 for an unsupported request that the caller reports. */
static int rfile_open(uint32_t *stack, uint32_t self, uint32_t fs, uint32_t path, uint32_t mode) {
    uint32_t length, text = text_data(stack, path, &length);
    char name[512];
    if (guest_read(stack, fs) != RFS_SESSION_TOKEN || !text || length >= sizeof name) return 1;
    for (uint32_t i = 0; i < length; i++) {
        uint32_t ch = guest_read_small(stack, text + 2 * i, 2);
        if (!ch || ch > 0x7e) return 1;
        name[i] = (char)ch;
    }
    name[length] = 0;
    char shown[512];
    memcpy(shown, name, length + 1);
    FILE *file = open_game_path(name, !!(mode & 0x200));
    uint32_t handle = file ? stream_slot(file) : 0;
    if (!handle) {
        fprintf(stderr, "RFile::Open: %s not found\n", shown);
        return -1;
    }
    guest_write(stack, self, RFS_SESSION_TOKEN);
    guest_write(stack, self + 4, handle);
    return 0;
}

/* RFile::Write(const TDesC8&, TInt): update from the current file position, without truncating on Open. */
static int rfile_write(uint32_t *stack, uint32_t self, uint32_t desc, uint32_t length) {
    FILE *file = rfile_stream(stack, self);
    if (!file) return -8; /* KErrBadHandle */
    uint32_t available, from = text_data(stack, desc, &available);
    if (!from || length > available) return -6;
    for (uint32_t i = 0; i < length; i++)
        if (fputc((int)guest_read_small(stack, from + i, 1), file) == EOF) return -2;
    return fflush(file) ? -2 : 0;
}

/* RFile::Flush() commits buffered file data before the caller closes the handle. */
static int rfile_flush(const uint32_t *stack, uint32_t self) {
    FILE *file = rfile_stream(stack, self);
    if (!file) return -8; /* KErrBadHandle */
    if (fflush(file) || fsync(fileno(file))) return errno == ENOSPC ? -26 : -2;
    return 0;
}

/* RFile::SetSize(TInt) acts on an already-open file without reopening its path. */
static int rfile_set_size(const uint32_t *stack, uint32_t self, int32_t size) {
    FILE *file = rfile_stream(stack, self);
    if (!file) return -8; /* KErrBadHandle */
    if (size < 0) return -6; /* KErrArgument */
    int fd = fileno(file), flags = fcntl(fd, F_GETFL);
    struct stat st;
    if (flags < 0 || fstat(fd, &st)) return -2; /* KErrGeneral */
    if ((flags & O_ACCMODE) == O_RDONLY || !S_ISREG(st.st_mode) || st.st_nlink != 1)
        return -21; /* KErrAccessDenied */
    if (fflush(file) || ftruncate(fd, size)) return errno == ENOSPC ? -26 : -2;
    return 0;
}

/* RFile::Close(): closing a handle that was never opened is a no-op, as on Symbian. */
static void rfile_close(uint32_t *stack, uint32_t self) {
    FILE *file = rfile_stream(stack, self);
    if (file) {
        fclose(file);
        streams[guest_read(stack, self + 4)] = NULL;
    }
    guest_write(stack, self, 0);
    guest_write(stack, self + 4, 0);
}

/* RFile::Read(TDes8&[, TInt aLength]): reads up to aLength (default MaxLength) bytes from the current position and
   sets the descriptor length; end of file gives a short or empty read with KErrNone. */
static int rfile_read(uint32_t *stack, uint32_t self, uint32_t desc, uint32_t length, int whole) {
    FILE *file = rfile_stream(stack, self);
    uint32_t header = guest_read(stack, desc), max = guest_read(stack, desc + 4), old;
    uint32_t to = text_data(stack, desc, &old);
    if (whole) length = max;
    if (!file || !to || (header >> 28 != 2 && header >> 28 != 3) || length > max) return 0;
    uint32_t n = 0;
    for (int c; n < length && (c = fgetc(file)) != EOF; n++) guest_write_small(stack, to + n, (uint32_t)c, 1);
    if (ferror(file)) return 0;
    guest_write(stack, desc, (header & 0xf0000000u) | n);
    return 1;
}

/* RFile::Seek(TSeek, TInt&): ESeekStart 1, ESeekCurrent 2, ESeekEnd 3 (ESeekAddress 0 is ROM-only); the resulting
   position is written back. */
static int rfile_seek(uint32_t *stack, uint32_t self, uint32_t mode, uint32_t position) {
    static const int whence[] = { -1, SEEK_SET, SEEK_CUR, SEEK_END };
    FILE *file = rfile_stream(stack, self);
    if (!file || mode < 1 || mode > 3 || fseek(file, (int32_t)guest_read(stack, position), whence[mode])) return 0;
    long pos = ftell(file);
    if (pos < 0 || pos > INT32_MAX) return 0;
    guest_write(stack, position, (uint32_t)pos);
    return 1;
}

/* RFileReadStream over an RFile (estor UF_STRM.CPP / UF_BUF.CPP): attaching moves the file handle into the stream
   (aFile = RFile()), and releasing the stream closes the file. RReadStream::iSrc at +0 holds STREAM_TOKEN | the
   streams[] index; it is not a guest MStreamBuf, so guest code that calls the buffer directly stops at a bad access. */
#define STREAM_TOKEN UINT32_C(0x5f000000)
#define WRITE_STREAM_TOKEN UINT32_C(0x5e000000)
static struct {
    size_t length;
    unsigned char bytes[16384]; /* Symbian's default RFileBuf size. */
} write_stream_buffers[sizeof streams / sizeof streams[0]];

static inline FILE *read_stream_file(const uint32_t *stack, uint32_t self, uint32_t *slot) {
    uint32_t token = guest_read(stack, self);
    *slot = token & 0xffffu;
    return (token & 0xffff0000u) == STREAM_TOKEN && *slot && *slot < sizeof streams / sizeof streams[0]
        ? streams[*slot] : NULL;
}

/* RFileReadStream and RFileWriteStream constructors taking an open file and offset. */
static inline int file_stream_attach(uint32_t *stack, uint32_t self, uint32_t file_object, uint32_t position,
                                     uint32_t token) {
    FILE *file = rfile_stream(stack, file_object);
    uint32_t slot = guest_read(stack, file_object + 4);
    if (!file || (int32_t)position < 0 || fseek(file, (long)position, SEEK_SET)) return 0;
    if (token == WRITE_STREAM_TOKEN) {
        int flags = fcntl(fileno(file), F_GETFL);
        if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY) return 0;
        write_stream_buffers[slot].length = 0;
    }
    guest_write(stack, self, token | slot);
    guest_write(stack, file_object, 0);
    guest_write(stack, file_object + 4, 0);
    return 1;
}

/* RReadStream::Release(): no-op for a released stream, otherwise closes the attached file. */
static inline void read_stream_release(uint32_t *stack, uint32_t self) {
    uint32_t slot;
    FILE *file = read_stream_file(stack, self, &slot);
    if (file) {
        fclose(file);
        streams[slot] = NULL;
    }
    guest_write(stack, self, 0);
}

/* RReadStream::ReadXxxL: little-endian, size bytes. Returns 0, or KErrEof (-25) for a short read (a leave). */
static inline int32_t read_stream_bytes(uint32_t *stack, uint32_t self, unsigned size, uint32_t *value) {
    uint32_t slot;
    FILE *file = read_stream_file(stack, self, &slot);
    unsigned char bytes[4];
    if (!file || fread(bytes, 1, size, file) != size) return -25;
    *value = 0;
    for (unsigned i = 0; i < size; i++) *value |= (uint32_t)bytes[i] << (8 * i);
    return 0;
}

static FILE *write_stream_file(const uint32_t *stack, uint32_t self, uint32_t *slot) {
    uint32_t token = guest_read(stack, self);
    *slot = token & 0xffffu;
    return (token & 0xffff0000u) == WRITE_STREAM_TOKEN && *slot && *slot < sizeof streams / sizeof streams[0]
        ? streams[*slot] : NULL;
}

static int32_t write_stream_flush(FILE *file, uint32_t slot) {
    size_t length = write_stream_buffers[slot].length;
    write_stream_buffers[slot].length = 0;
    if (fwrite(write_stream_buffers[slot].bytes, 1, length, file) != length || fflush(file))
        return errno == ENOSPC ? -26 : -2;
    return 0;
}

static int32_t write_stream_bytes(const uint32_t *stack, uint32_t self, unsigned size, uint32_t value) {
    uint32_t slot;
    FILE *file = write_stream_file(stack, self, &slot);
    if (!file) return -8;
    for (unsigned i = 0; i < size; ++i) {
        if (write_stream_buffers[slot].length == sizeof write_stream_buffers[slot].bytes) {
            int32_t rc = write_stream_flush(file, slot);
            if (rc) return rc;
        }
        write_stream_buffers[slot].bytes[write_stream_buffers[slot].length++] = (unsigned char)(value >> (8 * i));
    }
    return 0;
}

/* Close commits outstanding bytes; Release (including cleanup after a leave) discards them. */
static void write_stream_release(uint32_t *stack, uint32_t self, int commit) {
    uint32_t slot;
    FILE *file = write_stream_file(stack, self, &slot);
    if (!file) return;
    if (commit && write_stream_flush(file, slot)) fputs("RWriteStream::Close: write failed\n", stderr);
    if (fclose(file)) fputs("RWriteStream: file close failed\n", stderr);
    streams[slot] = NULL;
    guest_write(stack, self, 0);
}

/* RFile::Size(TInt&). */
static int rfile_size(uint32_t *stack, uint32_t self, uint32_t size) {
    FILE *file = rfile_stream(stack, self);
    struct stat st;
    if (!file || fstat(fileno(file), &st) || st.st_size > INT32_MAX) return 0;
    guest_write(stack, size, (uint32_t)st.st_size);
    return 1;
}

/* CompleteWithAppPath(TDes&). Assumes the application lives at e:\sys\bin\ (the SIS destination is !:\sys\bin and the
   game hard-codes its data under e:). A name with a drive stays unchanged; a rooted name gets the drive; a relative or
   empty name is placed in the application directory. Returns KErrOverflow (-9) if the result does not fit. */
static int complete_with_app_path(uint32_t *stack, uint32_t desc) {
    uint32_t count, max = guest_read(stack, desc + 4);
    uint32_t from = text_data(stack, desc, &count);
    uint32_t header = guest_read(stack, desc);
    if (!from || (header >> 28 != 2 && header >> 28 != 3) || count > 255) return -2;
    uint16_t name[256], out[512];
    for (uint32_t i = 0; i < count; i++) name[i] = (uint16_t)guest_read_small(stack, from + 2 * i, 2);
    uint32_t n = 0;
    if (count >= 2 && name[1] == ':') {
        for (; n < count; n++) out[n] = name[n];
    } else {
        const char *prefix = name[0] == '\\' ? "e:" : "e:\\sys\\bin\\";
        for (const char *p = prefix; *p; p++) out[n++] = (uint16_t)(unsigned char)*p;
        for (uint32_t i = 0; i < count; i++) out[n++] = name[i];
    }
    if (n > max) return -9;
    uint32_t old, to = text_data(stack, desc, &old);
    for (uint32_t i = 0; i < n; i++) guest_write_small(stack, to + 2 * i, out[i], 2);
    guest_write(stack, desc, (header & 0xf0000000u) | n);
    return 0;
}

/* TDesC16::Left(int) const returns TPtrC16 through a hidden result pointer. Lengths beyond the source are not clamped. */
static int desc_left(uint32_t *stack, uint32_t result, uint32_t desc, uint32_t length) {
    uint32_t count, from = text_data(stack, desc, &count);
    if (!from || length > count) return 0;
    guest_write(stack, result, 0x10000000u | length);
    guest_write(stack, result + 4, from);
    return 1;
}

/* TDes16::Append(const TDesC16&). Symbian panics on overflow; this fails the emulated call instead. */
static int desc_append(uint32_t *stack, uint32_t dest, uint32_t src) {
    uint32_t header = guest_read(stack, dest), max = guest_read(stack, dest + 4);
    uint32_t count, old, from = text_data(stack, src, &count), to = text_data(stack, dest, &old);
    if ((header >> 28 != 2 && header >> 28 != 3) || count > 512 || old > max || count > max - old) return 0;
    uint16_t copy[512];
    for (uint32_t i = 0; i < count; i++) copy[i] = (uint16_t)guest_read_small(stack, from + 2 * i, 2);
    for (uint32_t i = 0; i < count; i++) guest_write_small(stack, to + 2 * (old + i), copy[i], 2);
    guest_write(stack, dest, (header & 0xf0000000u) | (old + count));
    return 1;
}

/* TDes16::Num(TInt64): replaces the contents with the signed decimal representation (Symbian panics on overflow; this
   fails the emulated call instead). */
static int desc_num(uint32_t *stack, uint32_t dest, int64_t value) {
    uint32_t header = guest_read(stack, dest), max = guest_read(stack, dest + 4), old;
    uint32_t to = text_data(stack, dest, &old);
    char text[24];
    int n = snprintf(text, sizeof text, "%lld", (long long)value);
    if ((header >> 28 != 2 && header >> 28 != 3) || !to || n < 0 || (uint32_t)n > max) return 0;
    for (int i = 0; i < n; i++) guest_write_small(stack, to + 2 * (uint32_t)i, (unsigned char)text[i], 2);
    guest_write(stack, dest, (header & 0xf0000000u) | (uint32_t)n);
    return 1;
}

/* Stores count elements read from src (src_width bytes each, zero-extended) into a modifiable descriptor whose
   elements are dest_width bytes, replacing its contents or appending. Symbian panics on overflow; this fails the
   emulated call instead. TDes8 and TDes16 share the header layout, so text_data serves both. */
static inline int desc_set_length(uint32_t *stack, uint32_t dest, uint32_t length);

static inline int desc_store(uint32_t *stack, uint32_t dest, unsigned dest_width, uint32_t src, unsigned src_width,
                             uint32_t count, int append) {
    uint32_t header = guest_read(stack, dest), max = guest_read(stack, dest + 4), old;
    uint32_t to = text_data(stack, dest, &old);
    uint32_t start = append ? old : 0;
    if (header >> 28 < 2 || header >> 28 > 4 || !to || old > max || count > max - start) return 0;
    if (dest_width == src_width && src < to + start * dest_width && to + start * dest_width < src + count * src_width) {
        /* Overlapping same-width copy: move backwards. */
        for (uint32_t i = count; i-- > 0;)
            guest_write_small(stack, to + (start + i) * dest_width, guest_read_small(stack, src + i * src_width, src_width), dest_width);
    } else {
        for (uint32_t i = 0; i < count; i++)
            guest_write_small(stack, to + (start + i) * dest_width, guest_read_small(stack, src + i * src_width, src_width), dest_width);
    }
    return desc_set_length(stack, dest, start + count);
}

/* TDes8/TDes16::AppendFill(TChar, TInt aLength): appends aLength copies of the character. */
static inline int desc_append_fill(uint32_t *stack, uint32_t dest, unsigned width, uint32_t ch, uint32_t count) {
    uint32_t header = guest_read(stack, dest), max = guest_read(stack, dest + 4), old;
    uint32_t to = text_data(stack, dest, &old);
    if ((header >> 28 != 2 && header >> 28 != 3) || !to || old > max || count > max - old ||
        ch > (width == 1 ? 0xffu : 0xffffu)) return 0;
    for (uint32_t i = 0; i < count; i++) guest_write_small(stack, to + (old + i) * width, ch, width);
    guest_write(stack, dest, (header & 0xf0000000u) | (old + count));
    return 1;
}

/* TDes16::AppendNumUC(TUint64, TRadix): unsigned digits in the radix (2, 8, 10 or 16), upper-case letters. */
static inline int desc_append_num_uc(uint32_t *stack, uint32_t dest, uint64_t value, uint32_t radix) {
    char digits[65];
    unsigned n = 0;
    if (radix != 2 && radix != 8 && radix != 10 && radix != 16) return 0;
    do {
        digits[n++] = "0123456789ABCDEF"[value % radix];
        value /= radix;
    } while (value);
    uint32_t header = guest_read(stack, dest), max = guest_read(stack, dest + 4), old;
    uint32_t to = text_data(stack, dest, &old);
    if ((header >> 28 != 2 && header >> 28 != 3) || !to || old > max || n > max - old) return 0;
    for (unsigned i = 0; i < n; i++) guest_write_small(stack, to + 2 * (old + i), (unsigned char)digits[n - 1 - i], 2);
    guest_write(stack, dest, (header & 0xf0000000u) | (old + n));
    return 1;
}

/* Same as desc_store with a source descriptor. */
static inline int desc_store_desc(uint32_t *stack, uint32_t dest, unsigned dest_width, uint32_t src_desc,
                                  unsigned src_width, int append) {
    uint32_t count, src = text_data(stack, src_desc, &count);
    return src && desc_store(stack, dest, dest_width, src, src_width, count, append);
}

/* TDes8/TDes16::SetLength(TInt): 0 <= length <= MaxLength. */
static inline int desc_set_length(uint32_t *stack, uint32_t dest, uint32_t length) {
    uint32_t header = guest_read(stack, dest);
    unsigned type = header >> 28;
    if (type < 2 || type > 4 || length > 0x0fffffffu || length > guest_read(stack, dest + 4)) return 0;
    if (type == 4) {
        uint32_t owner = guest_read(stack, dest + 8);
        if (!owner || guest_read(stack, owner) >> 28 != 0) return 0;
        guest_write(stack, owner, length);
    }
    guest_write(stack, dest, (header & 0xf0000000u) | length);
    return 1;
}

static inline int hbufc16_des(uint32_t *stack, uint32_t result, uint32_t owner) {
    uint32_t start = owner - heap_base - 8;
    if (start >= thread_heap.used || (start & 7)) return 0;
    const uint32_t *block = heap_block(start);
    if (block[1] != HEAP_LIVE || block[0] < 16 || block[0] > thread_heap.used - start || (block[0] & 7)) return 0;
    uint32_t max = (block[0] - 12) / 2, length = guest_read(stack, owner);
    if (length > 0x0fffffffu || length > max) return 0;
    guest_write(stack, result, 0x40000000u | length);
    guest_write(stack, result + 4, max);
    guest_write(stack, result + 8, owner);
    return 1;
}

/* TDes8/TDes16::Format subset used for numbered resources and script commands. */
static int desc_format_int(uint32_t *stack, uint32_t dest, uint32_t format, unsigned width, int32_t number) {
    uint32_t length, fmt = text_data(stack, format, &length), old, text = text_data(stack, dest, &old);
    if (!fmt || !text) return 0;
    char digits[12];
    unsigned ndigits = (unsigned)snprintf(digits, sizeof digits, "%" PRId32, number);
    if (ndigits >= sizeof digits) return 0;
    /* Validate before writing; an invalid directive or overflow must not truncate a path. */
    uint32_t total = 0;
    int seen = 0;
    for (uint32_t i = 0; i < length; i++, total++) {
        if (guest_read_small(stack, fmt + width * i, width) != '%') continue;
        if (++i == length) return 0;
        uint32_t spec = guest_read_small(stack, fmt + width * i, width);
        if (spec == '%') continue;
        if (spec != 'd' || seen++) return 0;
        total += ndigits - 1;
    }
    if (!desc_set_length(stack, dest, total)) return 0;
    uint32_t j = 0;
    for (uint32_t i = 0; i < length; i++) {
        uint32_t ch = guest_read_small(stack, fmt + width * i, width);
        if (ch == '%' && guest_read_small(stack, fmt + width * ++i, width) == 'd') {
            for (unsigned k = 0; k < ndigits; k++) guest_write_small(stack, text + width * j++, digits[k], width);
        } else guest_write_small(stack, text + width * j++, ch, width);
    }
    return 1;
}

/* The original menu passes its URL as a TDesC16; only web URLs leave the guest. */
static void open_official_website(const uint32_t *stack, uint32_t descriptor) {
    uint32_t length, text = text_data(stack, descriptor, &length);
    char url[257];
    if (!text || length >= sizeof url) goto invalid;
    for (uint32_t i = 0; i < length; i++) {
        uint32_t ch = guest_read_small(stack, text + 2 * i, 2);
        if (ch < 0x21 || ch > 0x7e || ch == '\\') goto invalid;
        url[i] = (char)ch;
    }
    url[length] = 0;
    const char *host = !strncmp(url, "https://", 8) ? url + 8 : !strncmp(url, "http://", 7) ? url + 7 : NULL;
    if (!host || !*host || strchr("/?#", *host)) goto invalid;
    if (SDL_OpenURL(url) < 0) fprintf(stderr, "Could not open the official website: %s\n", SDL_GetError());
    return;
invalid:
    fputs("Invalid official website URL\n", stderr);
}

/* RDebug::Print: the descriptor occupies r0. Variadic words start in r1,
   continue on the stack after r3, and align to two words for doubles. */
static int debug_print(const uint32_t *stack, const uint32_t r[16]) {
    uint32_t length, text = text_data(stack, r[0], &length);
    if (!text) return 0;
    char message[257];
    unsigned n = 0, arg = 1;
    for (uint32_t i = 0; i < length && n < sizeof message - 1; i++) {
        uint32_t ch = guest_read_small(stack, text + 2 * i, 2);
        if (ch == '%') {
            if (++i == length) return 0;
            uint32_t spec = guest_read_small(stack, text + 2 * i, 2);
            if (spec == 'd' || spec == 'f') {
                unsigned remaining = sizeof message - 1 - n;
                int used;
                if (spec == 'd') {
                    uint32_t word = arg < 4 ? r[arg] : guest_read(stack, r[13] + 4 * (arg - 4));
                    used = snprintf(message + n, remaining + 1, "%d", (int32_t)word);
                    arg++;
                } else {
                    arg = (arg + 1) & ~1u;
                    uint64_t bits = arg < 4 ? (uint64_t)r[3] << 32 | r[2] :
                                              guest_read64(stack, r[13] + 4 * (arg - 4));
                    double value;
                    memcpy(&value, &bits, sizeof value);
                    used = snprintf(message + n, remaining + 1, "%f", value);
                    arg += 2;
                }
                if (used < 0) return 0;
                n += (unsigned)used < remaining ? (unsigned)used : remaining;
                continue;
            }
            if (spec != '%') return 0;
        }
        message[n++] = (char)ch; /* SDK collapses the descriptor before DebugPrint. */
    }
    fputs("RDebug::Print: ", stderr);
    fwrite(message, 1, n, stderr);
    fputc('\n', stderr);
    return 1;
}

static inline int tptrc16_from_z(uint32_t *stack, uint32_t result, uint32_t text) {
    if (!text || (text & 1)) return 0;
    uint32_t length = 0;
    while (guest_read_small(stack, text + 2 * length, 2)) {
        if (length == 0x0fffffffu) return 0;
        length++;
    }
    guest_write(stack, result, 0x10000000u | length);
    guest_write(stack, result + 4, text);
    return 1;
}

/* Mem::Compare semantics (euser memcompare): first differing byte difference, else the length difference. */
static inline uint32_t mem_compare(const uint32_t *stack, uint32_t left, uint32_t left_len, uint32_t right, uint32_t right_len) {
    uint32_t n = left_len < right_len ? left_len : right_len;
    for (uint32_t i = 0; i < n; i++) {
        int32_t a = (int32_t)guest_read_small(stack, left + i, 1), b = (int32_t)guest_read_small(stack, right + i, 1);
        if (a != b) return (uint32_t)(a - b);
    }
    return (uint32_t)((int32_t)left_len - (int32_t)right_len);
}

static inline uint32_t guest_strlen(const uint32_t *stack, uint32_t text) {
    uint32_t n = 0;
    while (guest_read_small(stack, text + n, 1)) n++;
    return n;
}

/* C string functions over guest memory (ESTLIB). Bytes compare as unsigned char, as in ISO C. */
static inline uint32_t guest_strcmp(const uint32_t *stack, uint32_t a, uint32_t b) {
    for (;; a++, b++) {
        uint32_t x = guest_read_small(stack, a, 1), y = guest_read_small(stack, b, 1);
        if (x != y || !x) return (uint32_t)((int32_t)x - (int32_t)y);
    }
}

static inline uint32_t guest_memcmp(const uint32_t *stack, uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = guest_read_small(stack, a + i, 1), y = guest_read_small(stack, b + i, 1);
        if (x != y) return (uint32_t)((int32_t)x - (int32_t)y);
    }
    return 0;
}

static inline uint32_t guest_strcat(uint32_t *stack, uint32_t dest, uint32_t src) {
    uint32_t to = dest + guest_strlen(stack, dest), ch;
    do {
        ch = guest_read_small(stack, src++, 1);
        guest_write_small(stack, to++, ch, 1);
    } while (ch);
    return dest;
}

/* strrchr: the terminating NUL is part of the string, so strrchr(s, 0) finds it. */
static inline uint32_t guest_strrchr(const uint32_t *stack, uint32_t text, uint32_t ch) {
    uint32_t found = 0, c;
    ch &= 0xff;
    do {
        c = guest_read_small(stack, text, 1);
        if (c == ch) found = text;
        text++;
    } while (c);
    return found;
}

static inline uint32_t guest_strstr(const uint32_t *stack, uint32_t haystack, uint32_t needle) {
    uint32_t n = guest_strlen(stack, needle), h = guest_strlen(stack, haystack);
    for (uint32_t i = 0; i + n <= h; i++)
        if (!guest_memcmp(stack, haystack + i, needle, n)) return haystack + i;
    return 0;
}

/* atoi: optional whitespace and sign, then decimal digits; overflow wraps (undefined in ISO C). */
static inline uint32_t guest_atoi(const uint32_t *stack, uint32_t text) {
    uint32_t c, value = 0;
    int negative = 0;
    while ((c = guest_read_small(stack, text, 1)) == ' ' || (c >= '\t' && c <= '\r')) text++;
    if (c == '+' || c == '-') { negative = c == '-'; text++; }
    while ((c = guest_read_small(stack, text++, 1)) >= '0' && c <= '9') value = value * 10 + (c - '0');
    return negative ? 0u - value : value;
}

/* CArrayFixBase (e32base.h): vptr +0, iCount +4, iGranularity +8, iLength +12, iCreateRep +16, iBase +20. Count() is
   inline in guest code, so iCount must be exact; the remaining storage is private to the exported methods. iBase
   here is a guest block holding the capacity in elements followed by the flat element data. */
enum { ARRAY_COUNT = 4, ARRAY_GRANULARITY = 8, ARRAY_LENGTH = 12, ARRAY_BASE = 20 };

static inline void array_fix_init(uint32_t *stack, uint32_t self, uint32_t vtable, uint32_t length, uint32_t granularity) {
    guest_write(stack, self, vtable);
    guest_write(stack, self + ARRAY_COUNT, 0);
    guest_write(stack, self + ARRAY_GRANULARITY, granularity);
    guest_write(stack, self + ARRAY_LENGTH, length);
    guest_write(stack, self + 16, 0);
    guest_write(stack, self + ARRAY_BASE, 0);
}

/* Host vtable for CBase objects created by imported constructors and factories (CMsvEntrySelection, CMsvSession):
   the complete and deleting destructors at +0/+4 and Extension_ at +8, dispatched at CBASE_VTABLE_BASE. The guest
   heap does not free, and these objects own no host resources. */
#define CBASE_VTABLE_BASE UINT32_C(0xff007000)
static uint32_t cbase_host_vtable;

static inline uint32_t host_cbase_vtable(uint32_t *stack) {
    if (!cbase_host_vtable && (cbase_host_vtable = heap_try_alloc(12)))
        for (uint32_t slot = 0; slot < 12; slot += 4) guest_write(stack, cbase_host_vtable + slot, CBASE_VTABLE_BASE + slot);
    return cbase_host_vtable;
}

/* Host vtable for CBaseMtm objects from CClientMtmRegistry::NewMtmL. Messaging MTMs cannot act on this host: the CBase
   destructors and Extension_ (+0..+8) behave as for host CBase objects, and every other virtual stops at MTM_BASE with
   the slot named. */
#define MTM_BASE UINT32_C(0xff008000)
#define MTM_SLOTS 64
static uint32_t mtm_host_vtable;

static inline uint32_t host_mtm_vtable(uint32_t *stack) {
    if (!mtm_host_vtable && (mtm_host_vtable = heap_try_alloc(4 * MTM_SLOTS)))
        for (uint32_t slot = 0; slot < 4 * MTM_SLOTS; slot += 4)
            guest_write(stack, mtm_host_vtable + slot, slot < 12 ? CBASE_VTABLE_BASE + slot : MTM_BASE + slot);
    return mtm_host_vtable;
}

/* CArrayFixBase::At(TInt) const: address of element index (panics when out of range on Symbian). */
static inline uint32_t array_fix_at(const uint32_t *stack, uint32_t self, uint32_t index) {
    uint32_t base = guest_read(stack, self + ARRAY_BASE);
    if (!base || index >= guest_read(stack, self + ARRAY_COUNT)) return 0;
    return base + 4 + index * guest_read(stack, self + ARRAY_LENGTH);
}

/* CArrayFixBase::InsertL(TInt anIndex, const TAny* aPtr): grows by the granularity when full. */
static inline int array_fix_insert(uint32_t *stack, uint32_t self, uint32_t index, uint32_t source) {
    uint32_t count = guest_read(stack, self + ARRAY_COUNT), length = guest_read(stack, self + ARRAY_LENGTH);
    uint32_t base = guest_read(stack, self + ARRAY_BASE), capacity = base ? guest_read(stack, base) : 0;
    if (index > count || !length || length > 0x10000) return 0;
    if (count == capacity) {
        uint32_t grown = capacity + guest_read(stack, self + ARRAY_GRANULARITY);
        uint32_t block = heap_try_alloc(4 + grown * length);
        if (!block || grown <= capacity) return 0;
        guest_write(stack, block, grown);
        for (uint32_t i = 0; i < count * length; i++)
            guest_write_small(stack, block + 4 + i, guest_read_small(stack, base + 4 + i, 1), 1);
        guest_write(stack, self + ARRAY_BASE, block);
        base = block;
    }
    for (uint32_t i = (count - index) * length; i-- > 0;)
        guest_write_small(stack, base + 4 + (index + 1) * length + i, guest_read_small(stack, base + 4 + index * length + i, 1), 1);
    for (uint32_t i = 0; i < length; i++)
        guest_write_small(stack, base + 4 + index * length + i, guest_read_small(stack, source + i, 1), 1);
    guest_write(stack, self + ARRAY_COUNT, count + 1);
    return 1;
}

/* EZLib compress/uncompress (ezlib2.def ordinals 49/69): zlib's one-shot API with 32-bit uLong. The guest buffers are
   copied through the checked accessors; the zlib return code (Z_OK, Z_BUF_ERROR, ...) is passed back unchanged. */
static int ezlib_oneshot(uint32_t *stack, uint32_t *r, int inflate_data) {
    uint32_t dest = r[0], dest_len_ptr = r[1], source = r[2], source_len = r[3];
    uint32_t capacity = guest_read(stack, dest_len_ptr);
    unsigned char *in = malloc(source_len ? source_len : 1), *out = malloc(capacity ? capacity : 1);
    if (!in || !out) { free(in); free(out); return 0; }
    for (uint32_t i = 0; i < source_len; i++) in[i] = (unsigned char)guest_read_small(stack, source + i, 1);
    uLongf produced = capacity;
    int rc = inflate_data ? uncompress(out, &produced, in, source_len) : compress(out, &produced, in, source_len);
    if (rc == Z_OK) {
        for (uLongf i = 0; i < produced; i++) guest_write_small(stack, dest + (uint32_t)i, out[i], 1);
        guest_write(stack, dest_len_ptr, (uint32_t)produced);
    }
    free(in);
    free(out);
    r[0] = (uint32_t)rc;
    return 1;
}

/* RFs::Connect(int): RSessionBase keeps its handle in the first word. The host file server is in-process, so the
   session handle is a fixed non-zero token (RFS_SESSION_TOKEN); connecting an object that already holds a handle fails. */
static int rfs_connect(uint32_t *stack, uint32_t self) {
    if (guest_read(stack, self) != 0) return 0;
    guest_write(stack, self, RFS_SESSION_TOKEN);
    return 1;
}

/* TRect::SetRect(const TPoint& aTL, const TSize& aSize): iTl = aTL, iBr = aTL + aSize (us_graph.cpp). */
static void rect_set_rect(uint32_t *stack, uint32_t rect, uint32_t top_left, uint32_t size) {
    uint32_t x = guest_read(stack, top_left), y = guest_read(stack, top_left + 4);
    uint32_t w = guest_read(stack, size), h = guest_read(stack, size + 4);
    guest_write(stack, rect, x);
    guest_write(stack, rect + 4, y);
    guest_write(stack, rect + 8, x + w);
    guest_write(stack, rect + 12, y + h);
}

/* User::TickCount(): monotonic milliseconds, consistent with the 1000 us tick reported by HAL. Masked to a positive TInt. */
static uint32_t user_tick_count(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint32_t)(((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u) & 0x7fffffffu);
}

/* Soft-float EABI helpers: doubles travel in register pairs and match IEEE-754 binary64 on the host. */
static uint64_t double_bits(uint32_t lo, uint32_t hi) { return (uint64_t)lo | ((uint64_t)hi << 32); }

static void aeabi_i2d(uint32_t *r) {
    double value = (double)(int32_t)r[0];
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    r[0] = (uint32_t)bits;
    r[1] = (uint32_t)(bits >> 32);
}

/* Double operand in register pair first, first+1 (EABI, little-endian word order). */
static double dbl_arg(const uint32_t *r, unsigned first) {
    uint64_t bits = double_bits(r[first], r[first + 1]);
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static inline uint32_t stream_real32_bits(const uint32_t *r) {
    float value = (float)dbl_arg(r, 2);
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* Double result into r0:r1. */
static void put_dbl(uint32_t *r, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    r[0] = (uint32_t)bits;
    r[1] = (uint32_t)(bits >> 32);
}

/* Math::Sin/Cos(TReal& aTrg, const TReal& aSrc). Host libm may differ from Symbian's implementation in the last bits. */
static int math_unary(uint32_t *stack, uint32_t target, uint32_t source, double (*fn)(double)) {
    uint64_t in_bits = (uint64_t)guest_read(stack, source) | ((uint64_t)guest_read(stack, source + 4) << 32), out_bits;
    double x, y;
    memcpy(&x, &in_bits, sizeof x);
    if (!isfinite(x)) return -6; /* KErrArgument */
    y = fn(x);
    memcpy(&out_bits, &y, sizeof out_bits);
    guest_write(stack, target, (uint32_t)out_bits);
    guest_write(stack, target + 4, (uint32_t)(out_bits >> 32));
    return 0;
}

/* Math::Sqrt / Math::ACos: a source outside [low, high] gives KErrArgument (um_sqrt.cpp, um_asin.cpp); otherwise as
   math_unary. */
static int math_unary_domain(uint32_t *stack, uint32_t target, uint32_t source, double (*fn)(double),
                             double low, double high) {
    uint64_t bits = (uint64_t)guest_read(stack, source) | ((uint64_t)guest_read(stack, source + 4) << 32);
    double x;
    memcpy(&x, &bits, sizeof x);
    if (x < low || x > high) return -6; /* KErrArgument */
    return math_unary(stack, target, source, fn);
}

/* __aeabi_idivmod: quotient in r0, remainder in r1, both truncated toward zero. Division by zero and INT_MIN/-1 fail. */
static int aeabi_idivmod(uint32_t *r) {
    int32_t numerator = (int32_t)r[0], denominator = (int32_t)r[1];
    if (!denominator || (numerator == INT32_MIN && denominator == -1)) return 0;
    r[0] = (uint32_t)(numerator / denominator);
    r[1] = (uint32_t)(numerator % denominator);
    return 1;
}

/* Single-precision EABI helpers: a float travels as its IEEE-754 binary32 bits in one core register. */
static inline float flt_arg(const uint32_t *r, unsigned index) {
    float value;
    memcpy(&value, &r[index], sizeof value);
    return value;
}

static inline void put_flt(uint32_t *r, float value) {
    memcpy(&r[0], &value, sizeof value);
}

/* Float/double to integer conversions truncate toward zero; NaN gives 0 and out-of-range values saturate. */
static inline uint32_t to_int32_sat(double x) {
    if (isnan(x)) return 0;
    if (x >= 2147483647.0) return (uint32_t)INT32_MAX;
    if (x <= -2147483648.0) return (uint32_t)INT32_MIN;
    return (uint32_t)(int32_t)x;
}

static inline uint32_t to_uint32_sat(double x) {
    if (isnan(x) || x <= 0.0) return 0;
    if (x >= 4294967295.0) return UINT32_MAX;
    return (uint32_t)x;
}

/* __aeabi_ldivmod: signed 64-bit r0:r1 / r2:r3, quotient in r0:r1 and remainder in r2:r3. */
static inline int aeabi_ldivmod(uint32_t *r) {
    int64_t n = (int64_t)((uint64_t)r[1] << 32 | r[0]), d = (int64_t)((uint64_t)r[3] << 32 | r[2]);
    if (!d || (n == INT64_MIN && d == -1)) return 0;
    uint64_t q = (uint64_t)(n / d), m = (uint64_t)(n % d);
    r[0] = (uint32_t)q; r[1] = (uint32_t)(q >> 32); r[2] = (uint32_t)m; r[3] = (uint32_t)(m >> 32);
    return 1;
}

/* __aeabi_uidivmod: unsigned quotient in r0, remainder in r1. */
static inline int aeabi_uidivmod(uint32_t *r) {
    if (!r[1]) return 0;
    uint32_t n = r[0], d = r[1];
    r[0] = n / d;
    r[1] = n % d;
    return 1;
}

/* __aeabi_d2iz: truncate toward zero; NaN gives 0 and out-of-range values saturate, as in the EABI. */
static void aeabi_d2iz(uint32_t *r) {
    uint64_t bits = double_bits(r[0], r[1]);
    double x;
    int32_t result;
    memcpy(&x, &bits, sizeof x);
    if (isnan(x)) result = 0;
    else if (x >= 2147483647.0) result = INT32_MAX;
    else if (x <= -2147483648.0) result = INT32_MIN;
    else result = (int32_t)x;
    r[0] = (uint32_t)result;
}

static int game_stdio(uint32_t *stack, uint32_t *r, unsigned ordinal) {
    if (ordinal == 140) {
        FILE *file = open_game_file(stack, r[0], r[1]);
        r[0] = file ? stream_slot(file) : 0;
        return 1;
    }
    uint32_t handle = ordinal == 144 ? r[3] : r[0];
    if (!handle || handle >= sizeof streams / sizeof streams[0] || !streams[handle]) return 0;
    FILE *file = streams[handle];
    switch (ordinal) {
    case 131: r[0] = (uint32_t)fclose(file); streams[handle] = NULL; return 1;
    case 144: {
        uint32_t dest = r[0], size = r[1], count = r[2], i = 0;
        if ((uint64_t)size * count > UINT32_MAX - dest) return 0;
        if (size) for (; i < count; i++) for (uint32_t j = 0; j < size; j++) {
            int c = fgetc(file);
            if (c == EOF) { r[0] = i; return 1; }
            guest_write_small(stack, dest++, (uint32_t)c, 1);
        }
        r[0] = i;
        return 1;
    }
    case 148:
        if (r[2] > 2) return 0;
        r[0] = (uint32_t)fseek(file, (int32_t)r[1], (int)r[2]);
        return 1;
    case 152: {
        long pos = ftell(file);
        r[0] = pos < 0 || pos > INT32_MAX ? UINT32_MAX : (uint32_t)pos;
        return 1;
    }
    default: return 0;
    }
}
