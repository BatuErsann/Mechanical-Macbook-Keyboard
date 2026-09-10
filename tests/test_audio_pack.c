#include "haptik_audio.h"

#include <assert.h>
#include <stdio.h>

static void assert_pack_loaded(
    haptik_audio_t *audio,
    const char *directory
) {
    const bool loaded = haptik_audio_load_kbsim_pack(audio, directory);
    if (!loaded) {
        fprintf(stderr, "pack load failed (%s): %s\n",
            directory, haptik_audio_last_error(audio));
    }
    assert(loaded);
}

int main(void) {
    haptik_audio_t *audio = haptik_audio_create();
    assert(audio != NULL);
    const bool loaded = haptik_audio_load_wav_pack(
        audio,
        "resources/sounds/kailh_white"
    );
    if (!loaded) {
        fprintf(stderr, "pack load failed: %s\n", haptik_audio_last_error(audio));
    }
    assert(loaded);

    static const char *pack_ids[] = {
        "alpaca", "blackink", "bluealps", "boxnavy", "buckling",
        "cream", "holypanda", "mxblack", "mxblue", "mxbrown",
        "redink", "topre", "turquoise"
    };
    char path[256];
    for (size_t index = 0; index < sizeof(pack_ids) / sizeof(pack_ids[0]); ++index) {
        snprintf(path, sizeof(path), "resources/sounds/kbsim/%s", pack_ids[index]);
        assert_pack_loaded(audio, path);
    }
    haptik_audio_destroy(audio);
    puts("audio pack tests passed");
    return 0;
}
