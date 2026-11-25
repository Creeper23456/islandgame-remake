/* CMdaAudioOutputStream ABI used by the game's mixer. SDL owns the queued
   PCM copy; callbacks run on the guest worker, never on an SDL thread.

   Pacing: the guest mixer produces one 800-sample block (50 ms) each time its buffer-copied
   callback runs, and the callback only runs when the host scheduler gives the mixer thread a
   pass. audio_wake_us() tells the main loop when the next pass is needed, so a main loop that
   would otherwise sleep until the next game timer cannot starve the mixer. */
#define AUDIO_BASE UINT32_C(0xff00a000)
/* 16 kHz mono PCM16 is 32000 bytes/s. The guest is acknowledged once the queue has room for
   another block, so the device holds between target-chunk and target bytes of audio: 150 ms
   by default (ISLAND_AUDIO_TARGET_MS, 100..400) so one slow frame does not drain it. */
#define AUDIO_BYTES_PER_SEC 32000u
#define AUDIO_TARGET_DEFAULT_MS 150u
/* A pass is requested at most this often while the mixer does not make progress. */
#define AUDIO_WAKE_MIN_US 2000u
static struct {
    struct guest_thread *owner;
    uint32_t object, callback, buffer, chunk, target;
    SDL_AudioDeviceID device;
    int volume, open_pending, playing, error;
    uint64_t drain_end_us, last_wake_us, gap_us, report_us;
    unsigned gaps, blocks;
} audio;

static uint64_t audio_now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

/* Wall time at which the queued PCM runs out is tracked analytically (the device drains at a
   fixed rate), so a block written after that point is a measured gap in the sound, whichever
   order the guest callbacks arrived in. */
static void audio_account_write(uint32_t length) {
    uint64_t now = audio_now_us();
    if (audio.drain_end_us && now > audio.drain_end_us + 2000) {
        audio.gap_us += now - audio.drain_end_us;
        if (++audio.gaps <= 8 || (audio.gaps & (audio.gaps - 1)) == 0)
            fprintf(stderr, "audio: gap #%u of %llu ms (total %llu ms)\n", audio.gaps,
                    (unsigned long long)((now - audio.drain_end_us) / 1000),
                    (unsigned long long)(audio.gap_us / 1000));
    }
    if (!audio.drain_end_us || now > audio.drain_end_us) audio.drain_end_us = now;
    audio.drain_end_us += (uint64_t)length * 1000000 / AUDIO_BYTES_PER_SEC;
    audio.blocks++;
    if (!audio.report_us) audio.report_us = now;
    if (now - audio.report_us >= 10000000) {
        fprintf(stderr, "audio: %u blocks in %llu ms, %u gaps (%llu ms)\n", audio.blocks,
                (unsigned long long)((now - audio.report_us) / 1000), audio.gaps,
                (unsigned long long)(audio.gap_us / 1000));
        audio.report_us = now;
        audio.blocks = 0;
    }
}

/* Absolute CLOCK_MONOTONIC microseconds at which the mixer needs a scheduler pass, or
   UINT64_MAX when it is idle. */
static uint64_t audio_wake_us(uint64_t now) {
    if (!audio.device) return UINT64_MAX;
    uint64_t at;
    if (audio.open_pending || (audio.playing && !audio.buffer)) {
        at = now;
    } else if (audio.buffer) {
        uint32_t queued = SDL_GetQueuedAudioSize(audio.device);
        uint32_t threshold = audio.target > audio.chunk ? audio.target - audio.chunk : 0;
        at = queued <= threshold ? now : now + (uint64_t)(queued - threshold) * 1000000 / AUDIO_BYTES_PER_SEC;
    } else return UINT64_MAX;
    /* Bounded retry rate if the mixer thread cannot run. */
    return at < audio.last_wake_us + AUDIO_WAKE_MIN_US ? audio.last_wake_us + AUDIO_WAKE_MIN_US : at;
}

static uint32_t audio_new(uint32_t *stack, uint32_t callback, uint32_t server) {
    if (!current_worker || !callback || server || audio.object) return 0;
    uint32_t object = heap_try_alloc(0x30);
    if (!object) return 0;
    guest_write(stack, object, object + 4);
    for (unsigned slot = 0; slot <= 0x28; slot += 4)
        guest_write(stack, object + 4 + slot, AUDIO_BASE + slot);
    audio.object = object;
    audio.callback = callback;
    audio.owner = current_worker;
    return object;
}

static void audio_destroy(void) {
    if (audio.device) SDL_CloseAudioDevice(audio.device);
    memset(&audio, 0, sizeof audio);
}

static void audio_set_volume(int volume) {
    audio.volume = SDL_clamp(volume, 0, SDL_MIX_MAXVOLUME);
}

