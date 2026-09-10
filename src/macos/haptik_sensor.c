#include "haptik_sensor.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    HAPTIK_VENDOR_USAGE_PAGE = 0xFF00,
    HAPTIK_ACCEL_USAGE = 3,
    HAPTIK_REPORT_LENGTH = 22,
    HAPTIK_DATA_OFFSET = 6,
    HAPTIK_REPORT_BUFFER_SIZE = 4096,
    HAPTIK_REPORT_INTERVAL_US = 1000
};

static const double HAPTIK_Q16_SCALE = 65536.0;

struct haptik_sensor {
    haptik_sensor_callback_t callback;
    void *callback_context;
    pthread_t thread;
    pthread_mutex_t state_mutex;
    pthread_cond_t state_condition;
    bool thread_created;
    bool startup_complete;
    atomic_bool stop_requested;
    haptik_sensor_result_t startup_result;
    CFRunLoopRef run_loop;
    IOHIDDeviceRef device;
    uint8_t report_buffer[HAPTIK_REPORT_BUFFER_SIZE];
    mach_timebase_info_data_t timebase;
};

static int64_t integer_property(io_registry_entry_t service, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service,
        key,
        kCFAllocatorDefault,
        0
    );
    if (value == NULL) {
        return 0;
    }

    int64_t result = 0;
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        (void)CFNumberGetValue(
            (CFNumberRef)value,
            kCFNumberSInt64Type,
            &result
        );
    }
    CFRelease(value);
    return result;
}

static bool wake_sensor_drivers(void) {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDriver");
    if (matching == NULL) {
        return false;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(
            kIOMainPortDefault,
            matching,
            &iterator
        ) != KERN_SUCCESS) {
        return false;
    }

    const int32_t one = 1;
    const int32_t interval = HAPTIK_REPORT_INTERVAL_US;
    CFNumberRef one_number = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberSInt32Type,
        &one
    );
    CFNumberRef interval_number = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberSInt32Type,
        &interval
    );

    bool found = false;
    io_service_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        found = true;
        if (one_number != NULL) {
            (void)IORegistryEntrySetCFProperty(
                service,
                CFSTR("SensorPropertyReportingState"),
                one_number
            );
            (void)IORegistryEntrySetCFProperty(
                service,
                CFSTR("SensorPropertyPowerState"),
                one_number
            );
        }
        if (interval_number != NULL) {
            (void)IORegistryEntrySetCFProperty(
                service,
                CFSTR("ReportInterval"),
                interval_number
            );
        }
        IOObjectRelease(service);
    }

    if (one_number != NULL) {
        CFRelease(one_number);
    }
    if (interval_number != NULL) {
        CFRelease(interval_number);
    }
    IOObjectRelease(iterator);
    return found;
}

static io_service_t copy_accelerometer_service(void) {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDevice");
    if (matching == NULL) {
        return IO_OBJECT_NULL;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(
            kIOMainPortDefault,
            matching,
            &iterator
        ) != KERN_SUCCESS) {
        return IO_OBJECT_NULL;
    }

    io_service_t accelerometer = IO_OBJECT_NULL;
    io_service_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        const int64_t usage_page = integer_property(
            service,
            CFSTR("PrimaryUsagePage")
        );
        const int64_t usage = integer_property(
            service,
            CFSTR("PrimaryUsage")
        );
        if (usage_page == HAPTIK_VENDOR_USAGE_PAGE &&
            usage == HAPTIK_ACCEL_USAGE) {
            accelerometer = service;
            break;
        }
        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);
    return accelerometer;
}

bool haptik_sensor_available(void) {
    io_service_t service = copy_accelerometer_service();
    if (service == IO_OBJECT_NULL) {
        return false;
    }
    IOObjectRelease(service);
    return true;
}

