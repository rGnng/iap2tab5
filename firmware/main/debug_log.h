#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-memory log mirror for the web debug console.
 *
 * ESP_LOG output is still forwarded to the USB Serial/JTAG console.  The
 * mirror stays available when USB-C is switched to host mode and the serial
 * console is no longer reachable.
 */
void debug_log_init(void);

/* Latest sequence number (the next line to be written). */
uint32_t debug_log_seq(void);

/* Copies raw log lines written after "since" into out (newline separated).
 * Returns the sequence number to pass on the next call.  If since is older
 * than the window, *dropped receives the number of skipped lines. */
uint32_t debug_log_read_text(uint32_t since, char * out, size_t cap,
                             size_t * out_len, uint32_t * dropped);

/* Same as debug_log_read_text(), but formats each line as an SSE data frame:
 *   data: <line>\n\n
 */
uint32_t debug_log_read_sse(uint32_t since, char * out, size_t cap,
                            size_t * out_len, uint32_t * dropped);

/* Append the log mirror to /sdcard/debug.log while enabled.  The setting is
 * persisted by app_config; the writer runs in its own low-priority task. */
void debug_log_set_sd_enabled(bool enabled);
bool debug_log_sd_enabled(void);

/* Runtime state for diagnostics. */
bool debug_log_sd_file_open(void);
uint32_t debug_log_sd_lines(void);
uint32_t debug_log_sd_errors(void);

#ifdef __cplusplus
}
#endif
