#include "haptik_audio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <unistd.h>

#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3_ex.h"

enum {
    HAPTIK_AUDIO_QUEUE_CAPACITY = 128,
    HAPTIK_AUDIO_MAX_VOICES = 24,
    HAPTIK_AUDIO_SAMPLE_VARIANTS = 5,
    HAPTIK_AUDIO_SPECIAL_KEYS = 3
};

typedef struct {
    uint16_t key_code;
    float intensity;
    bool release;
} haptik_audio_event_t;

typedef struct {
    float *data;
    uint32_t frame_count;
} haptik_audio_sample_t;

typedef struct {
    bool active;
    uint32_t age_frames;
    const float *sample_data;
    uint32_t sample_frame_count;
    uint32_t sample_frame_index;
    float sample_gain;
    bool sample_mode;
} haptik_audio_voice_t;

struct haptik_audio {
    AudioUnit output_unit;
    double sample_rate;
    atomic_uint_fast32_t volume_millionths;
    atomic_uint_fast32_t write_index;
    atomic_uint_fast32_t read_index;
    haptik_audio_event_t queue[HAPTIK_AUDIO_QUEUE_CAPACITY];
    haptik_audio_voice_t voices[HAPTIK_AUDIO_MAX_VOICES];
    haptik_audio_sample_t down_samples[HAPTIK_AUDIO_SAMPLE_VARIANTS];
    haptik_audio_sample_t up_samples[HAPTIK_AUDIO_SAMPLE_VARIANTS];
    haptik_audio_sample_t special_down[HAPTIK_AUDIO_SPECIAL_KEYS];
    haptik_audio_sample_t special_up[HAPTIK_AUDIO_SPECIAL_KEYS];
    uint32_t down_variant_count;
    uint32_t up_variant_count;
    bool sample_pack_loaded;
    bool running;
    char last_error[128];
};

static float clamp01(float value) {
    if (value < 0.0F) {
        return 0.0F;
    }
    if (value > 1.0F) {
        return 1.0F;
    }
    return value;
}

static void set_osstatus_error(
    haptik_audio_t *audio,
    const char *operation,
    OSStatus status
) {
    const uint32_t code = (uint32_t)status;
    const char characters[5] = {
        (char)((code >> 24U) & 0xFFU),
        (char)((code >> 16U) & 0xFFU),
        (char)((code >> 8U) & 0xFFU),
        (char)(code & 0xFFU),
        '\0'
    };
    const bool printable =
        characters[0] >= 32 && characters[0] <= 126 &&
        characters[1] >= 32 && characters[1] <= 126 &&
        characters[2] >= 32 && characters[2] <= 126 &&
        characters[3] >= 32 && characters[3] <= 126;
    if (printable) {
        snprintf(
            audio->last_error,
            sizeof(audio->last_error),
            "%s failed ('%s')",
            operation,
            characters
        );
    } else {
        snprintf(
            audio->last_error,
            sizeof(audio->last_error),
            "%s failed (%d)",
            operation,
            (int)status
        );
    }
}

static void free_sample(haptik_audio_sample_t *sample) {
    free(sample->data);
    sample->data = NULL;
    sample->frame_count = 0;
}