haptik_sensor_access_t haptik_sensor_check_access(void) {
    switch (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)) {
        case kIOHIDAccessTypeGranted:
            return HAPTIK_SENSOR_ACCESS_GRANTED;
        case kIOHIDAccessTypeDenied:
            return HAPTIK_SENSOR_ACCESS_DENIED;
        case kIOHIDAccessTypeUnknown:
            return HAPTIK_SENSOR_ACCESS_UNKNOWN;
    }
    return HAPTIK_SENSOR_ACCESS_UNKNOWN;
}

bool haptik_sensor_request_access(void) {
    return IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
}

static int32_t little_endian_i32(const uint8_t *bytes) {
    uint32_t value =
        ((uint32_t)bytes[0]) |
        ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
    int32_t signed_value = 0;
    memcpy(&signed_value, &value, sizeof(signed_value));
    return signed_value;
}

static void report_callback(
    void *context,
    IOReturn result,
    void *sender,
    IOHIDReportType type,
    uint32_t report_id,
    uint8_t *report,
    CFIndex report_length,
    uint64_t timestamp
) {
    (void)sender;
    (void)type;
    (void)report_id;

    haptik_sensor_t *sensor = context;
    if (sensor == NULL || result != kIOReturnSuccess ||
        report == NULL || report_length != HAPTIK_REPORT_LENGTH ||
        atomic_load_explicit(&sensor->stop_requested, memory_order_relaxed)) {
        return;
    }

    const uint8_t *payload = report + HAPTIK_DATA_OFFSET;
    const int32_t x = little_endian_i32(payload);
    const int32_t y = little_endian_i32(payload + 4);
    const int32_t z = little_endian_i32(payload + 8);
    const double tick_seconds =
        ((double)sensor->timebase.numer / (double)sensor->timebase.denom) *
        1e-9;

    haptik_accel_sample_t sample = {
        .timestamp_seconds = (double)timestamp * tick_seconds,
        .x_g = (float)((double)x / HAPTIK_Q16_SCALE),
        .y_g = (float)((double)y / HAPTIK_Q16_SCALE),
        .z_g = (float)((double)z / HAPTIK_Q16_SCALE)
    };
    sensor->callback(&sample, sensor->callback_context);
}

static void publish_startup_result(
    haptik_sensor_t *sensor,
    haptik_sensor_result_t result
) {
    pthread_mutex_lock(&sensor->state_mutex);
    sensor->startup_result = result;
    sensor->startup_complete = true;
    pthread_cond_signal(&sensor->state_condition);
    pthread_mutex_unlock(&sensor->state_mutex);
}

