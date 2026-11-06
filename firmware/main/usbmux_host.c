#include "usbmux_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

static const char * TAG = "usbmux";

#define APPLE_VID       0x05AC
#define IPOD_PID        0x12AA
#define USB_MRU         1024
#define MUX_BUF_SIZE    (64 * 1024)
#define MUX_MAGIC       0xFEEDFACEu

#define MUX_PROTO_VERSION 0
#define MUX_PROTO_CONTROL 1
#define MUX_PROTO_SETUP   2
#define MUX_PROTO_TCP     6

#define TCP_FIN         0x01
#define TCP_SYN         0x02
#define TCP_RST         0x04
#define TCP_PSH         0x08
#define TCP_ACK         0x10

#define MUX_MAX_CONN    4
#define MUX_CONN_BUF    (128 * 1024)
#define MUX_TX_QUEUE    32

/* The iPod's mux uses a fixed window scale of 256: the 16 bit field is
 * the real window shifted right by 8. usbmuxd uses 131072. */
#define TCP_WINDOW      131072u
#define TCP_WINDOW_FIELD (TCP_WINDOW >> 8)

typedef enum {
    CONN_FREE = 0,
    CONN_SYN_SENT,
    CONN_ESTABLISHED,
    CONN_CLOSED,
} conn_state_t;

struct usbmux_conn {
    volatile conn_state_t state;
    uint16_t sport;
    uint16_t dport;
    uint32_t tx_seq;
    uint32_t rx_ack;
    uint32_t peer_win;
    StreamBufferHandle_t rx;
    SemaphoreHandle_t open_sem;
};

typedef struct {
    uint8_t * data;
    uint16_t len;
} mux_tx_item_t;

static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static uint8_t s_ep_in;
static uint8_t s_ep_out;
static uint16_t s_mps = 512;
static uint8_t s_interface;

static usb_transfer_t * s_rx_xfer;
static SemaphoreHandle_t s_ready_sem;
static SemaphoreHandle_t s_conn_lock;
static SemaphoreHandle_t s_tx_alloc_lock;
static SemaphoreHandle_t s_tx_done;
static QueueHandle_t s_tx_queue;

static uint8_t * s_mux_buf;
static size_t s_mux_len;
static uint8_t s_version;
static bool s_mux_active;
static uint16_t s_tx_seq;
static uint16_t s_rx_seq;
static uint16_t s_next_sport = 0x4000;
static struct usbmux_conn s_conns[MUX_MAX_CONN];
static char s_status[80] = "searching for iPod";

