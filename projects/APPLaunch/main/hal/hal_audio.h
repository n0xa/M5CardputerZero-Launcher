#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

void hal_audio_init(void);
void hal_audio_play(const char *path);
void hal_audio_play_sync(const char *path);
void hal_audio_stop(void);
void hal_audio_deinit(void);

/* Low-latency sampled "click" sounds.
 *   hal_audio_click_init(path) — preload a small WAV into memory, open a
 *     persistent ALSA/SDL_mixer output stream, start a background worker.
 *     Safe to call multiple times (no-op on subsequent calls).
 *   hal_audio_click_play() — non-blocking. Enqueues one playback. The
 *     worker mixes overlapping plays. Never forks. <1 ms to return.
 *   hal_audio_click_deinit() — stop worker, close stream.
 */
int  hal_audio_click_init(const char *wav_path);
void hal_audio_click_play(void);
void hal_audio_click_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
