# Local USB HID host driver

Based on Espressif `usb_host_hid` 1.2.1, esp-usb commit
`e3c35b840b9fc9969b981011c9ff782c01168433` (Apache-2.0; see LICENSE).
Selected by `main/idf_component.yml` using `override_path`.

Local addition: `hid_host_device_send_output_report()` discovers and uses the
optional interrupt OUT endpoint. It copies the caller buffer, queues at most
one packet per interface, and logs completion errors. ESP_OK means queued,
not a device-level acknowledgement. Reports larger than the endpoint packet
size are rejected. The output buffer is allocated at open and released at
close; endpoint shutdown follows the input endpoint lifecycle.

The D100H vendor heartbeat needs this path. The application's existing
SET_REPORT fallback cannot activate vendor keys on the tested unit.