static bool pcm16_to_float_sample(
    haptik_audio_t *audio,
    const int16_t *pcm,
    size_t pcm_samples,
    int channels,
    int sample_rate,
    haptik_audio_sample_t *sample
) {
    if (pcm == NULL || pcm_samples == 0 || channels <= 0 || sample_rate <= 0) {
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "Invalid PCM sample");
        return false;
    }
    const size_t source_frames = pcm_samples / (size_t)channels;
    const double ratio = audio->sample_rate / (double)sample_rate;
    const uint32_t output_frames =
        (uint32_t)ceil((double)source_frames * ratio);
    float *buffer = calloc(output_frames, sizeof(float));
    if (buffer == NULL) {
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "Could not allocate sample buffer");
        return false;
    }

    for (uint32_t output_index = 0;
         output_index < output_frames;
         ++output_index) {
        const double position = (double)output_index / ratio;
        size_t left = (size_t)position;
        if (left >= source_frames) {
            left = source_frames - 1U;
        }
        const size_t right = left + 1U < source_frames ? left + 1U : left;
        const float fraction = (float)(position - (double)left);
        float left_value = 0.0F;
        float right_value = 0.0F;
        for (int channel = 0; channel < channels; ++channel) {
            left_value += (float)pcm[left * (size_t)channels + (size_t)channel];
            right_value += (float)pcm[right * (size_t)channels + (size_t)channel];
        }
        const float scale = 1.0F / (32768.0F * (float)channels);
        buffer[output_index] =
            (left_value + (right_value - left_value) * fraction) * scale;
    }

    uint32_t first = 0;
    while (first + 1U < output_frames && fabsf(buffer[first]) < 0.0020F) {
        ++first;
    }
    first = first > 8U ? first - 8U : 0U;
    uint32_t last = output_frames;
    while (last > first + 1U && fabsf(buffer[last - 1U]) < 0.0008F) {
        --last;
    }
    const uint32_t trimmed_frames = last - first;
    if (first > 0U) {
        memmove(buffer, buffer + first, trimmed_frames * sizeof(float));
    }

    float peak = 0.0F;
    for (uint32_t index = 0; index < trimmed_frames; ++index) {
        peak = fmaxf(peak, fabsf(buffer[index]));
    }
    if (peak > 0.0001F) {
        const float normalization = fminf(4.0F, 0.82F / peak);
        for (uint32_t index = 0; index < trimmed_frames; ++index) {
            buffer[index] *= normalization;
        }
    }
    sample->data = buffer;
    sample->frame_count = trimmed_frames;
    return true;
}

static bool load_mp3_as_float(
    haptik_audio_t *audio,
    const char *path,
    haptik_audio_sample_t *sample
) {
    mp3dec_t decoder;
    mp3dec_file_info_t info = {0};
    mp3dec_init(&decoder);
    const int result = mp3dec_load(&decoder, path, &info, NULL, NULL);
    if (result != 0 || info.buffer == NULL || info.samples == 0 ||
        info.channels <= 0 || info.hz <= 0) {
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "MP3 decode failed (%d)", result);
        free(info.buffer);
        return false;
    }

    const bool loaded = pcm16_to_float_sample(
        audio,
        info.buffer,
        info.samples,
        info.channels,
        info.hz,
        sample
    );
    free(info.buffer);
    return loaded;
}

static uint16_t read_le16(const unsigned char *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U);
}

static uint32_t read_le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
}

static bool load_pcm_wav_as_float(
    haptik_audio_t *audio,
    const char *path,
    haptik_audio_sample_t *sample
) {
    FILE *file = fopen(path, "rb");
    unsigned char header[44];
    if (file == NULL || fread(header, 1, sizeof(header), file) != sizeof(header)) {
        if (file != NULL) {
            fclose(file);
        }
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "Could not read WAV file");
        return false;
    }
    if (memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0 ||
        memcmp(header + 12, "fmt ", 4) != 0 ||
        memcmp(header + 36, "data", 4) != 0 ||
        read_le16(header + 20) != 1U ||
        read_le16(header + 34) != 16U) {
        fclose(file);
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "Unsupported WAV format");
        return false;
    }
    const int channels = (int)read_le16(header + 22);
    const int sample_rate = (int)read_le32(header + 24);
    const uint32_t byte_count = read_le32(header + 40);
    int16_t *pcm = malloc(byte_count);
    if (pcm == NULL || fread(pcm, 1, byte_count, file) != byte_count) {
        free(pcm);
        fclose(file);
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "Could not read WAV samples");
        return false;
    }
    fclose(file);
    const bool loaded = pcm16_to_float_sample(
        audio,
        pcm,
        byte_count / sizeof(int16_t),
        channels,
        sample_rate,
        sample
    );
    free(pcm);
    return loaded;
}