static void audio_open(uint32_t *stack, uint32_t settings) {
    if (audio.device || audio.open_pending) return;
    /* TMdaAudioDataSettings: guest requests 16000 Hz, mono, PCM16. */
    audio.error = -5; /* KErrNotSupported */
    audio.open_pending = 1;
    if (!settings || guest_read(stack, settings + 0x0c) != 0x10003949 ||
        guest_read(stack, settings + 0x1c) != 0x100 || guest_read(stack, settings + 0x20) != 0x02000000) {
        fprintf(stderr, "CMdaAudioOutputStream::Open: unsupported settings at 0x%08x\n", settings);
        return;
    }
    audio_set_volume((int32_t)guest_read(stack, settings + 0x24));
    SDL_AudioSpec spec = { .freq = 16000, .format = AUDIO_S16LSB, .channels = 1, .samples = 512 };
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) audio.device = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (!audio.device) {
        fprintf(stderr, "CMdaAudioOutputStream::Open: %s\n", SDL_GetError());
        audio.error = -2; /* KErrGeneral */
        return;
    }
    audio.error = 0;
    audio.target = AUDIO_BYTES_PER_SEC * AUDIO_TARGET_DEFAULT_MS / 1000;
    const char *latency = getenv("ISLAND_AUDIO_TARGET_MS");
    if (latency && *latency) {
        char *end = NULL;
        long ms = strtol(latency, &end, 10);
        if (!*end && ms >= 100 && ms <= 400) audio.target = (uint32_t)(AUDIO_BYTES_PER_SEC * ms / 1000);
        else fprintf(stderr, "audio: ISLAND_AUDIO_TARGET_MS=%s is invalid (want 100..400), using %u\n", latency,
                     AUDIO_TARGET_DEFAULT_MS);
    }
    SDL_PauseAudioDevice(audio.device, 0);
}

static int audio_write(uint32_t *stack, uint32_t buffer) {
    uint32_t length, data = text_data(stack, buffer, &length);
    if (!audio.device || audio.open_pending || audio.buffer) return -18; /* KErrNotReady */
    const unsigned char *pcm = heap_word(data);
    if (!pcm || !length || (length & 1) || length > thread_heap.used - (data - heap_base)) return -6;
    if (audio.volume == 128) {
        if (SDL_QueueAudio(audio.device, pcm, length)) return -2;
    } else {
        /* The guest mixer submits at most 800 signed 16-bit samples per write. */
        unsigned char scaled[1600];
        if (length > sizeof scaled) return -6;
        SDL_memset(scaled, 0, length);
        SDL_MixAudioFormat(scaled, pcm, AUDIO_S16LSB, length, audio.volume);
        if (SDL_QueueAudio(audio.device, scaled, length)) return -2;
    }
    audio_account_write(length);
    audio.buffer = buffer;
    audio.chunk = length;
    audio.playing = 1;
    return 0;
}

static int audio_callback(uint32_t *stack, uint32_t sp, uint32_t slot, int error, uint32_t buffer) {
    struct guest_thread nested = {0};
    nested.target = guest_read(stack, guest_read(stack, audio.callback) + slot);
    nested.r[0] = audio.callback;
    nested.r[1] = (uint32_t)error;
    nested.r[2] = buffer;
    nested.r[13] = sp;
    nested.r[14] = GUEST_THREAD_SENTINEL;
    return guest_run(&nested, stack) == 0;
}

static int audio_stop(uint32_t *stack, uint32_t sp) {
    uint32_t buffer = audio.buffer;
    int was_playing = audio.playing;
    if (audio.device) SDL_ClearQueuedAudio(audio.device);
    audio.buffer = audio.playing = 0;
    audio.drain_end_us = 0; /* a flush is not a gap */
    /* The SDK flushes pending writes and reports cancellation synchronously. */
    if (buffer && !audio_callback(stack, sp, 4, -39, buffer)) return 0; /* KErrAbort */
    return !was_playing || audio_callback(stack, sp, 8, -3, 0); /* KErrCancel */
}

static int audio_dispatch(uint32_t *stack, struct guest_thread *owner) {
    if (audio.owner != owner) return 1;
    uint32_t slot, buffer = 0;
    int error;
    if (audio.open_pending) {
        audio.open_pending = 0;
        slot = 0;
        error = audio.error;
    } else if (audio.buffer && SDL_GetQueuedAudioSize(audio.device) + audio.chunk <= audio.target) {
        /* Report the previous block as consumed as soon as the queue has room, so the
           guest mixer keeps audio.target bytes of audio ahead of the device. */
        buffer = audio.buffer;
        audio.buffer = 0;
        slot = 4;
        error = 0;
    } else if (audio.playing && !audio.buffer && !SDL_GetQueuedAudioSize(audio.device)) {
        slot = 8;
        error = -10; /* KErrUnderflow */
        audio.playing = 0;
    } else return 1;
    return audio_callback(stack, owner->r[13], slot, error, buffer);
}
