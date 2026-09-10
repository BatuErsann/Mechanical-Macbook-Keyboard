#include "haptik_sensor.h"

#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    atomic_uint_fast64_t sample_count;
    atomic_uint_fast32_t peak_milligal;
} probe_state_t;

static atomic_bool keep_running = true;

static void stop_probe(int signal_number) {
    (void)signal_number;
    atomic_store(&keep_running, false);
}

static void on_sample(
    const haptik_accel_sample_t *sample,
    void *context
) {
    probe_state_t *state = context;
    const float magnitude = sqrtf(
        sample->x_g * sample->x_g +
        sample->y_g * sample->y_g +
        sample->z_g * sample->z_g
    );
    const float dynamic = fabsf(magnitude - 1.0F);
    const uint_fast32_t milligal = (uint_fast32_t)(dynamic * 1000.0F);

    atomic_fetch_add(&state->sample_count, 1);
    uint_fast32_t old_peak = atomic_load(&state->peak_milligal);
    while (milligal > old_peak && !atomic_compare_exchange_weak(
        &state->peak_milligal,
        &old_peak,
        milligal
    )) {
    }
}

int main(void) {
    signal(SIGINT, stop_probe);
    signal(SIGTERM, stop_probe);

    printf("Haptik native accelerometer probe\n");
    printf("Device present: %s\n", haptik_sensor_available() ? "yes" : "no");
    const haptik_sensor_access_t access = haptik_sensor_check_access();
    const char *access_name = access == HAPTIK_SENSOR_ACCESS_GRANTED
        ? "granted"
        : access == HAPTIK_SENSOR_ACCESS_DENIED ? "denied" : "unknown";
    printf("HID listen access: %s\n", access_name);
    fflush(stdout);

    probe_state_t state;
    atomic_init(&state.sample_count, 0);
    atomic_init(&state.peak_milligal, 0);

    haptik_sensor_t *sensor = haptik_sensor_create(on_sample, &state);
    if (sensor == NULL) {
        fprintf(stderr, "Could not allocate sensor reader.\n");
        return 1;
    }

    const haptik_sensor_result_t result = haptik_sensor_start(sensor);
    if (result != HAPTIK_SENSOR_OK) {
        fprintf(
            stderr,
            "Could not start sensor: %s\n",
            haptik_sensor_result_string(result)
        );
        if (result == HAPTIK_SENSOR_PERMISSION_DENIED) {
            fprintf(
                stderr,
                "This undocumented HID path may require elevated or TCC access.\n"
            );
        }
        haptik_sensor_destroy(sensor);
        return 2;
    }

    printf("Streaming for 5 seconds. Tap or type on the MacBook...\n");
    uint_fast64_t previous_count = 0;
    for (int second = 0; second < 5 && atomic_load(&keep_running); ++second) {
        struct timespec duration = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&duration, NULL);
        const uint_fast64_t current_count = atomic_load(&state.sample_count);
        const uint_fast32_t peak = atomic_exchange(&state.peak_milligal, 0);
        printf(
            "%d s: %llu samples/s, peak %.3f g\n",
            second + 1,
            (unsigned long long)(current_count - previous_count),
            (double)peak / 1000.0
        );
        previous_count = current_count;
    }

    haptik_sensor_destroy(sensor);
    return 0;
}
