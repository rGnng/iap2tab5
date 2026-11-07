#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IP2326 charger enable, wired to PI4IOE5V6408 #2 E2.P7. */
esp_err_t charge_set_enabled(bool enable);
bool charge_read_enabled(void);

/* Raw level of E2.P5 nCHG_QC_EN (active-low by name).  Kept as a debug/test
 * control because the IP2326 NTC/QC divider is also affected by this pin. */
esp_err_t charge_set_qc_high(bool high);

#ifdef __cplusplus
}
#endif
