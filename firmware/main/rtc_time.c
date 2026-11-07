#include "rtc_time.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

static const char * TAG = "rtc";

#define RTC_ADDR          0x32
#define RTC_REG_SEC       0x10
#define RTC_REG_MIN       0x11
#define RTC_REG_HOUR      0x12
#define RTC_REG_WDAY      0x13
#define RTC_REG_MDAY      0x14
#define RTC_REG_MONTH     0x15
#define RTC_REG_YEAR      0x16
#define RTC_REG_EXT       0x1C
#define RTC_REG_FLAG      0x1D
#define RTC_REG_CONTROL   0x1E
#define RTC_REG_CONTROL1  0x1F
#define RTC_REG_OFFSET    0x30

#define RTC_FLAG_VLF      0x02
#define RTC_EXT_TE        0x10
#define RTC_CTRL_INIEN    0x10
#define RTC_CTRL_CHGEN    0x20
/* FLAG is write-zero-to-clear.  Clear VLF while leaving the other flags. */
#define RTC_FLAG_CLEAR_VLF 0xFD

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;
static bool s_available;

static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t dec_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static esp_err_t rtc_write_reg(uint8_t reg, uint8_t value)
{
    if(s_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t rtc_read_reg(uint8_t reg, uint8_t * value)
{
    if(s_dev == NULL || value == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 100);
}

static esp_err_t rtc_read_regs(uint8_t reg, uint8_t * data, size_t len)
{
    if(s_dev == NULL || data == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

static esp_err_t rtc_write_regs(uint8_t reg, const uint8_t * data, size_t len)
{
    if(s_dev == NULL || data == NULL || len > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[8];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 100);
}

static void rtc_lock(void)
{
    if(s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void rtc_unlock(void)
{
    if(s_lock != NULL) xSemaphoreGive(s_lock);
}

/* Keep the Tab5 supercapacitor charged (CHGEN) and the power switch enabled
 * (INIEN).  Also make sure the chip is not left in test/stop mode and that
 * the unused periodic timer is stopped. */
static esp_err_t rtc_configure(void)
{
    uint8_t control1 = 0;
    esp_err_t err = rtc_read_reg(RTC_REG_CONTROL1, &control1);
    if(err != ESP_OK) return err;
    control1 = (uint8_t)((control1 & ~RTC_CTRL_CHGEN) |
                         RTC_CTRL_INIEN | RTC_CTRL_CHGEN);
    err = rtc_write_reg(RTC_REG_CONTROL1, control1);
    if(err != ESP_OK) return err;

    err = rtc_write_reg(RTC_REG_OFFSET, 0x00);
    if(err != ESP_OK) return err;

    err = rtc_write_reg(RTC_REG_CONTROL, 0x00);
    if(err != ESP_OK) return err;

    uint8_t ext = 0;
    err = rtc_read_reg(RTC_REG_EXT, &ext);
    if(err != ESP_OK) return err;
    if((ext & RTC_EXT_TE) != 0) {
        err = rtc_write_reg(RTC_REG_EXT, (uint8_t)(ext & ~RTC_EXT_TE));
    }
    return err;
}

static bool rtc_parse_time(const uint8_t * data, struct tm * out)
{
    int sec = bcd_to_dec(data[0] & 0x7F);
    int min = bcd_to_dec(data[1] & 0x7F);
    int hour = bcd_to_dec(data[2] & 0x3F);
    int mday = bcd_to_dec(data[4] & 0x3F);
    int month = bcd_to_dec(data[5] & 0x1F);
    int year = 2000 + bcd_to_dec(data[6]);

    /* WDAY is a one-hot bit field (Sunday = bit 0), not BCD. */
    uint8_t wday_bits = data[3] & 0x7F;
    int wday = 0;
    if(wday_bits != 0 && (wday_bits & (wday_bits - 1)) == 0) {
        wday = __builtin_ctz(wday_bits);
    }

    if(sec > 59 || min > 59 || hour > 23 || wday > 6 ||
       mday < 1 || mday > 31 || month < 1 || month > 12 ||
       year < 2020 || year > 2099) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->tm_sec = sec;
    out->tm_min = min;
    out->tm_hour = hour;
    out->tm_wday = wday;
    out->tm_mday = mday;
    out->tm_mon = month - 1;
    out->tm_year = year - 1900;
    out->tm_isdst = -1;
    return true;
}

static esp_err_t rtc_read_time(struct tm * out)
{
    uint8_t data[7];
    esp_err_t err = rtc_read_regs(RTC_REG_SEC, data, sizeof(data));
    if(err != ESP_OK) return err;
    ESP_LOGI(TAG, "raw time %02x %02x %02x %02x %02x %02x %02x",
             data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
    return rtc_parse_time(data, out) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t rtc_write_time(const struct tm * local)
{
    uint8_t data[7] = {
        dec_to_bcd((uint8_t)local->tm_sec),
        dec_to_bcd((uint8_t)local->tm_min),
        dec_to_bcd((uint8_t)local->tm_hour),
        (uint8_t)(1u << (local->tm_wday & 0x07)),
        dec_to_bcd((uint8_t)local->tm_mday),
        dec_to_bcd((uint8_t)(local->tm_mon + 1)),
        dec_to_bcd((uint8_t)((local->tm_year + 1900) % 100)),
    };
    return rtc_write_regs(RTC_REG_SEC, data, sizeof(data));
}

static esp_err_t rtc_clear_voltage_loss(void)
{
    uint8_t current = 0;
    if(rtc_read_reg(RTC_REG_FLAG, &current) == ESP_OK &&
       (current & RTC_FLAG_VLF) == 0) {
        return ESP_OK;
    }

    for(int attempt = 0; attempt < 10; attempt++) {
        esp_err_t err = rtc_write_reg(RTC_REG_FLAG, RTC_FLAG_CLEAR_VLF);
        if(err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t flag = 0;
        if(rtc_read_reg(RTC_REG_FLAG, &flag) == ESP_OK &&
           (flag & RTC_FLAG_VLF) == 0) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t rtc_time_init(void)
{
    if(s_dev != NULL) return ESP_OK;
    if(s_lock == NULL) s_lock = xSemaphoreCreateMutex();

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if(bus == NULL) {
        ESP_LOGW(TAG, "I2C bus unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if(i2c_master_probe(bus, RTC_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "RX8130CE not found at 0x%02x", RTC_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }
    s_available = true;

    rtc_lock();
    err = rtc_configure();
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "configure failed: %s", esp_err_to_name(err));
    }

    uint8_t flag = 0;
    err = rtc_read_reg(RTC_REG_FLAG, &flag);
    ESP_LOGI(TAG, "flag=0x%02x", (unsigned)flag);
    struct tm local;
    bool valid = err == ESP_OK && (flag & RTC_FLAG_VLF) == 0 &&
                 rtc_read_time(&local) == ESP_OK;
    time_t reference = time(NULL);
    time_t epoch = valid ? mktime(&local) : (time_t)-1;
    if(epoch > 0) {
        long long diff = (long long)epoch - (long long)reference;
        if(diff < 0) diff = -diff;
        /* A fresh / undetected RTC can contain plausible-looking BCD garbage.
         * Reject dates more than 15 years away from the build clock. */
        if(diff > (15LL * 365 * 24 * 3600)) {
            ESP_LOGW(TAG, "ignoring implausible RTC time");
            valid = false;
        }
    }
    else {
        valid = false;
    }

    if(valid) {
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        rtc_unlock();
        ESP_LOGI(TAG, "loaded %04d-%02d-%02d %02d:%02d:%02d (VLP=0)",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "RTC invalid (flag=0x%02x), seeding from system clock",
             (unsigned)flag);
    time_t now = time(NULL);
    struct tm seed;
    localtime_r(&now, &seed);
    err = rtc_write_time(&seed);
    if(err == ESP_OK) err = rtc_clear_voltage_loss();
    rtc_unlock();
    if(err == ESP_OK) {
        ESP_LOGI(TAG, "seeded %04d-%02d-%02d %02d:%02d:%02d",
                 seed.tm_year + 1900, seed.tm_mon + 1, seed.tm_mday,
                 seed.tm_hour, seed.tm_min, seed.tm_sec);
    }
    else {
        ESP_LOGW(TAG, "RTC seed failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t rtc_time_sync_epoch(time_t utc)
{
    if(utc <= 0) return ESP_ERR_INVALID_ARG;

    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    if(s_dev == NULL) {
        ESP_LOGW(TAG, "system time updated, RTC unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    struct tm local;
    localtime_r(&utc, &local);
    rtc_lock();
    esp_err_t err = rtc_write_time(&local);
    if(err == ESP_OK) err = rtc_clear_voltage_loss();
    rtc_unlock();
    if(err == ESP_OK) {
        ESP_LOGI(TAG, "synced %04d-%02d-%02d %02d:%02d:%02d",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec);
    }
    else {
        ESP_LOGW(TAG, "RTC write failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool rtc_time_available(void)
{
    return s_available;
}
