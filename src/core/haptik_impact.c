#include "haptik_impact.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static float clamp01(float value) {
    if (value < 0.0F) {
        return 0.0F;
    }
    if (value > 1.0F) {
        return 1.0F;
    }
    return value;
}

void haptik_impact_init(haptik_impact_detector_t *detector) {
    if (detector == NULL) {
        return;
    }
    memset(detector, 0, sizeof(*detector));
    detector->noise_floor = 0.0015F;
    atomic_init(&detector->sensitivity_millionths, 1000000U);
    atomic_init(&detector->published_intensity, 0);
    atomic_init(&detector->published_timestamp_us, 0);
}

void haptik_impact_set_sensitivity(
    haptik_impact_detector_t *detector,
    float sensitivity
) {
    if (detector == NULL) {
        return;
    }
    if (sensitivity < 0.35F) {
        sensitivity = 0.35F;
    } else if (sensitivity > 3.0F) {
        sensitivity = 3.0F;
    }
    atomic_store_explicit(
        &detector->sensitivity_millionths,
        (uint_fast32_t)lrintf(sensitivity * 1000000.0F),
        memory_order_relaxed
    );
}

void haptik_impact_push_sample(
    haptik_impact_detector_t *detector,
    const haptik_accel_sample_t *sample
) {
    if (detector == NULL || sample == NULL) {
        return;
    }

    if (!detector->initialized) {
        detector->gravity_x = sample->x_g;
        detector->gravity_y = sample->y_g;
        detector->gravity_z = sample->z_g;
        detector->initialized = true;
    }

    /* Roughly a 0.5 s gravity estimate at the native ~1 kHz rate. */
    const float gravity_alpha = 0.002F;
    detector->gravity_x += gravity_alpha *
        (sample->x_g - detector->gravity_x);
    detector->gravity_y += gravity_alpha *
        (sample->y_g - detector->gravity_y);
    detector->gravity_z += gravity_alpha *
        (sample->z_g - detector->gravity_z);

    const float dynamic_x = sample->x_g - detector->gravity_x;
    const float dynamic_y = sample->y_g - detector->gravity_y;
    const float dynamic_z = sample->z_g - detector->gravity_z;
    const float magnitude = sqrtf(
        dynamic_x * dynamic_x +
        dynamic_y * dynamic_y +
        dynamic_z * dynamic_z
    );

    /* Track quiet chassis noise slowly, but do not absorb sharp impacts. */
    if (magnitude < detector->noise_floor * 3.0F) {
        detector->noise_floor += 0.002F *
            (magnitude - detector->noise_floor);
    }
    if (detector->noise_floor < 0.0005F) {
        detector->noise_floor = 0.0005F;
    }

    const float cleaned = fmaxf(
        0.0F,
        magnitude - detector->noise_floor * 1.35F
    );
    detector->envelope = fmaxf(cleaned, detector->envelope * 0.94F);

    /* 0.004 g is a soft key; around 0.07 g is a hard chassis hit. */
    const float sensitivity = (float)atomic_load_explicit(
        &detector->sensitivity_millionths,
        memory_order_relaxed
    ) / 1000000.0F;
    const float normalized = clamp01(
        (detector->envelope * sensitivity - 0.0035F) /
        0.0665F
    );
    const float shaped = sqrtf(normalized);
    const uint_fast32_t quantized =
        (uint_fast32_t)lrintf(shaped * 1000000.0F);
    const uint_fast64_t timestamp_us =
        (uint_fast64_t)llrint(sample->timestamp_seconds * 1000000.0);

    atomic_store_explicit(
        &detector->published_intensity,
        quantized,
        memory_order_relaxed
    );
    atomic_store_explicit(
        &detector->published_timestamp_us,
        timestamp_us,
        memory_order_release
    );
}

float haptik_impact_intensity_at(
    const haptik_impact_detector_t *detector,
    double event_timestamp_seconds
) {
    if (detector == NULL) {
        return 0.18F;
    }

    const uint_fast64_t timestamp_us = atomic_load_explicit(
        &detector->published_timestamp_us,
        memory_order_acquire
    );
    const uint_fast32_t quantized = atomic_load_explicit(
        &detector->published_intensity,
        memory_order_relaxed
    );
    const uint_fast64_t event_us =
        (uint_fast64_t)llrint(event_timestamp_seconds * 1000000.0);

    /* KeyDown normally trails chassis acceleration by a few milliseconds. */
    const uint_fast64_t age_us = event_us >= timestamp_us
        ? event_us - timestamp_us
        : timestamp_us - event_us;
    if (timestamp_us == 0 || age_us > 30000U) {
        return 0.18F;
    }

    const float intensity = (float)quantized / 1000000.0F;
    return fmaxf(0.08F, clamp01(intensity));
}
