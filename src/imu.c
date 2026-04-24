#include "imu.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

// Per the Waveshare ESP32-S3-Touch-LCD-2.8 pinout: the QMI8658 sits on a
// different I2C bus than the CST328 touch controller. Touch uses port 0 on
// GPIO1/3; the IMU is on port 1 at GPIO10/11.
#define IMU_I2C_PORT   I2C_NUM_1
#define IMU_PIN_SDA    11
#define IMU_PIN_SCL    10
#define IMU_I2C_HZ     400000

// QMI8658C register map (subset — see QST QMI8658 datasheet rev 1.2).
#define QMI8658_REG_WHO_AM_I   0x00
#define QMI8658_REG_CTRL1      0x02   // system
#define QMI8658_REG_CTRL2      0x03   // accel: [7]aST [6:4]aFS [3:0]aODR
#define QMI8658_REG_CTRL3      0x04   // gyro (unused — we leave it off)
#define QMI8658_REG_CTRL7      0x08   // sensor enable: [0]aEN [1]gEN
#define QMI8658_REG_AX_L       0x35

#define QMI8658_WHO_AM_I_VALUE 0x05
// ±8 g full scale → 32768 LSB = 8 g → 0.000244 g/LSB.
#define ACCEL_LSB_TO_G         (8.0f / 32768.0f)

// The SA0 pad on the Waveshare module is wired high, giving 0x6B. We probe
// both addresses at init so a board revision that flips SA0 still comes up.
#define QMI8658_ADDR_PRIMARY   0x6B
#define QMI8658_ADDR_ALT       0x6A

// Shake tuning. 50 Hz poll → a vigorous shake produces multiple
// high-magnitude samples within a few hundred ms. Cooldown prevents the
// same shake from triggering the callback twice.
#define POLL_PERIOD_MS         20
#define SHAKE_THRESHOLD_G      1.2f
#define SHAKE_HIGH_SAMPLES     4
#define SHAKE_WINDOW_MS        400
#define SHAKE_COOLDOWN_MS      3500

static i2c_master_dev_handle_t s_dev    = NULL;
static imu_shake_cb_t          s_cb     = NULL;
static volatile bool           s_ready  = false;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t n) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val) {
    uint8_t pkt[2] = { reg, val };
    return i2c_master_transmit(s_dev, pkt, 2, 100);
}

static bool probe_addr(i2c_master_bus_handle_t bus, uint8_t addr) {
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;

    uint8_t id = 0;
    if (read_regs(QMI8658_REG_WHO_AM_I, &id, 1) != ESP_OK || id != QMI8658_WHO_AM_I_VALUE) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }
    return true;
}

// Converts the six raw accel bytes into floating-point g on x/y/z. Registers
// are little-endian int16; CTRL1 bit 5 (BE) is left at 0 to match.
static void decode_accel(const uint8_t *raw, float *ax, float *ay, float *az) {
    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);
    *ax = x * ACCEL_LSB_TO_G;
    *ay = y * ACCEL_LSB_TO_G;
    *az = z * ACCEL_LSB_TO_G;
}

static void imu_task(void *arg) {
    (void)arg;
    int64_t window_start  = 0;
    int     high_count    = 0;
    int64_t last_trigger  = 0;

    while (true) {
        uint8_t raw[6];
        if (read_regs(QMI8658_REG_AX_L, raw, sizeof(raw)) == ESP_OK) {
            float ax, ay, az;
            decode_accel(raw, &ax, &ay, &az);
            float mag   = sqrtf(ax*ax + ay*ay + az*az);
            float delta = fabsf(mag - 1.0f);

            int64_t now = esp_timer_get_time() / 1000;
            if (delta > SHAKE_THRESHOLD_G) {
                if (high_count == 0 || (now - window_start) > SHAKE_WINDOW_MS) {
                    window_start = now;
                    high_count   = 1;
                } else {
                    high_count++;
                }
            } else if ((now - window_start) > SHAKE_WINDOW_MS) {
                high_count = 0;
            }

            if (high_count >= SHAKE_HIGH_SAMPLES &&
                (now - last_trigger) > SHAKE_COOLDOWN_MS) {
                last_trigger = now;
                high_count   = 0;
                imu_shake_cb_t cb = s_cb;
                if (cb) cb();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

bool imu_begin(void) {
    if (s_ready) return true;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = IMU_I2C_PORT,
        .sda_io_num                   = IMU_PIN_SDA,
        .scl_io_num                   = IMU_PIN_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed");
        return false;
    }

    if (!probe_addr(bus, QMI8658_ADDR_PRIMARY) &&
        !probe_addr(bus, QMI8658_ADDR_ALT)) {
        ESP_LOGW(TAG, "QMI8658 not found at 0x6A/0x6B");
        return false;
    }

    // CTRL1 = 0x40 → auto-increment on multi-byte reads, little-endian, INTs off.
    // CTRL2 = 0x27 → accel ±8 g, ODR 62.5 Hz.
    // CTRL7 = 0x01 → enable accel, gyro stays off.
    if (write_reg(QMI8658_REG_CTRL1, 0x40) != ESP_OK ||
        write_reg(QMI8658_REG_CTRL2, 0x27) != ESP_OK ||
        write_reg(QMI8658_REG_CTRL7, 0x01) != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 config write failed");
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        imu_task, "imu", 3072, NULL, 4, NULL, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "imu task create failed");
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 up (accel ±8g @62.5Hz, shake-only)");
    return true;
}

void imu_set_shake_cb(imu_shake_cb_t cb) {
    s_cb = cb;
}
