#include "hal/hal_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <alsa/asoundlib.h>

void hal_audio_init(void) {}

/* ------------------------------------------------------------------ */
/*  hal_audio_play / play_sync: fork aplay/mpg123                      */
/*  Used for startup / shutdown (one-shot, latency doesn't matter)    */
/* ------------------------------------------------------------------ */
static void exec_player(const char *path)
{
    /* Child: silence + exec. Try multiple players. Extension-aware. */
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }

    const char *ext = strrchr(path, '.');
    int is_mp3 = (ext && (strcasecmp(ext, ".mp3") == 0));
    if (is_mp3) {
        execlp("mpg123", "mpg123", "-q", path, (char *)NULL);
        execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
               path, (char *)NULL);
    } else {
        execlp("aplay", "aplay", "-q", path, (char *)NULL);
    }
    _exit(127);
}

void hal_audio_play(const char *path)
{
    if (!path || access(path, F_OK) != 0) return;
    pid_t pid = fork();
    if (pid == 0) exec_player(path);
    /* Orphan reaping: double-fork would be cleaner, but here we rely on
     * the caller periodically waking up and waitpid is not done anywhere.
     * One-shot startup/shutdown -> low volume of zombies, acceptable. */
}

void hal_audio_play_sync(const char *path)
{
    if (!path || access(path, F_OK) != 0) return;
    pid_t pid = fork();
    if (pid == 0) exec_player(path);
    if (pid > 0) waitpid(pid, NULL, 0);
}

void hal_audio_stop(void) {}
void hal_audio_deinit(void) {}

/* ==================================================================== */
/*  hal_audio_click_*                                                    */
/*  Persistent ALSA PCM + preloaded WAV + background worker.             */
/*                                                                        */
/*  Design: one int16 mono PCM buffer lives in memory. An atomic counter  */
/*  tracks pending "fire" events. A worker thread blocks on a condvar,    */
/*  and when pending>0, writes the sample into an already-open ALSA PCM.  */
/*  If two events fire within the sample duration, the worker drops the   */
/*  newer one (simpler than mixing, the click is short enough it doesn't  */
/*  matter for UX).                                                       */
/* ==================================================================== */
struct wav_hdr {
    char     riff[4];
    uint32_t chunk_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t fmt_fmt;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} __attribute__((packed));

static snd_pcm_t *g_pcm = NULL;
static int16_t   *g_click_samples = NULL;
static size_t     g_click_frames  = 0;
static unsigned   g_click_rate    = 0;
static unsigned   g_click_channels = 0;

static pthread_t       g_worker;
static pthread_mutex_t g_worker_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_worker_cv  = PTHREAD_COND_INITIALIZER;
static atomic_int      g_pending    = 0;
static volatile int    g_worker_run = 0;

/* Load a RIFF WAV (PCM 16-bit only). Returns 0 on success. Caller owns
 * *samples (must free). */
static int load_wav_pcm16(const char *path, int16_t **samples, size_t *nframes,
                          unsigned *rate, unsigned *channels)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("[CLICK] fopen"); return -1; }

    struct wav_hdr h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) {
        fprintf(stderr, "[CLICK] short read on header\n");
        fclose(f); return -1;
    }
    if (memcmp(h.riff, "RIFF", 4) != 0 || memcmp(h.wave, "WAVE", 4) != 0) {
        fprintf(stderr, "[CLICK] not a RIFF/WAVE file\n");
        fclose(f); return -1;
    }
    if (h.fmt_fmt != 1 || h.bits_per_sample != 16) {
        fprintf(stderr, "[CLICK] only uncompressed 16-bit PCM supported (got fmt=%u bits=%u)\n",
                h.fmt_fmt, h.bits_per_sample);
        fclose(f); return -1;
    }
    if (h.fmt_size > 16)
        fseek(f, h.fmt_size - 16, SEEK_CUR);

    /* Scan for 'data' chunk */
    char chunk_id[4];
    uint32_t chunk_size = 0;
    while (1) {
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return -1; }
        if (fread(&chunk_size, 1, 4, f) != 4) { fclose(f); return -1; }
        if (memcmp(chunk_id, "data", 4) == 0) break;
        fseek(f, chunk_size, SEEK_CUR);
    }

    size_t bytes  = chunk_size;
    size_t frames = bytes / (h.channels * 2);
    int16_t *buf  = (int16_t *)malloc(bytes);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, bytes, f) != bytes) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    *samples  = buf;
    *nframes  = frames;
    *rate     = h.sample_rate;
    *channels = h.channels;
    return 0;
}

