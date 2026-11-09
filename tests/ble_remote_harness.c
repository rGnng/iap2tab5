/* Fake GAP/GATT, NVS and clock boundaries. The production ESP backend is
 * included by test_ble_remote.py; no alternative lifecycle implementation. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ble_remote.h"
#include "ulanzi_d100h.h"
#define ESP_LOG_DEBUG 4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_OK 0
#define ESP_FAIL 1
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_ADDR_PUBLIC 0
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EALREADY 2
#define BLE_HS_EINVAL 3
#define BLE_HS_EDONE 14
#define BLE_ERR_REM_USER_CONN_TERM 19
#define BLE_UUID_TYPE_16 16
#define BLE_UUID_TYPE_128 128
#define BLE_GATT_DSC_CLT_CFG_UUID16 0x2902
#define BLE_SM_IO_CAP_NO_IO 0
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define portMAX_DELAY 0
#define D100H_INPUT_BLE 1
#define BLE_GAP_EVENT_DISC 1
#define BLE_GAP_EVENT_DISC_COMPLETE 2
#define BLE_GAP_EVENT_CONNECT 3
#define BLE_GAP_EVENT_ENC_CHANGE 4
#define BLE_GAP_EVENT_PASSKEY_ACTION 5
#define BLE_GAP_EVENT_DISCONNECT 6
#define BLE_GAP_EVENT_NOTIFY_RX 7
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef void * SemaphoreHandle_t;
typedef struct { uint8_t type, val[6]; } ble_addr_t;
typedef struct { int type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; ble_uuid128_t u128; } ble_uuid_any_t;
struct ble_npl_event { int unused; };
struct ble_npl_callout { int unused; };
struct ble_gatt_error { int status; };
struct ble_gatt_svc { ble_uuid_any_t uuid; uint16_t start_handle, end_handle; };
struct ble_gatt_chr { ble_uuid_any_t uuid; uint16_t val_handle; int properties; };
struct ble_gatt_dsc { ble_uuid_any_t uuid; uint16_t handle; };
struct ble_gatt_attr { struct fake_mbuf *om; };
struct ble_gap_disc_desc { uint8_t *data; uint8_t length_data; ble_addr_t addr; int rssi; };
struct ble_gap_disc_params { int itvl, window, passive, filter_duplicates; };
struct fake_mbuf { int len; uint8_t data[20]; };
struct ble_gap_event {
    int type;
    struct ble_gap_disc_desc disc;
    struct { int reason; } disc_complete;
    struct { int status; uint16_t conn_handle; } connect;
    struct { int status; uint16_t conn_handle; } enc_change;
    struct { struct { int action; uint32_t numcmp; } params; } passkey;
    struct { int reason; struct { uint16_t conn_handle; } conn; } disconnect;
    struct { uint16_t conn_handle, attr_handle; struct fake_mbuf *om; } notify_rx;
};
static struct {
    void (*reset_cb)(int); void (*sync_cb)(void); void *store_status_cb;
    int sm_io_cap, sm_bonding, sm_mitm, sm_sc, sm_our_key_dist, sm_their_key_dist;
} ble_hs_cfg;
static int64_t clock_us;
static int connect_rc, write_rc, connects, terminates, cancels, writes, gestures;
static int side_events,side_source,side_id,side_down;
static bool connecting, scanning;
static int probe_reads, subscribe_writes, probe_rc;
static int (*probe_cb)(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *);
static void *probe_arg;
static uint8_t last_write[20];
static int64_t esp_timer_get_time(void) { return clock_us; }
static void *xSemaphoreCreateMutex(void) { return (void *)1; }
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
static int ble_addr_cmp(const ble_addr_t *a, const ble_addr_t *b) { return memcmp(a,b,sizeof(*a)); }
#define ble_uuid_cmp(...) 1
#define ble_uuid_to_str(...) ((void)0)
#define nvs_open(...) ESP_FAIL
#define nvs_get_str(...) ESP_FAIL
#define nvs_set_str(...) 0
#define nvs_commit(...) 0
#define nvs_close(...) ((void)0)
static bool ble_gap_conn_active(void) { return connecting; }
static bool ble_gap_disc_active(void) { return scanning; }
static int ble_gap_connect(uint8_t own, const ble_addr_t *addr, int ms, void *params,
                           int (*cb)(struct ble_gap_event *,void *),void *arg)
{ connects++;connecting=connect_rc==0;return connect_rc; }
static int ble_gap_conn_cancel(void) { cancels++;connecting=false;return 0; }
static int ble_gap_terminate(int conn,int reason) { terminates++;return 0; }
static int ble_gap_disc(int own,int ms,void *params,void *cb,void *arg) { scanning=true;return 0; }
static int ble_gap_disc_cancel(void) { scanning=false;return 0; }
#define ble_gap_security_initiate(...) 0
#define ble_gattc_disc_all_svcs(...) 0
#define ble_gattc_disc_all_chrs(...) 0
#define ble_gattc_disc_all_dscs(...) 0
static int ble_gattc_read(uint16_t conn,uint16_t handle,
    int (*cb)(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *),void *arg)
{ assert(handle==78);probe_reads++;probe_cb=cb;probe_arg=arg;return probe_rc; }
static int ble_gattc_write_flat(uint16_t conn,uint16_t handle,const void *data,int len,
    int (*cb)(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *),void *arg)
{ subscribe_writes++;probe_cb=cb;probe_arg=arg;return probe_rc; }
static int ble_gattc_write_no_rsp_flat(int conn,int handle,const void *data,size_t len)
{ writes++;memcpy(last_write,data,20);return write_rc; }
#define ble_npl_callout_stop(...) ((void)0)
#define ble_npl_callout_reset(...) ((void)0)
#define ble_npl_callout_init(...) ((void)0)
#define ble_npl_time_ms_to_ticks32(ms) (ms)
#define ble_hs_util_ensure_addr(...) 0
#define ble_hs_id_infer_auto(...) 0
#define esp_hosted_connect_to_slave(...) 0
#define esp_hosted_bt_controller_init(...) 0
#define esp_hosted_bt_controller_enable(...) 0
#define esp_err_to_name(...) "fake"
#define nimble_port_init(...) 0
#define nimble_port_run(...) ((void)0)
#define nimble_port_freertos_init(...) ((void)0)
#define nimble_port_freertos_deinit(...) ((void)0)
#define ble_store_util_status_rr NULL
#define ble_store_util_delete_peer(...) 0
void ble_store_config_init(void) {}
#define OS_MBUF_PKTLEN(m) ((m)->len)
static int os_mbuf_copydata(struct fake_mbuf *m,int start,int len,void *out) { memcpy(out,m->data+start,len);return 0; }
static void usb_hid_input_cancel_gestures(int source) { gestures++; }
static void usb_hid_input_post_side(int source,uint8_t id,bool down)
{ side_events++;side_source=source;side_id=id;side_down=down; }
#define usb_hid_input_post_back(...) ((void)0)
#define usb_hid_input_post_play(...) ((void)0)
#define usb_hid_input_post_top_right(...) ((void)0)
#define usb_hid_input_post_knob(...) ((void)0)
#define usb_hid_input_post_rotary(...) ((void)0)
#define usb_hid_input_post_key_state(...) ((void)0)
#define usb_hid_input_post_key(...) ((void)0)
#include "backend_under_test.c"
static void tick(int64_t now) { clock_us=now;heartbeat_callout(NULL); }
static void reset(void)
{
    clock_us=1000000;connecting=scanning=false;connect_rc=write_rc=0;
    connects=terminates=cancels=writes=gestures=0;
    side_events=0;side_source=side_id=side_down=-1;
    probe_reads=subscribe_writes=probe_rc=0;probe_cb=NULL;probe_arg=NULL;
    memset(&s_link_stats,0,sizeof(s_link_stats));
    s_enabled=s_nimble_ready=true;s_have_target=true;s_disconnect_requested=false;
    s_reconnect=true;s_reconnect_at=0;s_retry_count=0;
    s_scan_pending=s_connect_pending=s_scan_stop_pending=false;s_forget_count=0;
    s_conn_handle=BLE_HS_CONN_HANDLE_NONE;clear_link();
    addr_from_str("CA:63:45:7D:8C:B9",&s_target_addr);s_target_addr.type=1;
    s_pending_addr=s_target_addr;s_paired_count=1;s_paired_type[0]=1;
    strcpy(s_paired[0].addr,"CA:63:45:7D:8C:B9");strcpy(s_paired[0].name,"Ulanzi Dial");
    s_state=BLE_REMOTE_IDLE;
}
static void linked(void)
{
    tick(clock_us);
    struct ble_gap_event ev={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=0,.conn_handle=7}};
    connecting=false;gap_event(&ev,NULL);
    s_vendor_write_handle=80;s_vendor_notify_handle=77;s_vendor_cccd_handle=78;
    struct ble_gatt_error ok={0};on_write(7,&ok,NULL,NULL);
    assert(s_info.connected && s_sleep_phase==1);
}
static void notify(const uint8_t *plain,size_t len,int conn)
{
    struct fake_mbuf m={.len=20};ulanzi_d100h_encode_ble(plain,len,m.data);
    struct ble_gap_event ev={.type=BLE_GAP_EVENT_NOTIFY_RX,
        .notify_rx={.conn_handle=conn,.attr_handle=77,.om=&m}};
    gap_event(&ev,NULL);
}
#define NOTIFY(...) do { const uint8_t p[]={__VA_ARGS__};notify(p,sizeof(p),7); } while(0)
static void expect_write(const uint8_t *p,size_t len)
{ uint8_t wire[20];ulanzi_d100h_encode_ble(p,len,wire);assert(memcmp(wire,last_write,20)==0); }
static void sleep_off(void) { NOTIFY(1,1,0x42,0x11,0,0);assert(s_info.sleep_disabled); }
static void respond_probe(int status)
{
    struct ble_gatt_error error={.status=status};
    struct fake_mbuf value={.len=2,.data={1,0}};
    struct ble_gatt_attr attr={.om=&value};
    probe_cb(7,&error,&attr,probe_arg);
}
int main(void)
{
    reset();linked();
    for(int id=4;id<=6;id+=2) {
        for(int down=1;down>=0;down--) {
            int before=side_events;
            NOTIFY(0x8b,0x10,id,down);
            assert(side_events==before+1 && side_source==D100H_INPUT_BLE);
            assert(side_id==id && side_down==down);
        }
    }

    reset();connect_rc=4;
    for(int i=0;i<40;i++) tick(clock_us+16000000);
    assert(connects==40 && s_reconnect && s_state==BLE_REMOTE_IDLE);

    reset();tick(clock_us);assert(connecting);
    struct ble_gap_event failed={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=3}};
    connecting=false;gap_event(&failed,NULL);tick(clock_us+3000000);
    assert(connects==2 && connecting);

    reset();linked();assert(writes==2); /* Immediate heartbeat, then sleep read. */
    NOTIFY(1,1,0x42,0x11,0x20,0x1c);
    const uint8_t off[]={1,1,0x42,2,0,0};expect_write(off,sizeof(off));
    assert(!s_info.sleep_disabled && s_sleep_phase==2);
    NOTIFY(1,1,0x42,0x12,0,0);
    const uint8_t read[]={1,1,0x42,1};expect_write(read,sizeof(read));
    assert(!s_info.sleep_disabled && s_sleep_phase==3);
    sleep_off();assert(s_sleep_phase==0);
    NOTIFY(0x0b,0x7b,0x1c,0x10,8,75);
    ble_remote_info_t info;ble_remote_get_info(s_current_addr,&info);
    assert(info.connected && info.battery_valid && info.battery_percent==75 && info.charging);
    ble_remote_get_info("00:00:00:00:00:00",&info);assert(!info.connected && !info.battery_valid);
    uint32_t rev=s_rev;NOTIFY(0x0b,0x7b,0x1c,0x10,8,75);assert(s_rev==rev);
    NOTIFY(0x0b,0x7b,0x1c,0x10,0,74);assert(s_info.battery_percent==74 && !s_info.charging);
    uint8_t stale[]={0x0b,0x7b,0,0,8,20};notify(stale,sizeof(stale),99);assert(s_info.battery_percent==74);

    tick(clock_us+10000000);assert(terminates==0 && probe_reads==1 && s_probe_phase==1);
    tick(clock_us+10000000);assert(terminates==1 && !s_vendor_ready && !s_info.battery_valid && gestures>0);
    struct ble_gap_event disconnected={.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={.conn={.conn_handle=7}}};
    gap_event(&disconnected,NULL);tick(clock_us+3000000);assert(connects==2);

    reset();linked();sleep_off();
    for(int i=0;i<60;i++) { clock_us+=1000000;NOTIFY(6,1,0x23,0x10,1);tick(clock_us); }
    assert(terminates==0 && s_info.connected);
    write_rc=4;for(int i=0;i<3;i++) tick(clock_us+1000000);
    assert(terminates==1);

    reset();linked();
    for(int i=0;i<10;i++) { clock_us+=2000000;NOTIFY(6,1,0x23,0x10,1);tick(clock_us); }
    assert(terminates>=1 && !s_info.sleep_disabled);

    reset();linked();
    for(int i=0;i<7;i++) { clock_us+=1000000;NOTIFY(6,1,0x23,0x10,1);tick(clock_us); }
    assert(terminates==0);sleep_off();assert(s_info.connected);

    reset();tick(clock_us);connecting=false;
    struct ble_gap_event connected={.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=7}};
    gap_event(&connected,NULL);tick(clock_us+20000000);assert(terminates==1);

    reset();tick(clock_us);ble_remote_set_enabled(false);tick(clock_us);
    assert(cancels==1 && s_state==BLE_REMOTE_OFF && !s_reconnect);
    gap_event(&connected,NULL);assert(terminates==1 && !s_vendor_ready);
    gap_event(&disconnected,NULL);tick(clock_us+60000000);
    assert(connects==1 && s_state==BLE_REMOTE_OFF);

    reset();linked();sleep_off();ble_remote_set_enabled(false);ble_remote_set_enabled(true);
    tick(clock_us);assert(terminates==1);gap_event(&disconnected,NULL);
    tick(clock_us+3000000);assert(connects==2);

    reset();linked();sleep_off();ble_remote_unpair("CA:63:45:7D:8C:B9");tick(clock_us);
    assert(terminates==1 && !s_have_target && s_paired_count==0);
    gap_event(&disconnected,NULL);tick(clock_us+60000000);assert(connects==1);

    reset();on_reset(3);assert(!s_nimble_ready && !s_info.battery_valid);
    on_sync();tick(clock_us);assert(connects==1 && s_target_addr.type==1);
    reset();linked();sleep_off();ble_remote_scan_start();tick(clock_us);
    assert(scanning && s_info.connected);ble_remote_scan_stop();tick(clock_us);
    assert(!scanning && s_info.connected && s_state==BLE_REMOTE_CONNECTED);
    ble_remote_connect("CA:63:45:7D:8C:B9");tick(clock_us);
    assert(terminates==0 && s_state==BLE_REMOTE_CONNECTED);

    reset();tick(clock_us);ble_remote_scan_start();tick(clock_us);
    assert(cancels==1);connecting=false;gap_event(&failed,NULL);tick(clock_us);
    assert(scanning);ble_remote_scan_stop();tick(clock_us+3000000);
    assert(connects==2 && connecting);

    /* Healthy idle peers need no periodic vendor response. Real ATT replies
     * and an acknowledged subscription refresh prove liveness for 12 minutes. */
    reset();linked();sleep_off();
    for(int i=0;i<720;i++) {
        tick(clock_us+1000000);
        if(s_probe_phase==1)respond_probe(0);
        if(s_probe_phase==2)respond_probe(0);
    }
    assert(terminates==0 && s_info.connected && s_info.sleep_disabled);
    assert(s_link_stats.link_checks>=60 && probe_reads==subscribe_writes);
    assert(s_link_stats.heartbeat_replies==0 && writes>=720);

    /* Key/status traffic is also liveness even without recognized heartbeat
     * payloads. Unknown future vendor frames must not trigger a disconnect. */
    reset();linked();sleep_off();
    for(int i=0;i<120;i++) {
        clock_us+=1000000;NOTIFY(6,1,0x23,0x10,2);tick(clock_us);
    }
    assert(terminates==0 && probe_reads==0 && s_link_stats.notifications==121);

    reset();linked();sleep_off();tick(clock_us+10000000);
    void *old_probe=probe_arg;
    NOTIFY(6,1,0x23,0x10,1);assert(s_probe_phase==0);
    tick(clock_us+10000000);assert(s_probe_phase==1);
    struct ble_gatt_error ok={0};
    on_link_probe(7,&ok,NULL,old_probe);assert(s_probe_phase==1);
    respond_probe(0);assert(s_probe_phase==2);respond_probe(0);
    assert(terminates==0 && s_link_stats.link_checks==1);
    ble_remote_link_stats_t stats;ble_remote_get_link_stats(&stats);
    assert(stats.heartbeat_replies==1 && stats.heartbeat_age_ms==10000);

    reset();linked();sleep_off();probe_rc=4;
    for(int i=0;i<3;i++)tick(clock_us+10000000);
    assert(terminates==1 && strcmp(s_link_stats.last_recovery,"GATT link probe failed")==0);

    reset();linked();sleep_off();tick(clock_us+10000000);
    respond_probe(5);assert(s_probe_phase==0 && s_probe_failures==1 && terminates==0);
    tick(clock_us+10000000);respond_probe(0);assert(s_probe_phase==2);
    respond_probe(5);assert(s_probe_phase==0 && s_probe_failures==2 && terminates==0);
    tick(clock_us+10000000);respond_probe(5);assert(terminates==1);

    reset();linked();sleep_off();tick(clock_us+10000000);
    old_probe=probe_arg;ble_remote_set_enabled(false);
    on_link_probe(7,&ok,NULL,old_probe);
    assert(s_probe_phase==1 && s_state==BLE_REMOTE_OFF && terminates==0);
    tick(clock_us);assert(terminates==1);

    /* The official SDK has a separate standby timer. Disable it only after
     * a supported read; a write ack alone never counts as verified readback. */
    reset();linked();sleep_off();
    tick(clock_us);
    const uint8_t standby_read[]={1,1,0x2c,1};expect_write(standby_read,sizeof(standby_read));
    NOTIFY(1,1,0x2c,0x11,0xb4,0,0,0);
    const uint8_t standby_off[]={1,1,0x2c,2,0,0,0,0};expect_write(standby_off,sizeof(standby_off));
    assert(s_info.standby_time_valid && s_info.standby_seconds==180 && s_standby_phase==2);
    NOTIFY(1,1,0x2c,0x12,0,0,0,0);
    expect_write(standby_read,sizeof(standby_read));
    assert(s_info.standby_seconds==180 && s_standby_phase==3);
    NOTIFY(1,1,0x2c,0x11,0,0,0,0);
    assert(s_info.standby_seconds==0 && s_standby_phase==0);
    NOTIFY(1,1,0x0d,0x11,1);
    assert(s_info.standby_state_valid && s_info.standby);
    clear_link();
    assert(!s_info.standby_time_valid && !s_info.standby_state_valid);

    /* Unsupported standby commands expire without disconnecting or claiming
     * success. A responsive peer can still deliver controls and heartbeat. */
    reset();linked();sleep_off();
    for(int i=0;i<25;i++) {
        tick(clock_us+1000000);
        NOTIFY(6,1,0x23,0x10,1);
    }
    assert(s_standby_phase==0 && terminates==0 && s_info.connected && !s_info.standby_time_valid);

    /* An already-disabled timer must not be rewritten on every reconnect. */
    reset();linked();sleep_off();tick(clock_us);
    int before_standby_reply=writes;
    NOTIFY(1,1,0x2c,0x11,0,0,0,0);
    const uint8_t state_read[]={1,1,0x0d,1};expect_write(state_read,sizeof(state_read));
    assert(writes==before_standby_reply+1 && s_standby_phase==0);

    /* A missing write ack must not claim zero or repeatedly reconnect a
     * healthy device. Keep its last actual readback visible to diagnostics. */
    reset();linked();sleep_off();tick(clock_us);
    NOTIFY(1,1,0x2c,0x11,0xb4,0,0,0);
    for(int i=0;i<25;i++) {
        tick(clock_us+1000000);
        NOTIFY(6,1,0x23,0x10,1);
    }
    assert(s_standby_phase==0 && terminates==0 && s_info.standby_seconds==180);

    puts("BLE lifecycle: persistent retry, sleep, battery, stalls, off/on, unpair and reset passed");
    return 0;
}
