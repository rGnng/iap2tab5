#include "charge.h"

#include "bsp/esp-bsp.h"
#include "esp_io_expander.h"
#include "esp_log.h"

static const char * TAG = "charge";

esp_err_t charge_set_enabled(bool enable)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    if(io == NULL) {
        ESP_LOGE(TAG, "IO expander unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_io_expander_set_dir(
        io, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
    if(err == ESP_OK) {
        err = esp_io_expander_set_level(
            io, IO_EXPANDER_PIN_NUM_7, enable ? 1 : 0);
    }

    if(err == ESP_OK) {
        ESP_LOGI(TAG, "CHG_EN=%d", enable ? 1 : 0);
    }
    else {
        ESP_LOGE(TAG, "CHG_EN set failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool charge_read_enabled(void)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    if(io == NULL || io->read_output_reg == NULL) return false;

    uint32_t out = 0;
    if(io->read_output_reg(io, &out) != ESP_OK) return false;
    return (out & IO_EXPANDER_PIN_NUM_7) != 0;
}

esp_err_t charge_set_qc_high(bool high)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    if(io == NULL) {
        ESP_LOGE(TAG, "IO expander unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_io_expander_set_dir(
        io, IO_EXPANDER_PIN_NUM_5, IO_EXPANDER_OUTPUT);
    if(err == ESP_OK) {
        err = esp_io_expander_set_level(
            io, IO_EXPANDER_PIN_NUM_5, high ? 1 : 0);
    }

    if(err == ESP_OK) {
        ESP_LOGI(TAG, "nCHG_QC_EN=%d", high ? 1 : 0);
    }
    else {
        ESP_LOGE(TAG, "nCHG_QC_EN set failed: %s", esp_err_to_name(err));
    }
    return err;
}
