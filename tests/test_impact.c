#include "haptik_impact.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    haptik_impact_detector_t detector;
    haptik_impact_init(&detector);

    for (int index = 0; index < 1500; ++index) {
        haptik_accel_sample_t sample = {
            .timestamp_seconds = (double)index / 1000.0,
            .x_g = 0.0F,
            .y_g = 0.0F,
            .z_g = -1.0F
        };
        haptik_impact_push_sample(&detector, &sample);
    }
    const float quiet = haptik_impact_intensity_at(&detector, 1.499);
    assert(quiet <= 0.10F);

    haptik_accel_sample_t hit = {
        .timestamp_seconds = 1.500,
        .x_g = 0.08F,
        .y_g = 0.01F,
        .z_g = -1.0F
    };
    haptik_impact_push_sample(&detector, &hit);
    const float hard = haptik_impact_intensity_at(&detector, 1.503);
    assert(hard > 0.75F);

    const float stale = haptik_impact_intensity_at(&detector, 1.600);
    assert(stale > 0.15F && stale < 0.20F);

    puts("impact detector tests passed");
    return 0;
}
