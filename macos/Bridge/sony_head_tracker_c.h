#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SHTHandle SHTHandle;

typedef struct {
    double rotation_vector[3];
    double quaternion[4];
    double ypr_degrees[3];
    double gyroscope[3];
    double accelerometer[3];
    bool has_gyroscope;
    bool has_accelerometer;
    uint8_t reset_counter;
    double packets_per_second;
    double receive_latency_ms;
} SHTSample;

typedef struct {
    double smoothing;
    uint16_t udp_port;
    unsigned axis_source[3];
    double axis_sign[3];
} SHTConfig;

typedef enum {
    SHT_STATUS_STOPPED = 0,
    SHT_STATUS_SCANNING = 1,
    SHT_STATUS_CONNECTED = 2,
    SHT_STATUS_RECONNECTING = 3,
    SHT_STATUS_PERMISSION_DENIED = 4,
    SHT_STATUS_NOT_VISIBLE = 5,
    SHT_STATUS_NOT_VERIFIED = 6,
    SHT_STATUS_FEATURE_WRITE_FAILED = 7,
    SHT_STATUS_STREAM_TIMEOUT = 8,
    SHT_STATUS_UDP_ERROR = 9,
    SHT_STATUS_ERROR = 10
} SHTStatus;

typedef void (*SHTSampleCallback)(const SHTSample *sample, void *context);
typedef void (*SHTStatusCallback)(SHTStatus status,
                                  const char *message_utf8,
                                  void *context);

SHTHandle *sht_create(void);
void sht_destroy(SHTHandle *handle);
bool sht_start(SHTHandle *handle,
               uint16_t base_port,
               SHTSampleCallback sample_callback,
               SHTStatusCallback status_callback,
               void *context);
void sht_stop(SHTHandle *handle);
void sht_recenter(SHTHandle *handle);
void sht_set_filter(SHTHandle *handle,
                    double smoothing,
                    const unsigned axis_source[3],
                    const double axis_sign[3]);

// Returns the required UTF-8 buffer size including the trailing NUL. When
// buffer is non-null and capacity is nonzero, writes a NUL-terminated,
// shareable diagnostic snapshot with personal device identifiers omitted.
size_t sht_get_diagnostics(SHTHandle *handle, char *buffer, size_t capacity);
bool sht_load_config(SHTConfig *config);
bool sht_save_config(const SHTConfig *config);

#ifdef __cplusplus
}
#endif
