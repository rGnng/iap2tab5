#include "np_battery_hw.h"
#include "charge.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"

static np_battery_t model;
static np_battery_status_t cached = {.percent=-1};
static portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

void np_battery_hw_snapshot(np_battery_status_t *out)
{
    portENTER_CRITICAL(&snapshot_lock);
    *out=cached;
    portEXIT_CRITICAL(&snapshot_lock);
}

void np_battery_hw_poll(uint32_t now_ms)
{
    static i2c_master_dev_handle_t dev;
    np_battery_sample_t sample={0};
    i2c_master_bus_handle_t bus=bsp_i2c_get_handle();
    if(!dev && bus) {
        const i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
            .device_address=0x41,.scl_speed_hz=100000};
        i2c_master_bus_add_device(bus,&cfg,&dev);
    }
    if(dev) {
        const uint8_t regs[]={0x00,0x02,0x01};
        uint16_t values[3]={0};
        esp_err_t err=ESP_OK;
        for(unsigned i=0;i<3 && err==ESP_OK;i++) {
            uint8_t raw[2];
            err=i2c_master_transmit_receive(dev,&regs[i],1,raw,2,100);
            if(err==ESP_OK) values[i]=(uint16_t)((raw[0]<<8)|raw[1]);
        }
        /* Require continuous shunt + bus conversion; shutdown/stale registers
         * do not establish charging, completion, or battery removal. */
        sample.valid=err==ESP_OK && (values[0]&7)==7;
        sample.voltage_mv=values[1]*5/4;
        sample.current_ma=(int16_t)values[2]*5/10; /* 2.5 uV / 5 mOhm */
        sample.charger_enabled=charge_read_enabled();
    }
    np_battery_update(&model,&sample,now_ms);
    portENTER_CRITICAL(&snapshot_lock);
    cached=model.status;
    portEXIT_CRITICAL(&snapshot_lock);
}