static uint32_t rd_be32(const uint8_t * p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rd_be16(const uint8_t * p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr_be32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr_be16(uint8_t * p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void set_status(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* USB transmit queue                                                  */
/* ------------------------------------------------------------------ */

static void tx_done_cb(usb_transfer_t * xfer)
{
    if(xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "mux TX failed: %d", (int)xfer->status);
    }
    usb_host_transfer_free(xfer);
    xSemaphoreGive(s_tx_done);
}

static void mux_tx_task(void * arg)
{
    (void)arg;
    for(;;) {
        mux_tx_item_t item;
        if(xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        if(s_dev == NULL) {
            free(item.data);
            continue;
        }

        usb_transfer_t * xfer = NULL;
        esp_err_t err = ESP_FAIL;
        while(s_dev != NULL) {
            err = usb_host_transfer_alloc(item.len, 0, &xfer);
            if(err == ESP_OK) break;
            ESP_LOGW(TAG, "TX alloc %u failed: %s, retrying", item.len,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if(xfer == NULL) {
            free(item.data);
            continue;
        }

        memcpy(xfer->data_buffer, item.data, item.len);
        xfer->device_handle = s_dev;
        xfer->bEndpointAddress = s_ep_out;
        xfer->num_bytes = item.len;
        xfer->flags = USB_TRANSFER_FLAG_ZERO_PACK;
        xfer->callback = tx_done_cb;
        xfer->context = NULL;

        err = usb_host_transfer_submit(xfer);
        free(item.data);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "TX submit failed: %s", esp_err_to_name(err));
            usb_host_transfer_free(xfer);
            continue;
        }
        /* Keep the queue in order and bound the number of in-flight
         * transfers. The callback runs in the USB client task, which is
         * never blocked on this semaphore. */
        xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(2000));
    }
}

/* Builds a mux packet (16 byte v2 header when negotiated, 8 byte
 * otherwise), assigns the mux sequence numbers and queues it. */
static esp_err_t mux_enqueue_packet(uint32_t proto, const void * header,
                                    size_t header_len, const void * payload,
                                    size_t payload_len)
{
    if(xSemaphoreTake(s_tx_alloc_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t mux_hdr = (s_version >= 2) ? 16 : 8;
    size_t total = mux_hdr + header_len + payload_len;
    if(total > USB_MRU * 2) {
        xSemaphoreGive(s_tx_alloc_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t * buf = heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(buf == NULL) {
        xSemaphoreGive(s_tx_alloc_lock);
        return ESP_ERR_NO_MEM;
    }

    wr_be32(buf + 0, proto);
    wr_be32(buf + 4, (uint32_t)total);

    if(mux_hdr == 16) {
        uint16_t tx_seq = s_tx_seq;
        if(proto == MUX_PROTO_SETUP) {
            /* usbmuxd resets both counters for the setup packet. */
            s_tx_seq = 0;
            s_rx_seq = 0xFFFF;
            tx_seq = 0;
        }
        wr_be32(buf + 8, MUX_MAGIC);
        wr_be16(buf + 12, tx_seq);
        wr_be16(buf + 14, s_rx_seq);
        s_tx_seq = (uint16_t)(tx_seq + 1);
    }

    if(header_len > 0) {
        memcpy(buf + mux_hdr, header, header_len);
    }
    if(payload_len > 0) {
        memcpy(buf + mux_hdr + header_len, payload, payload_len);
    }

    mux_tx_item_t item = {
        .data = buf,
        .len = (uint16_t)total,
    };

    if(xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, dropping proto %u len %u",
                 (unsigned)proto, (unsigned)total);
        free(buf);
        xSemaphoreGive(s_tx_alloc_lock);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_tx_alloc_lock);
    return ESP_OK;
}

static esp_err_t send_tcp_segment(struct usbmux_conn * conn, uint8_t flags,
                                  uint32_t seq, uint32_t ack,
                                  const void * payload, size_t payload_len)
{
    uint8_t th[20];
    memset(th, 0, sizeof(th));
    wr_be16(th + 0, conn->sport);
    wr_be16(th + 2, conn->dport);
    wr_be32(th + 4, seq);
    wr_be32(th + 8, ack);
    th[12] = 5 << 4;              /* data offset, no options */
    th[13] = flags;
    wr_be16(th + 14, TCP_WINDOW_FIELD);

    return mux_enqueue_packet(MUX_PROTO_TCP, th, sizeof(th), payload,
                              payload_len);
}

/* ------------------------------------------------------------------ */
/* Mux handshake and inbound packet handling                           */
/* ------------------------------------------------------------------ */

static esp_err_t send_version_request(void)
{
    uint8_t vh[12];
    wr_be32(vh + 0, 2);   /* major */
    wr_be32(vh + 4, 0);   /* minor */
    wr_be32(vh + 8, 0);   /* padding */

    s_version = 0;
    s_mux_len = 0;
    s_tx_seq = 0;
    s_rx_seq = 0;
    s_mux_active = false;

    ESP_LOGI(TAG, "sending usbmux version request");
    return mux_enqueue_packet(MUX_PROTO_VERSION, vh, sizeof(vh), NULL, 0);
}

static esp_err_t send_setup_packet(void)
{
    const uint8_t setup = 0x07;
    ESP_LOGI(TAG, "sending usbmux setup packet");
    return mux_enqueue_packet(MUX_PROTO_SETUP, NULL, 0, &setup, 1);
}

static struct usbmux_conn * find_conn(uint16_t sport, uint16_t dport)
{
    struct usbmux_conn * conn = NULL;
    for(int i = 0; i < MUX_MAX_CONN; i++) {
        if(s_conns[i].state != CONN_FREE && s_conns[i].sport == dport &&
           s_conns[i].dport == sport) {
            conn = &s_conns[i];
            break;
        }
    }
    return conn;
}

static void handle_tcp_segment(const uint8_t * tcp, size_t len)
{
    if(len < 20) return;

    uint16_t sport = rd_be16(tcp + 0);   /* device side port */
    uint16_t dport = rd_be16(tcp + 2);   /* our side port */
    uint32_t seq = rd_be32(tcp + 4);
    uint32_t ack = rd_be32(tcp + 8);
    uint8_t data_off = (uint8_t)((tcp[12] >> 4) * 4);
    uint8_t flags = tcp[13];
    uint16_t win = rd_be16(tcp + 14);

    if(data_off < 20 || data_off > len) {
        ESP_LOGW(TAG, "TCP bad data offset %u (len %u)", data_off,
                 (unsigned)len);
        return;
    }

    const uint8_t * payload = tcp + data_off;
    size_t payload_len = len - data_off;

    struct usbmux_conn * conn = NULL;
    bool ack_needed = false;
    uint32_t ack_seq = 0;
    uint32_t ack_val = 0;
    bool wake_open = false;

    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    conn = find_conn(sport, dport);

    if(conn == NULL) {
        xSemaphoreGive(s_conn_lock);
        if(!(flags & TCP_RST)) {
            ESP_LOGD(TAG, "TCP no conn for %u->%u flags=0x%02x",
                     sport, dport, flags);
            struct usbmux_conn tmp = { .sport = dport, .dport = sport };
            send_tcp_segment(&tmp, TCP_RST, 0, seq + payload_len, NULL, 0);
        }
        return;
    }

    if(conn->state == CONN_SYN_SENT) {
        if((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            conn->tx_seq = ack;
            conn->rx_ack = seq + 1;
            conn->peer_win = (uint32_t)win << 8;
            conn->state = CONN_ESTABLISHED;
            ack_seq = conn->tx_seq;
            ack_val = conn->rx_ack;
            ack_needed = true;
            wake_open = true;
            ESP_LOGI(TAG, "TCP %u->%u established", conn->sport, conn->dport);
        }
        else if(flags & TCP_RST) {
            ESP_LOGW(TAG, "TCP %u->%u refused", conn->sport, conn->dport);
            conn->state = CONN_CLOSED;
            wake_open = true;
        }
    }
    else if(conn->state == CONN_ESTABLISHED) {
        if(flags & TCP_RST) {
            ESP_LOGW(TAG, "TCP %u->%u reset by device", conn->sport,
                     conn->dport);
            conn->state = CONN_CLOSED;
        }
        else if(payload_len > 0) {
            if(seq == conn->rx_ack) {
                size_t written = xStreamBufferSend(conn->rx, payload,
                                                   payload_len, 0);
                if(written != payload_len) {
                    ESP_LOGW(TAG, "TCP rx buffer full (%u/%u), dropping conn",
                             (unsigned)written, (unsigned)payload_len);
                    conn->state = CONN_CLOSED;
                }
                else {
                    conn->rx_ack += payload_len;
                    ack_seq = conn->tx_seq;
                    ack_val = conn->rx_ack;
                    ack_needed = true;
                    ESP_LOGD(TAG, "TCP %u->%u data %u bytes", sport, dport,
                             (unsigned)payload_len);
                }
            }
            else {
                ESP_LOGW(TAG, "TCP %u->%u seq %u expected %u len %u",
                         sport, dport, (unsigned)seq, (unsigned)conn->rx_ack,
                         (unsigned)payload_len);
                /* A retransmission can be ACKed without delivering it
                 * twice; a future segment would be a real gap. */
                if(seq + payload_len <= conn->rx_ack) {
                    ack_seq = conn->tx_seq;
                    ack_val = conn->rx_ack;
                    ack_needed = true;
                }
            }
        }
    }

    xSemaphoreGive(s_conn_lock);

    if(ack_needed) {
        struct usbmux_conn tmp = { .sport = dport, .dport = sport };
        send_tcp_segment(&tmp, TCP_ACK, ack_seq, ack_val, NULL, 0);
    }
    if(wake_open && conn != NULL) {
        xSemaphoreGive(conn->open_sem);
    }
}

static void handle_mux_packet(const uint8_t * packet, size_t len)
{
    const size_t hdr = (s_version >= 2) ? 16 : 8;
    if(len < hdr) return;

    uint32_t proto = rd_be32(packet);
    uint32_t plen = rd_be32(packet + 4);
    if(plen > len) {
        ESP_LOGW(TAG, "mux packet truncated (%u > %u)", (unsigned)plen,
                 (unsigned)len);
        return;
    }

    if(s_version >= 2) {
        uint32_t magic = rd_be32(packet + 8);
        /* usbmuxd writes 0xfeedface, but this iPod's replies carry
         * 0xfaceface. The field is not interpreted by either side. */
        if(magic != MUX_MAGIC && magic != 0xFACEFACEu) {
            ESP_LOGW(TAG, "bad mux magic 0x%08x proto=%u len=%u",
                     (unsigned)magic, (unsigned)proto, (unsigned)plen);
            return;
        }
        /* Bytes 12-13 are the device's own tx counter; ignore them.
         * Bytes 14-15 tell us how much of our stream it has received. */
        s_rx_seq = rd_be16(packet + 14);
    }

    if(proto == MUX_PROTO_VERSION) {
        if(len < hdr + 12) return;
        uint32_t major = rd_be32(packet + hdr);
        uint32_t minor = rd_be32(packet + hdr + 4);
        ESP_LOGI(TAG, "usbmux version %u.%u", (unsigned)major, (unsigned)minor);
        if(major != 1 && major != 2) {
            set_status("unsupported usbmux version");
            return;
        }
        s_version = (uint8_t)major;
        if(s_version >= 2) {
            if(send_setup_packet() == ESP_OK) {
                s_mux_active = true;
                set_status("usbmux v2 active");
                ESP_LOGI(TAG, "usbmux v2 active; Apple mux ready");
                xSemaphoreGive(s_ready_sem);
            }
            else {
                set_status("usbmux setup failed");
            }
        }
        else {
            s_mux_active = true;
            set_status("usbmux v1 active");
            xSemaphoreGive(s_ready_sem);
        }
    }
    else if(proto == MUX_PROTO_CONTROL) {
        size_t payload_len = len - hdr;
        if(payload_len > 1) {
            char message[128];
            size_t n = payload_len - 1;
            if(n >= sizeof(message)) n = sizeof(message) - 1;
            memcpy(message, packet + hdr + 1, n);
            message[n] = '\0';
            ESP_LOGI(TAG, "device control type %u: %s", packet[hdr], message);
        }
    }
    else if(proto == MUX_PROTO_TCP) {
        handle_tcp_segment(packet + hdr, len - hdr);
    }
}

static void feed_mux_data(const uint8_t * data, size_t len)
{
    if(len > MUX_BUF_SIZE - s_mux_len) {
        ESP_LOGW(TAG, "mux buffer overflow, dropping");
        s_mux_len = 0;
        return;
    }
    memcpy(s_mux_buf + s_mux_len, data, len);
    s_mux_len += len;

    for(;;) {
        size_t hdr = (s_version >= 2) ? 16 : 8;
        if(s_mux_len < hdr) break;
        uint32_t plen = rd_be32(s_mux_buf + 4);
        if(plen < hdr || plen > MUX_BUF_SIZE) {
            ESP_LOGW(TAG, "invalid mux length %u", (unsigned)plen);
            s_mux_len = 0;
            break;
        }
        if(s_mux_len < plen) break;

        handle_mux_packet(s_mux_buf, plen);
        if(plen < s_mux_len) {
            memmove(s_mux_buf, s_mux_buf + plen, s_mux_len - plen);
        }
        s_mux_len -= plen;
    }
}

static void rx_done_cb(usb_transfer_t * xfer)
{
    if(xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        if(xfer->actual_num_bytes > 0) {
            ESP_LOGD(TAG, "mux RX %d bytes", xfer->actual_num_bytes);
            feed_mux_data(xfer->data_buffer, (size_t)xfer->actual_num_bytes);
        }
        xfer->num_bytes = USB_MRU;
        if(usb_host_transfer_submit(xfer) != ESP_OK) {
            ESP_LOGW(TAG, "mux RX resubmit failed");
            if(s_rx_xfer == xfer) s_rx_xfer = NULL;
            usb_host_transfer_free(xfer);
        }
    }
    else {
        ESP_LOGW(TAG, "mux RX stopped: %d", (int)xfer->status);
        if(s_rx_xfer == xfer) s_rx_xfer = NULL;
        usb_host_transfer_free(xfer);
    }
}

/* ------------------------------------------------------------------ */
/* Device lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void close_device(void)
{
    /* The RX transfer is owned by its completion callback. Do not free it
     * here while it may still be pending; the host will complete it with
     * NO_DEVICE and the callback will release it. */
    s_rx_xfer = NULL;

    if(s_dev != NULL) {
        usb_host_interface_release(s_client, s_dev, s_interface);
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }

    s_ep_in = s_ep_out = 0;
    s_version = 0;
    s_mux_active = false;
    s_mux_len = 0;
    s_tx_seq = 0;
    s_rx_seq = 0;

    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    for(int i = 0; i < MUX_MAX_CONN; i++) {
        struct usbmux_conn * conn = &s_conns[i];
        if(conn->state == CONN_FREE) continue;
        conn->state = CONN_CLOSED;
        xSemaphoreGive(conn->open_sem);
    }
    xSemaphoreGive(s_conn_lock);

    set_status("iPod disconnected");
}

static void open_device(uint8_t address)
{
    if(s_dev != NULL) return;

    usb_device_handle_t dev = NULL;
    esp_err_t err = usb_host_device_open(s_client, address, &dev);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "device open failed: %s", esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t * dev_desc = NULL;
    err = usb_host_get_device_descriptor(dev, &dev_desc);
    if(err != ESP_OK || dev_desc == NULL ||
       dev_desc->idVendor != APPLE_VID || dev_desc->idProduct != IPOD_PID) {
        usb_host_device_close(s_client, dev);
        return;
    }

    const usb_config_desc_t * cfg = NULL;
    err = usb_host_get_active_config_descriptor(dev, &cfg);
    if(err != ESP_OK || cfg == NULL) {
        ESP_LOGW(TAG, "config descriptor failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_client, dev);
        return;
    }

    const usb_intf_desc_t * intf = NULL;
    int intf_offset = 0;
    for(int i = 0; i < cfg->bNumInterfaces; i++) {
        int offset = 0;
        const usb_intf_desc_t * candidate =
            usb_parse_interface_descriptor(cfg, (uint8_t)i, 0, &offset);
        if(candidate == NULL) continue;
        if(candidate->bInterfaceClass == 0xFF &&
           candidate->bInterfaceSubClass == 0xFE &&
           candidate->bInterfaceProtocol == 0x02) {
            intf = candidate;
            intf_offset = offset;
            break;
        }
    }

    if(intf == NULL) {
        ESP_LOGW(TAG, "iPod has no usbmux interface in config %u",
                 (unsigned)cfg->bConfigurationValue);
        usb_host_device_close(s_client, dev);
        return;
    }

    err = usb_host_interface_claim(s_client, dev, intf->bInterfaceNumber, 0);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "mux interface claim failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_client, dev);
        return;
    }

    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
    uint16_t mps = 0;
    for(int i = 0; i < intf->bNumEndpoints; i++) {
        int offset = intf_offset;
        const usb_ep_desc_t * ep =
            usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength,
                                                   &offset);
        if(ep == NULL) continue;
        if((ep->bmAttributes & 0x03) != 0x02) continue;
        if(ep->bEndpointAddress & 0x80) {
            ep_in = ep->bEndpointAddress;
            mps = ep->wMaxPacketSize & 0x7FF;
        }
        else {
            ep_out = ep->bEndpointAddress;
        }
    }

    if(ep_in == 0 || ep_out == 0) {
        ESP_LOGW(TAG, "mux bulk endpoints not found");
        usb_host_interface_release(s_client, dev, intf->bInterfaceNumber);
        usb_host_device_close(s_client, dev);
        return;
    }

    s_dev = dev;
    s_ep_in = ep_in;
    s_ep_out = ep_out;
    s_mps = mps ? mps : 512;
    s_interface = intf->bInterfaceNumber;

    ESP_LOGI(TAG, "iPod mux if=%u ep_out=0x%02x ep_in=0x%02x mps=%u",
             s_interface, s_ep_out, s_ep_in, s_mps);

    err = usb_host_transfer_alloc(USB_MRU, 0, &s_rx_xfer);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "RX transfer alloc failed: %s", esp_err_to_name(err));
        close_device();
        return;
    }
    s_rx_xfer->device_handle = s_dev;
    s_rx_xfer->bEndpointAddress = s_ep_in;
    s_rx_xfer->num_bytes = USB_MRU;
    s_rx_xfer->callback = rx_done_cb;
    s_rx_xfer->context = NULL;
    err = usb_host_transfer_submit(s_rx_xfer);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "RX submit failed: %s", esp_err_to_name(err));
        close_device();
        return;
    }

    set_status("iPod detected, mux handshake");
    send_version_request();
}

static void client_event_cb(const usb_host_client_event_msg_t * msg, void * arg)
{
    (void)arg;
    if(msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        open_device(msg->new_dev.address);
    }
    else if(msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if(msg->dev_gone.dev_hdl == s_dev) {
            ESP_LOGI(TAG, "iPod disconnected");
            close_device();
        }
    }
}

static void client_task(void * arg)
{
    (void)arg;
    for(;;) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

/* ------------------------------------------------------------------ */
/* Public connection API                                               */
/* ------------------------------------------------------------------ */

esp_err_t usbmux_connect(uint16_t port, usbmux_conn_t ** out,
                         uint32_t timeout_ms)
{
    if(out == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    if(!usbmux_host_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    struct usbmux_conn * conn = NULL;
    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    for(int i = 0; i < MUX_MAX_CONN; i++) {
        if(s_conns[i].state == CONN_FREE) {
            conn = &s_conns[i];
            break;
        }
    }
    if(conn == NULL) {
        xSemaphoreGive(s_conn_lock);
        return ESP_ERR_NO_MEM;
    }

    uint16_t sport = s_next_sport++;
    if(s_next_sport == 0) s_next_sport = 0x4000;

    conn->state = CONN_SYN_SENT;
    conn->sport = sport;
    conn->dport = port;
    conn->tx_seq = 0;
    conn->rx_ack = 0;
    conn->peer_win = 0;
    xStreamBufferReset(conn->rx);
    xSemaphoreTake(conn->open_sem, 0);

    ESP_LOGI(TAG, "TCP connecting %u->%u", sport, port);
    esp_err_t err = send_tcp_segment(conn, TCP_SYN, 0, 0, NULL, 0);
    xSemaphoreGive(s_conn_lock);

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "TCP SYN enqueue failed: %s", esp_err_to_name(err));
        xSemaphoreTake(s_conn_lock, portMAX_DELAY);
        conn->state = CONN_FREE;
        xSemaphoreGive(s_conn_lock);
        return err;
    }

    if(xSemaphoreTake(conn->open_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "TCP %u->%u connect timeout", sport, port);
        xSemaphoreTake(s_conn_lock, portMAX_DELAY);
        if(conn->state == CONN_SYN_SENT) {
            conn->state = CONN_FREE;
            conn->sport = 0;
            conn->dport = 0;
        }
        xSemaphoreGive(s_conn_lock);
        return ESP_ERR_TIMEOUT;
    }

    if(conn->state != CONN_ESTABLISHED) {
        xSemaphoreTake(s_conn_lock, portMAX_DELAY);
        conn->state = CONN_FREE;
        conn->sport = 0;
        conn->dport = 0;
        xSemaphoreGive(s_conn_lock);
        return ESP_FAIL;
    }

    *out = conn;
    return ESP_OK;
}

int usbmux_conn_send(usbmux_conn_t * conn, const void * data, size_t len)
{
    if(conn == NULL || (data == NULL && len > 0)) return -1;

    const uint8_t * p = (const uint8_t *)data;
    size_t sent = 0;

    while(sent < len) {
        size_t chunk = len - sent;
        if(chunk > USB_MRU - 16 - 20) {
            chunk = USB_MRU - 16 - 20;
        }

        xSemaphoreTake(s_conn_lock, portMAX_DELAY);
        if(conn->state != CONN_ESTABLISHED) {
            xSemaphoreGive(s_conn_lock);
            return -1;
        }
        esp_err_t err = send_tcp_segment(conn, TCP_ACK, conn->tx_seq,
                                         conn->rx_ack, p + sent, chunk);
        if(err == ESP_OK) {
            conn->tx_seq += chunk;
        }
        xSemaphoreGive(s_conn_lock);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "TCP send enqueue failed: %s", esp_err_to_name(err));
            return -1;
        }
        sent += chunk;
    }

    return (int)sent;
}

