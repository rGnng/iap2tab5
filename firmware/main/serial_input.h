#ifndef IAP2TAB5_SERIAL_INPUT_H
#define IAP2TAB5_SERIAL_INPUT_H

#include <stdbool.h>

/** Start the USB Serial/JTAG console key reader (debug/preview input). */
void serial_input_start(void);

/** Inject one debug key (same mapping as the serial console) from another
 * task, for example the web debug API. Returns false for an unknown key. */
bool serial_input_inject_key(char c);

#endif /* IAP2TAB5_SERIAL_INPUT_H */
