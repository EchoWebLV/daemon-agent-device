#pragma once
// ---------------------------------------------------------------------------
//  QMI8658C 6-axis IMU driver + shake detector.
//
//  The Waveshare ESP32-S3-Touch-LCD-2.8 carries a QST QMI8658C on the same
//  I2C bus as the CST328 touch controller. We only use the accelerometer —
//  the gyro is left disabled to save power.
//
//  imu_begin() expects touch_init() to have run first (we borrow its bus).
//  After init, a background task polls the accelerometer at ~50 Hz and
//  invokes the registered shake callback when it sees vigorous movement.
//  The callback runs on the IMU task and must not block for long.
// ---------------------------------------------------------------------------
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*imu_shake_cb_t)(void);

// Probe the sensor, configure it for ±8 g / 62.5 Hz accel-only, and spawn
// the polling task. Returns false if WHO_AM_I didn't match or the task
// couldn't be created. Safe to call more than once — subsequent calls are
// no-ops and return the first-call result.
bool imu_begin(void);

// Register the shake callback. Pass NULL to disable. Only one callback
// at a time — last writer wins.
void imu_set_shake_cb(imu_shake_cb_t cb);

#ifdef __cplusplus
}
#endif