int usbmux_conn_recv(usbmux_conn_t * conn, void * data, size_t len,
                     uint32_t timeout_ms)
{
    if(conn == NULL || data == NULL || len == 0) return -1;

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    for(;;) {
        size_t n = xStreamBufferReceive(conn->rx, data, len,
                                        pdMS_TO_TICKS(25));
        if(n > 0) return (int)n;

        if(conn->state != CONN_ESTABLISHED) {
            n = xStreamBufferReceive(conn->rx, data, len, 0);
            return (n > 0) ? (int)n : -1;
        }
        if((xTaskGetTickCount() - start) >= timeout) {
            return 0;
        }
    }
}

esp_err_t usbmux_conn_recv_exact(usbmux_conn_t * conn, void * data, size_t len,
                                 uint32_t timeout_ms)
{
    if(conn == NULL || data == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t * p = (uint8_t *)data;
    size_t got = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while(got < len) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if(elapsed >= timeout) return ESP_ERR_TIMEOUT;
        int n = usbmux_conn_recv(conn, p + got, len - got,
                                 (uint32_t)((timeout - elapsed) *
                                            portTICK_PERIOD_MS));
        if(n < 0) return ESP_FAIL;
        if(n == 0) return ESP_ERR_TIMEOUT;
        got += (size_t)n;
    }
    return ESP_OK;
}

