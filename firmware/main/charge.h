#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IP2326 charger enable, wired to PI4IOE5V6408 #2 E2.P7. */
esp_err_t charge_set_enabled(bool enable);
bool charge_read_enabled(void);

/* Raw E2.P5 nCHG_QC_EN level for the board's NTC/current divider.
 * Low selects nominal current; high selects the reduced-current divider. */
esp_err_t charge_set_qc_high(bool high);

/* Disable charging, configure the NTC/current divider, then enable if asked.
 * Control pins use push-pull outputs; BAT_STAT is an input. */
esp_err_t charge_apply(bool enable, bool qc_high);

#ifdef __cplusplus
}
#endif
