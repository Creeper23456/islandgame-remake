/* CMdaAudioOutputStream ABI used by the game's mixer. SDL owns the queued
   PCM copy; callbacks run on the guest worker, never on an SDL thread. */
#define AUDIO_BASE UINT32_C(0xff00a000)
static struct {
    struct guest_thread *owner;
    uint32_t object, callback, buffer, chunk;
    SDL_AudioDeviceID device;
    int volume, open_pending, playing, error;
} audio;

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
    } else if (audio.buffer && SDL_GetQueuedAudioSize(audio.device) <= audio.chunk) {
        /* Keep about one mixer block queued while letting the guest refill. */
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