static bool load_audio_as_float(
    haptik_audio_t *audio,
    const char *path,
    haptik_audio_sample_t *sample
) {
    const size_t path_length = strlen(path);
    if (path_length >= 4U &&
        strcasecmp(path + path_length - 4U, ".mp3") == 0) {
        return load_mp3_as_float(audio, path, sample);
    }
    if (path_length >= 4U &&
        strcasecmp(path + path_length - 4U, ".wav") == 0) {
        return load_pcm_wav_as_float(audio, path, sample);
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        (const UInt8 *)path,
        (CFIndex)strlen(path),
        false
    );
    if (url == NULL) {
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "Could not create audio file URL");
        return false;
    }

    ExtAudioFileRef file = NULL;
    OSStatus status = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (status != noErr || file == NULL) {
        set_osstatus_error(audio, "ExtAudioFileOpenURL", status);
        return false;
    }

    AudioStreamBasicDescription source_format = {0};
    UInt32 property_size = sizeof(source_format);
    status = ExtAudioFileGetProperty(
        file,
        kExtAudioFileProperty_FileDataFormat,
        &property_size,
        &source_format
    );

    SInt64 source_frames = 0;
    property_size = sizeof(source_frames);
    if (status == noErr) {
        status = ExtAudioFileGetProperty(
            file,
            kExtAudioFileProperty_FileLengthFrames,
            &property_size,
            &source_frames
        );
    }

    AudioStreamBasicDescription client_format = {
        .mSampleRate = audio->sample_rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked,
        .mBytesPerPacket = sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = sizeof(float),
        .mChannelsPerFrame = 1,
        .mBitsPerChannel = 8U * sizeof(float),
        .mReserved = 0
    };
    if (status == noErr) {
        status = ExtAudioFileSetProperty(
            file,
            kExtAudioFileProperty_ClientDataFormat,
            sizeof(client_format),
            &client_format
        );
    }
    if (status != noErr || source_frames <= 0 || source_format.mSampleRate <= 0) {
        set_osstatus_error(audio, "ExtAudioFile format", status);
        ExtAudioFileDispose(file);
        return false;
    }

    const double ratio = audio->sample_rate / source_format.mSampleRate;
    const uint32_t capacity =
        (uint32_t)ceil((double)source_frames * ratio) + 64U;
    float *buffer = calloc(capacity, sizeof(float));
    if (buffer == NULL) {
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "Could not allocate sample buffer");
        ExtAudioFileDispose(file);
        return false;
    }

    AudioBufferList output = {
        .mNumberBuffers = 1,
        .mBuffers = {{
            .mNumberChannels = 1,
            .mDataByteSize = capacity * sizeof(float),
            .mData = buffer
        }}
    };
    UInt32 frames = capacity;
    status = ExtAudioFileRead(file, &frames, &output);
    ExtAudioFileDispose(file);
    if (status != noErr || frames == 0) {
        set_osstatus_error(audio, "ExtAudioFileRead", status);
        free(buffer);
        return false;
    }

    uint32_t first = 0;
    while (first + 1U < frames && fabsf(buffer[first]) < 0.0020F) {
        ++first;
    }
    first = first > 8U ? first - 8U : 0U;
    uint32_t last = frames;
    while (last > first + 1U && fabsf(buffer[last - 1U]) < 0.0008F) {
        --last;
    }
    const uint32_t trimmed_frames = last - first;
    if (first > 0U) {
        memmove(buffer, buffer + first, trimmed_frames * sizeof(float));
    }

    float peak = 0.0F;
    for (uint32_t index = 0; index < trimmed_frames; ++index) {
        peak = fmaxf(peak, fabsf(buffer[index]));
    }
    if (peak > 0.0001F) {
        const float normalization = fminf(4.0F, 0.82F / peak);
        for (uint32_t index = 0; index < trimmed_frames; ++index) {
            buffer[index] *= normalization;
        }
    }

    sample->data = buffer;
    sample->frame_count = trimmed_frames;
    return true;
}

static void clear_sample_pack(haptik_audio_t *audio) {
    memset(audio->voices, 0, sizeof(audio->voices));
    const uint_fast32_t write = atomic_load_explicit(
        &audio->write_index,
        memory_order_relaxed
    );
    atomic_store_explicit(&audio->read_index, write, memory_order_relaxed);
    for (size_t index = 0; index < HAPTIK_AUDIO_SAMPLE_VARIANTS; ++index) {
        free_sample(&audio->down_samples[index]);
        free_sample(&audio->up_samples[index]);
    }
    for (size_t index = 0; index < HAPTIK_AUDIO_SPECIAL_KEYS; ++index) {
        free_sample(&audio->special_down[index]);
        free_sample(&audio->special_up[index]);
    }
    audio->down_variant_count = 0;
    audio->up_variant_count = 0;
    audio->sample_pack_loaded = false;
}

