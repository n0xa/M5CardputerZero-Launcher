#include "hal/hal_audio.h"
#include <stdio.h>

#ifdef EMU_HAS_AUDIO
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

static int g_audio_ready = 0;
static Mix_Music *g_music = NULL;
static Mix_Chunk *g_click_chunk = NULL;

void hal_audio_init(void)
{
    if (g_audio_ready) return;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0)
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) == 0) {
        g_audio_ready = 1;
    } else {
        fprintf(stderr, "[HAL] audio init failed: %s\n", Mix_GetError());
    }
}

void hal_audio_play(const char *path)
{
    if (!g_audio_ready) hal_audio_init();
    if (!g_audio_ready) return;
    if (g_music) {
        Mix_HaltMusic();
        Mix_FreeMusic(g_music);
        g_music = NULL;
    }
    g_music = Mix_LoadMUS(path);
    if (g_music) {
        Mix_PlayMusic(g_music, 1);
        printf("[HAL] Playing: %s\n", path);
    } else {
        fprintf(stderr, "[HAL] Failed to load %s: %s\n", path, Mix_GetError());
    }
}

void hal_audio_stop(void)
{
    if (g_audio_ready) Mix_HaltMusic();
    if (g_music) {
        Mix_FreeMusic(g_music);
        g_music = NULL;
    }
}

void hal_audio_deinit(void)
{
    hal_audio_stop();
    if (g_audio_ready) {
        Mix_CloseAudio();
        g_audio_ready = 0;
    }
}

void hal_audio_play_sync(const char *path)
{
    if (!g_audio_ready) hal_audio_init();
    if (!g_audio_ready) return;
    if (g_music) {
        Mix_HaltMusic();
        Mix_FreeMusic(g_music);
        g_music = NULL;
    }
    g_music = Mix_LoadMUS(path);
    if (g_music) {
        Mix_PlayMusic(g_music, 1);
        printf("[HAL] Playing (sync): %s\n", path);
        while (Mix_PlayingMusic()) SDL_Delay(50);
        Mix_FreeMusic(g_music);
        g_music = NULL;
    }
}

int hal_audio_click_init(const char *wav_path)
{
    if (!g_audio_ready) hal_audio_init();
    if (!g_audio_ready) return -1;
    if (g_click_chunk) return 0;
    g_click_chunk = Mix_LoadWAV(wav_path);
    if (!g_click_chunk) {
        fprintf(stderr, "[CLICK] LoadWAV %s: %s\n", wav_path, Mix_GetError());
        return -1;
    }
    Mix_AllocateChannels(8);   /* allow 8 overlapping clicks */
    printf("[CLICK] loaded %s (SDL_mixer)\n", wav_path);
    return 0;
}

void hal_audio_click_play(void)
{
    if (!g_click_chunk) return;
    Mix_PlayChannel(-1, g_click_chunk, 0);
}

void hal_audio_click_deinit(void)
{
    if (g_click_chunk) { Mix_FreeChunk(g_click_chunk); g_click_chunk = NULL; }
}

#else

void hal_audio_init(void) {}
void hal_audio_play(const char *path) { (void)path; }
void hal_audio_play_sync(const char *path) { (void)path; }
void hal_audio_stop(void) {}
void hal_audio_deinit(void) {}
int  hal_audio_click_init(const char *p) { (void)p; return -1; }
void hal_audio_click_play(void) {}
void hal_audio_click_deinit(void) {}

#endif
