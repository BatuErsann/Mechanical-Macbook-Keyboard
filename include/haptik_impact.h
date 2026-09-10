#ifndef HAPTIK_IMPACT_H
#define HAPTIK_IMPACT_H

#include "haptik_sensor.h"

#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float gravity_x;
    float gravity_y;
    float gravity_z;
    float noise_floor;
    float envelope;
    bool initialized;
    atomic_uint_fast32_t sensitivity_millionths;
    atomic_uint_fast32_t published_intensity;
    atomic_uint_fast64_t published_timestamp_us;
} haptik_impact_detector_t;

void haptik_impact_init(haptik_impact_detector_t *detector);
void haptik_impact_set_sensitivity(
    haptik_impact_detector_t *detector,
    float sensitivity
);
void haptik_impact_push_sample(
    haptik_impact_detector_t *detector,
    const haptik_accel_sample_t *sample
);
float haptik_impact_intensity_at(
    const haptik_impact_detector_t *detector,
    double event_timestamp_seconds
);

#ifdef __cplusplus
}
#endif

#endif