bool usbmux_conn_is_open(const usbmux_conn_t * conn)
{
    return conn != NULL && conn->state == CONN_ESTABLISHED;
}

void usbmux_conn_close(usbmux_conn_t * conn)
{
    if(conn == NULL) return;

    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    if(conn->state == CONN_ESTABLISHED) {
        send_tcp_segment(conn, TCP_RST | TCP_ACK, conn->tx_seq, conn->rx_ack,
                         NULL, 0);
    }
    conn->state = CONN_FREE;
    conn->sport = 0;
    conn->dport = 0;
    conn->tx_seq = 0;
    conn->rx_ack = 0;
    xStreamBufferReset(conn->rx);
    xSemaphoreGive(conn->open_sem);
    xSemaphoreGive(s_conn_lock);
}

bool usbmux_host_wait_ready(uint32_t timeout_ms)
{
    if(usbmux_host_is_ready()) return true;
    if(xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    return usbmux_host_is_ready();
}

esp_err_t usbmux_host_start(void)
{
    s_mux_buf = heap_caps_malloc(MUX_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if(s_mux_buf == NULL) return ESP_ERR_NO_MEM;

    s_ready_sem = xSemaphoreCreateBinary();
    s_conn_lock = xSemaphoreCreateMutex();
    s_tx_alloc_lock = xSemaphoreCreateMutex();
    s_tx_done = xSemaphoreCreateBinary();
    s_tx_queue = xQueueCreate(MUX_TX_QUEUE, sizeof(mux_tx_item_t));
    if(s_ready_sem == NULL || s_conn_lock == NULL || s_tx_alloc_lock == NULL ||
       s_tx_done == NULL || s_tx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for(int i = 0; i < MUX_MAX_CONN; i++) {
        struct usbmux_conn * conn = &s_conns[i];
        conn->rx = xStreamBufferCreateWithCaps(MUX_CONN_BUF, 0,
                                               MALLOC_CAP_SPIRAM);
        conn->open_sem = xSemaphoreCreateBinary();
        if(conn->rx == NULL || conn->open_sem == NULL) {
            ESP_LOGE(TAG, "connection buffer allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    esp_err_t err = usb_host_client_register(&cfg, &s_client);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "client register failed: %s", esp_err_to_name(err));
        return err;
    }

    if(xTaskCreate(client_task, "usbmux_evt", 6144, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if(xTaskCreate(mux_tx_task, "usbmux_tx", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* A device may already be present when the client is registered. */
    uint8_t addresses[8];
    int count = 0;
    if(usb_host_device_addr_list_fill(8, addresses, &count) == ESP_OK) {
        for(int i = 0; i < count; i++) {
            open_device(addresses[i]);
        }
    }

    ESP_LOGI(TAG, "usbmux client started");
    return ESP_OK;
}

esp_err_t usbmux_host_dump_configs(void)
{
    if(s_dev == NULL) {
        ESP_LOGW(TAG, "no iPod open");
        return ESP_ERR_INVALID_STATE;
    }

    const usb_device_desc_t * dev_desc = NULL;
    esp_err_t err = usb_host_get_device_descriptor(s_dev, &dev_desc);
    if(err != ESP_OK || dev_desc == NULL) return err;

    ESP_LOGI(TAG, "device %04x:%04x configs=%u", dev_desc->idVendor,
             dev_desc->idProduct, (unsigned)dev_desc->bNumConfigurations);

    for(uint8_t value = 1; value <= dev_desc->bNumConfigurations; value++) {
        const usb_config_desc_t * cfg = NULL;
        err = usb_host_get_config_desc(s_client, s_dev, value, &cfg);
        if(err != ESP_OK || cfg == NULL) {
            ESP_LOGW(TAG, "config %u: read failed (%s)", value,
                     esp_err_to_name(err));
            continue;
        }

        ESP_LOGI(TAG, "config %u: value=%u interfaces=%u attr=0x%02x "
                 "maxpower=%umA", value, (unsigned)cfg->bConfigurationValue,
                 (unsigned)cfg->bNumInterfaces, cfg->bmAttributes,
                 (unsigned)cfg->bMaxPower * 2);

        for(uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
            int alts = usb_parse_interface_number_of_alternate(cfg, i);
            if(alts < 1) alts = 1;
            for(int a = 0; a < alts; a++) {
                int intf_offset = 0;
                const usb_intf_desc_t * intf =
                    usb_parse_interface_descriptor(cfg, i, (uint8_t)a,
                                                   &intf_offset);
                if(intf == NULL) continue;

                ESP_LOGI(TAG, "  if=%u alt=%u class=%02x/%02x/%02x "
                         "eps=%u", intf->bInterfaceNumber,
                         intf->bAlternateSetting, intf->bInterfaceClass,
                         intf->bInterfaceSubClass,
                         intf->bInterfaceProtocol,
                         (unsigned)intf->bNumEndpoints);

                for(int e = 0; e < intf->bNumEndpoints; e++) {
                    int offset = intf_offset;
                    const usb_ep_desc_t * ep =
                        usb_parse_endpoint_descriptor_by_index(
                            intf, e, cfg->wTotalLength, &offset);
                    if(ep == NULL) continue;
                    ESP_LOGI(TAG, "    ep=0x%02x attr=0x%02x mps=%u "
                             "interval=%u", ep->bEndpointAddress,
                             ep->bmAttributes, ep->wMaxPacketSize,
                             ep->bInterval);
                }
            }
        }

        usb_host_free_config_desc(cfg);
    }
    return ESP_OK;
}

bool usbmux_host_is_ready(void)
{
    return s_mux_active && s_dev != NULL;
}

const char * usbmux_host_status(void)
{
    return s_status;
}