static void *sensor_thread_main(void *context) {
    haptik_sensor_t *sensor = context;
    (void)mach_timebase_info(&sensor->timebase);
    (void)wake_sensor_drivers();

    io_service_t service = copy_accelerometer_service();
    if (service == IO_OBJECT_NULL) {
        publish_startup_result(sensor, HAPTIK_SENSOR_NOT_FOUND);
        return NULL;
    }

    sensor->device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    IOObjectRelease(service);
    if (sensor->device == NULL) {
        publish_startup_result(sensor, HAPTIK_SENSOR_IO_ERROR);
        return NULL;
    }

    const IOReturn open_result = IOHIDDeviceOpen(sensor->device, 0);
    if (open_result != kIOReturnSuccess) {
        const haptik_sensor_result_t result =
            open_result == kIOReturnNotPermitted ||
            open_result == kIOReturnNotPrivileged
                ? HAPTIK_SENSOR_PERMISSION_DENIED
                : HAPTIK_SENSOR_IO_ERROR;
        CFRelease(sensor->device);
        sensor->device = NULL;
        publish_startup_result(sensor, result);
        return NULL;
    }

    IOHIDDeviceRegisterInputReportWithTimeStampCallback(
        sensor->device,
        sensor->report_buffer,
        sizeof(sensor->report_buffer),
        report_callback,
        sensor
    );

    sensor->run_loop = CFRunLoopGetCurrent();
    CFRetain(sensor->run_loop);
    IOHIDDeviceScheduleWithRunLoop(
        sensor->device,
        sensor->run_loop,
        kCFRunLoopDefaultMode
    );
    publish_startup_result(sensor, HAPTIK_SENSOR_OK);

    while (!atomic_load_explicit(
        &sensor->stop_requested,
        memory_order_acquire
    )) {
        (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    }

    IOHIDDeviceUnscheduleFromRunLoop(
        sensor->device,
        sensor->run_loop,
        kCFRunLoopDefaultMode
    );
    IOHIDDeviceClose(sensor->device, 0);
    CFRelease(sensor->device);
    sensor->device = NULL;
    CFRelease(sensor->run_loop);
    sensor->run_loop = NULL;
    return NULL;
}

haptik_sensor_t *haptik_sensor_create(
    haptik_sensor_callback_t callback,
    void *context
) {
    if (callback == NULL) {
        return NULL;
    }

    haptik_sensor_t *sensor = calloc(1, sizeof(*sensor));
    if (sensor == NULL) {
        return NULL;
    }

    sensor->callback = callback;
    sensor->callback_context = context;
    sensor->startup_result = HAPTIK_SENSOR_IO_ERROR;
    atomic_init(&sensor->stop_requested, false);
    if (pthread_mutex_init(&sensor->state_mutex, NULL) != 0 ||
        pthread_cond_init(&sensor->state_condition, NULL) != 0) {
        free(sensor);
        return NULL;
    }
    return sensor;
}

haptik_sensor_result_t haptik_sensor_start(haptik_sensor_t *sensor) {
    if (sensor == NULL) {
        return HAPTIK_SENSOR_IO_ERROR;
    }
    if (sensor->thread_created) {
        return sensor->startup_result;
    }

    sensor->startup_complete = false;
    atomic_store_explicit(
        &sensor->stop_requested,
        false,
        memory_order_release
    );
    if (pthread_create(
            &sensor->thread,
            NULL,
            sensor_thread_main,
            sensor
        ) != 0) {
        return HAPTIK_SENSOR_THREAD_ERROR;
    }
    sensor->thread_created = true;

    pthread_mutex_lock(&sensor->state_mutex);
    while (!sensor->startup_complete) {
        pthread_cond_wait(
            &sensor->state_condition,
            &sensor->state_mutex
        );
    }
    const haptik_sensor_result_t result = sensor->startup_result;
    pthread_mutex_unlock(&sensor->state_mutex);

    if (result != HAPTIK_SENSOR_OK) {
        pthread_join(sensor->thread, NULL);
        sensor->thread_created = false;
    }
    return result;
}

void haptik_sensor_stop(haptik_sensor_t *sensor) {
    if (sensor == NULL || !sensor->thread_created) {
        return;
    }
    atomic_store_explicit(
        &sensor->stop_requested,
        true,
        memory_order_release
    );
    if (sensor->run_loop != NULL) {
        CFRunLoopStop(sensor->run_loop);
    }
    pthread_join(sensor->thread, NULL);
    sensor->thread_created = false;
}

void haptik_sensor_destroy(haptik_sensor_t *sensor) {
    if (sensor == NULL) {
        return;
    }
    haptik_sensor_stop(sensor);
    pthread_cond_destroy(&sensor->state_condition);
    pthread_mutex_destroy(&sensor->state_mutex);
    free(sensor);
}

const char *haptik_sensor_result_string(haptik_sensor_result_t result) {
    switch (result) {
        case HAPTIK_SENSOR_OK:
            return "ok";
        case HAPTIK_SENSOR_NOT_FOUND:
            return "accelerometer not found";
        case HAPTIK_SENSOR_PERMISSION_DENIED:
            return "accelerometer access denied";
        case HAPTIK_SENSOR_IO_ERROR:
            return "IOKit sensor error";
        case HAPTIK_SENSOR_THREAD_ERROR:
            return "sensor thread error";
    }
    return "unknown sensor error";
}