static int alsa_open(unsigned rate, unsigned channels)
{
    int err = snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "[CLICK] snd_pcm_open: %s\n", snd_strerror(err));
        g_pcm = NULL;
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);
    snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(g_pcm, hw, channels);
    unsigned exact_rate = rate;
    snd_pcm_hw_params_set_rate_near(g_pcm, hw, &exact_rate, 0);

    /* Small period for low latency on clicks. */
    snd_pcm_uframes_t period_frames = 256;
    snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &period_frames, 0);
    snd_pcm_uframes_t buffer_frames = 2048;
    snd_pcm_hw_params_set_buffer_size_near(g_pcm, hw, &buffer_frames);

    err = snd_pcm_hw_params(g_pcm, hw);
    if (err < 0) {
        fprintf(stderr, "[CLICK] snd_pcm_hw_params: %s\n", snd_strerror(err));
        snd_pcm_close(g_pcm); g_pcm = NULL;
        return -1;
    }
    snd_pcm_prepare(g_pcm);
    printf("[CLICK] ALSA ready: rate=%u channels=%u\n", exact_rate, channels);
    return 0;
}

static void *worker_fn(void *arg)
{
    (void)arg;
    while (g_worker_run) {
        pthread_mutex_lock(&g_worker_mu);
        while (g_worker_run && atomic_load(&g_pending) == 0)
            pthread_cond_wait(&g_worker_cv, &g_worker_mu);
        pthread_mutex_unlock(&g_worker_mu);
        if (!g_worker_run) break;

        /* Drain pending: coalesce multiple events into one playback. */
        int expected;
        do {
            expected = atomic_load(&g_pending);
        } while (!atomic_compare_exchange_weak(&g_pending, &expected, 0));

        if (!g_pcm || !g_click_samples) continue;

        snd_pcm_sframes_t written = snd_pcm_writei(g_pcm, g_click_samples,
                                                   g_click_frames);
        if (written == -EPIPE) {
            snd_pcm_prepare(g_pcm);
            snd_pcm_writei(g_pcm, g_click_samples, g_click_frames);
        } else if (written < 0) {
            fprintf(stderr, "[CLICK] snd_pcm_writei: %s\n",
                    snd_strerror((int)written));
        }
    }
    return NULL;
}

int hal_audio_click_init(const char *wav_path)
{
    if (g_click_samples) return 0;   /* already initialized */
    if (!wav_path) return -1;

    if (load_wav_pcm16(wav_path, &g_click_samples, &g_click_frames,
                       &g_click_rate, &g_click_channels) != 0) {
        return -1;
    }
    printf("[CLICK] loaded %s: %zu frames, %u Hz, %u ch\n",
           wav_path, g_click_frames, g_click_rate, g_click_channels);

    if (alsa_open(g_click_rate, g_click_channels) != 0) {
        free(g_click_samples);
        g_click_samples = NULL;
        return -1;
    }

    g_worker_run = 1;
    if (pthread_create(&g_worker, NULL, worker_fn, NULL) != 0) {
        perror("[CLICK] pthread_create");
        g_worker_run = 0;
        snd_pcm_close(g_pcm); g_pcm = NULL;
        free(g_click_samples); g_click_samples = NULL;
        return -1;
    }
    return 0;
}

void hal_audio_click_play(void)
{
    if (!g_worker_run) return;
    atomic_fetch_add(&g_pending, 1);
    pthread_cond_signal(&g_worker_cv);
}

void hal_audio_click_deinit(void)
{
    if (!g_worker_run) return;
    g_worker_run = 0;
    pthread_cond_signal(&g_worker_cv);
    pthread_join(g_worker, NULL);
    if (g_pcm) { snd_pcm_close(g_pcm); g_pcm = NULL; }
    free(g_click_samples); g_click_samples = NULL;
}
