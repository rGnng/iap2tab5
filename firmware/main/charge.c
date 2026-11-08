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
        /* PI4IOE5V6408 defaults to high-Z: the output latch alone does not
         * drive EN past the board's external pull-down. */
        err = esp_io_expander_set_output_mode(
            io, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
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
    if(io == NULL || io->read_output_reg == NULL ||
       io->read_direction_reg == NULL || io->read_highz_reg == NULL) return false;

    uint32_t out = 0, direction = 0, highz = 0;
    if(io->read_output_reg(io, &out) != ESP_OK ||
       io->read_direction_reg(io, &direction) != ESP_OK ||
       io->read_highz_reg(io, &highz) != ESP_OK) return false;
    return (out & direction & ~highz & IO_EXPANDER_PIN_NUM_7) != 0;
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
        err = esp_io_expander_set_output_mode(
            io, IO_EXPANDER_PIN_NUM_5, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    }

    if(err == ESP_OK) {
        ESP_LOGI(TAG, "nCHG_QC_EN=%d", high ? 1 : 0);
    }
    else {
        ESP_LOGE(TAG, "nCHG_QC_EN set failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t charge_apply(bool enable, bool qc_high)
{
    /* Configure the board's NTC/current divider while the charger is off,
     * then apply the requested enable state. */
    esp_err_t err = charge_set_enabled(false);
    if(err == ESP_OK) {
        esp_io_expander_handle_t io = bsp_io_expander1_init();
        err = esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_INPUT);
    }
    if(err == ESP_OK) err = charge_set_qc_high(qc_high);
    if(err == ESP_OK && enable) {
        err = charge_set_enabled(true);
        if(err != ESP_OK) charge_set_enabled(false);
    }
    return err;
}