bool haptik_audio_load_wav_pack(
    haptik_audio_t *audio,
    const char *directory
) {
    if (audio == NULL || directory == NULL || audio->running) {
        return false;
    }

    clear_sample_pack(audio);

    char path[1024];
    for (size_t index = 0; index < HAPTIK_AUDIO_SAMPLE_VARIANTS; ++index) {
        snprintf(path, sizeof(path), "%s/down%zu.wav", directory, index + 1U);
        if (!load_audio_as_float(audio, path, &audio->down_samples[index])) {
            clear_sample_pack(audio);
            return false;
        }
        snprintf(path, sizeof(path), "%s/up%zu.wav", directory, index + 1U);
        if (!load_audio_as_float(audio, path, &audio->up_samples[index])) {
            clear_sample_pack(audio);
            return false;
        }
    }
    audio->down_variant_count = HAPTIK_AUDIO_SAMPLE_VARIANTS;
    audio->up_variant_count = HAPTIK_AUDIO_SAMPLE_VARIANTS;
    audio->sample_pack_loaded = true;
    audio->last_error[0] = '\0';
    return true;
}

bool haptik_audio_load_kbsim_pack(
    haptik_audio_t *audio,
    const char *directory
) {
    if (audio == NULL || directory == NULL || audio->running) {
        return false;
    }

    clear_sample_pack(audio);
    char path[1024];
    for (size_t index = 0; index < HAPTIK_AUDIO_SAMPLE_VARIANTS; ++index) {
        snprintf(
            path,
            sizeof(path),
            "%s/press/GENERIC_R%zu.mp3",
            directory,
            index
        );
        if (!load_audio_as_float(audio, path, &audio->down_samples[index])) {
            clear_sample_pack(audio);
            return false;
        }
    }
    snprintf(path, sizeof(path), "%s/release/GENERIC.mp3", directory);
    if (!load_audio_as_float(audio, path, &audio->up_samples[0])) {
        clear_sample_pack(audio);
        return false;
    }

    static const char *names[HAPTIK_AUDIO_SPECIAL_KEYS] = {
        "SPACE", "ENTER", "BACKSPACE"
    };
    for (size_t index = 0; index < HAPTIK_AUDIO_SPECIAL_KEYS; ++index) {
        snprintf(path, sizeof(path), "%s/press/%s.mp3", directory, names[index]);
        if (access(path, R_OK) == 0 &&
            !load_audio_as_float(audio, path, &audio->special_down[index])) {
            clear_sample_pack(audio);
            return false;
        }
        snprintf(path, sizeof(path), "%s/release/%s.mp3", directory, names[index]);
        if (access(path, R_OK) == 0 &&
            !load_audio_as_float(audio, path, &audio->special_up[index])) {
            clear_sample_pack(audio);
            return false;
        }
    }

    audio->down_variant_count = HAPTIK_AUDIO_SAMPLE_VARIANTS;
    audio->up_variant_count = 1;
    audio->sample_pack_loaded = true;
    audio->last_error[0] = '\0';
    return true;
}

static int special_key_index(uint16_t key_code) {
    switch (key_code) {
        case 49:
            return 0;
        case 36:
        case 76:
            return 1;
        case 51:
        case 117:
            return 2;
        default:
            return -1;
    }
}

static void start_voice(
    haptik_audio_t *audio,
    const haptik_audio_event_t *event
) {
    if (!audio->sample_pack_loaded) {
        return;
    }
    haptik_audio_voice_t *voice = NULL;
    for (size_t index = 0; index < HAPTIK_AUDIO_MAX_VOICES; ++index) {
        if (!audio->voices[index].active) {
            voice = &audio->voices[index];
            break;
        }
    }
    if (voice == NULL) {
        voice = &audio->voices[0];
        for (size_t index = 1; index < HAPTIK_AUDIO_MAX_VOICES; ++index) {
            if (audio->voices[index].age_frames > voice->age_frames) {
                voice = &audio->voices[index];
            }
        }
    }

    const float intensity = clamp01(event->intensity);
    {
        uint32_t variant = 0;
        if (event->release) {
            variant = event->key_code % audio->up_variant_count;
        } else {
            const int force_tier = (int)lrintf(intensity *
                (audio->down_variant_count - 1U));
            const int variation = (int)(event->key_code % 3U) - 1;
            int selected = force_tier + variation;
            if (selected < 0) {
                selected = 0;
            } else if ((uint32_t)selected >= audio->down_variant_count) {
                selected = (int)audio->down_variant_count - 1;
            }
            variant = (uint32_t)selected;
        }
        const haptik_audio_sample_t *sample = event->release
            ? &audio->up_samples[variant]
            : &audio->down_samples[variant];
        const int special = special_key_index(event->key_code);
        if (special >= 0) {
            const haptik_audio_sample_t *candidate = event->release
                ? &audio->special_up[special]
                : &audio->special_down[special];
            if (candidate->data != NULL) {
                sample = candidate;
            }
        }
        *voice = (haptik_audio_voice_t){
            .active = true,
            .sample_data = sample->data,
            .sample_frame_count = sample->frame_count,
            .sample_frame_index = 0,
            .sample_gain = event->release
                ? 0.38F
                : 0.22F + 0.78F * intensity,
            .sample_mode = true,
            .age_frames = 0
        };
        return;
    }
}

