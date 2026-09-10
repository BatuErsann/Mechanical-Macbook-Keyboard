#ifndef HAPTIK_AUDIO_H
#define HAPTIK_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct haptik_audio haptik_audio_t;

haptik_audio_t *haptik_audio_create(void);
bool haptik_audio_load_wav_pack(
    haptik_audio_t *audio,
    const char *directory
);
bool haptik_audio_load_kbsim_pack(
    haptik_audio_t *audio,
    const char *directory
);
bool haptik_audio_start(haptik_audio_t *audio);
void haptik_audio_stop(haptik_audio_t *audio);
void haptik_audio_destroy(haptik_audio_t *audio);
void haptik_audio_set_volume(haptik_audio_t *audio, float volume);
void haptik_audio_trigger(
    haptik_audio_t *audio,
    uint16_t key_code,
    float intensity
);
void haptik_audio_trigger_release(
    haptik_audio_t *audio,
    uint16_t key_code
);
const char *haptik_audio_last_error(const haptik_audio_t *audio);

#ifdef __cplusplus
}
#endif

#endif
