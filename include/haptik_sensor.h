#ifndef HAPTIK_SENSOR_H
#define HAPTIK_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double timestamp_seconds;
    float x_g;
    float y_g;
    float z_g;
} haptik_accel_sample_t;

typedef void (*haptik_sensor_callback_t)(
    const haptik_accel_sample_t *sample,
    void *context
);

typedef enum {
    HAPTIK_SENSOR_OK = 0,
    HAPTIK_SENSOR_NOT_FOUND,
    HAPTIK_SENSOR_PERMISSION_DENIED,
    HAPTIK_SENSOR_IO_ERROR,
    HAPTIK_SENSOR_THREAD_ERROR
} haptik_sensor_result_t;

typedef enum {
    HAPTIK_SENSOR_ACCESS_UNKNOWN = 0,
    HAPTIK_SENSOR_ACCESS_GRANTED,
    HAPTIK_SENSOR_ACCESS_DENIED
} haptik_sensor_access_t;

typedef struct haptik_sensor haptik_sensor_t;

bool haptik_sensor_available(void);
haptik_sensor_access_t haptik_sensor_check_access(void);
bool haptik_sensor_request_access(void);
haptik_sensor_t *haptik_sensor_create(
    haptik_sensor_callback_t callback,
    void *context
);
haptik_sensor_result_t haptik_sensor_start(haptik_sensor_t *sensor);
void haptik_sensor_stop(haptik_sensor_t *sensor);
void haptik_sensor_destroy(haptik_sensor_t *sensor);
const char *haptik_sensor_result_string(haptik_sensor_result_t result);

#ifdef __cplusplus
}
#endif

#endif