static void drain_events(haptik_audio_t *audio) {
    uint_fast32_t read = atomic_load_explicit(
        &audio->read_index,
        memory_order_relaxed
    );
    const uint_fast32_t write = atomic_load_explicit(
        &audio->write_index,
        memory_order_acquire
    );
    while (read != write) {
        start_voice(audio, &audio->queue[read]);
        read = (read + 1U) % HAPTIK_AUDIO_QUEUE_CAPACITY;
    }
    atomic_store_explicit(
        &audio->read_index,
        read,
        memory_order_release
    );
}

static float render_sample(haptik_audio_t *audio) {
    float mixed = 0.0F;
    for (size_t index = 0; index < HAPTIK_AUDIO_MAX_VOICES; ++index) {
        haptik_audio_voice_t *voice = &audio->voices[index];
        if (!voice->active) {
            continue;
        }

        if (voice->sample_frame_index < voice->sample_frame_count) {
            mixed += voice->sample_data[voice->sample_frame_index++] *
                voice->sample_gain;
            ++voice->age_frames;
        } else {
            voice->active = false;
        }
    }

    const float volume = (float)atomic_load_explicit(
        &audio->volume_millionths,
        memory_order_relaxed
    ) / 1000000.0F;
    mixed *= volume;
    return mixed / (1.0F + fabsf(mixed));
}

static OSStatus render_callback(
    void *context,
    AudioUnitRenderActionFlags *action_flags,
    const AudioTimeStamp *timestamp,
    UInt32 bus_number,
    UInt32 frame_count,
    AudioBufferList *buffers
) {
    (void)action_flags;
    (void)timestamp;
    (void)bus_number;
    haptik_audio_t *audio = context;
    drain_events(audio);

    if (buffers->mNumberBuffers == 1U) {
        float *output = buffers->mBuffers[0].mData;
        const UInt32 channels = buffers->mBuffers[0].mNumberChannels;
        for (UInt32 frame = 0; frame < frame_count; ++frame) {
            const float sample = render_sample(audio);
            for (UInt32 channel = 0; channel < channels; ++channel) {
                output[frame * channels + channel] = sample;
            }
        }
    } else {
        for (UInt32 frame = 0; frame < frame_count; ++frame) {
            const float sample = render_sample(audio);
            for (UInt32 buffer_index = 0;
                 buffer_index < buffers->mNumberBuffers;
                 ++buffer_index) {
                float *output = buffers->mBuffers[buffer_index].mData;
                output[frame] = sample;
            }
        }
    }
    return noErr;
}

haptik_audio_t *haptik_audio_create(void) {
    haptik_audio_t *audio = calloc(1, sizeof(*audio));
    if (audio == NULL) {
        return NULL;
    }
    audio->sample_rate = 48000.0;
    atomic_init(&audio->volume_millionths, 650000U);
    atomic_init(&audio->write_index, 0);
    atomic_init(&audio->read_index, 0);
    return audio;
}

bool haptik_audio_start(haptik_audio_t *audio) {
    if (audio == NULL) {
        return false;
    }
    if (audio->running) {
        return true;
    }

    AudioComponentDescription description = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0
    };
    AudioComponent component = AudioComponentFindNext(NULL, &description);
    if (component == NULL) {
        snprintf(audio->last_error, sizeof(audio->last_error),
                 "Default output audio component not found");
        return false;
    }

    OSStatus status = AudioComponentInstanceNew(component, &audio->output_unit);
    if (status != noErr) {
        set_osstatus_error(audio, "AudioComponentInstanceNew", status);
        return false;
    }

    AudioStreamBasicDescription format = {
        .mSampleRate = audio->sample_rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked,
        .mBytesPerPacket = 2U * sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 2U * sizeof(float),
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 8U * sizeof(float),
        .mReserved = 0
    };
    status = AudioUnitSetProperty(
        audio->output_unit,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input,
        0,
        &format,
        sizeof(format)
    );
    if (status != noErr) {
        set_osstatus_error(audio, "AudioUnitSetProperty(format)", status);
        haptik_audio_stop(audio);
        return false;
    }

    AURenderCallbackStruct callback = {
        .inputProc = render_callback,
        .inputProcRefCon = audio
    };
    status = AudioUnitSetProperty(
        audio->output_unit,
        kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input,
        0,
        &callback,
        sizeof(callback)
    );
    if (status != noErr) {
        set_osstatus_error(audio, "AudioUnitSetProperty(callback)", status);
        haptik_audio_stop(audio);
        return false;
    }

    status = AudioUnitInitialize(audio->output_unit);
    if (status == noErr) {
        status = AudioOutputUnitStart(audio->output_unit);
    }
    if (status != noErr) {
        set_osstatus_error(audio, "AudioOutputUnitStart", status);
        haptik_audio_stop(audio);
        return false;
    }

    audio->running = true;
    audio->last_error[0] = '\0';
    return true;
}

void haptik_audio_stop(haptik_audio_t *audio) {
    if (audio == NULL || audio->output_unit == NULL) {
        return;
    }
    if (audio->running) {
        AudioOutputUnitStop(audio->output_unit);
    }
    AudioUnitUninitialize(audio->output_unit);
    AudioComponentInstanceDispose(audio->output_unit);
    audio->output_unit = NULL;
    audio->running = false;
}

void haptik_audio_destroy(haptik_audio_t *audio) {
    if (audio == NULL) {
        return;
    }
    haptik_audio_stop(audio);
    clear_sample_pack(audio);
    free(audio);
}

void haptik_audio_set_volume(haptik_audio_t *audio, float volume) {
    if (audio == NULL) {
        return;
    }
    const uint_fast32_t value =
        (uint_fast32_t)lrintf(clamp01(volume) * 1000000.0F);
    atomic_store_explicit(
        &audio->volume_millionths,
        value,
        memory_order_relaxed
    );
}

void haptik_audio_trigger(
    haptik_audio_t *audio,
    uint16_t key_code,
    float intensity
) {
    if (audio == NULL || !audio->running) {
        return;
    }
    const uint_fast32_t write = atomic_load_explicit(
        &audio->write_index,
        memory_order_relaxed
    );
    const uint_fast32_t next =
        (write + 1U) % HAPTIK_AUDIO_QUEUE_CAPACITY;
    const uint_fast32_t read = atomic_load_explicit(
        &audio->read_index,
        memory_order_acquire
    );
    if (next == read) {
        return;
    }
    audio->queue[write] = (haptik_audio_event_t){
        .key_code = key_code,
        .intensity = clamp01(intensity),
        .release = false
    };
    atomic_store_explicit(
        &audio->write_index,
        next,
        memory_order_release
    );
}

void haptik_audio_trigger_release(
    haptik_audio_t *audio,
    uint16_t key_code
) {
    if (audio == NULL || !audio->running) {
        return;
    }
    const uint_fast32_t write = atomic_load_explicit(
        &audio->write_index,
        memory_order_relaxed
    );
    const uint_fast32_t next =
        (write + 1U) % HAPTIK_AUDIO_QUEUE_CAPACITY;
    const uint_fast32_t read = atomic_load_explicit(
        &audio->read_index,
        memory_order_acquire
    );
    if (next == read) {
        return;
    }
    audio->queue[write] = (haptik_audio_event_t){
        .key_code = key_code,
        .intensity = 0.32F,
        .release = true
    };
    atomic_store_explicit(
        &audio->write_index,
        next,
        memory_order_release
    );
}

const char *haptik_audio_last_error(const haptik_audio_t *audio) {
    if (audio == NULL) {
        return "audio engine unavailable";
    }
    return audio->last_error;
}
