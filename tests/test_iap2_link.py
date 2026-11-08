"""Exercise production iAP2 transport functions with bounded USB/RTOS fakes."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from test_album_view import function

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iap2_host.h"
#define LINK_START 0xff5a
#define CONTROL_SYN 128
#define CONTROL_ACK 64
#define CONTROL_EAK 32
#define CONTROL_RST 16
#define CONTROL_SESSION 10
#define EA_SESSION 11
#define FT_SESSION 12
#define LINK_IDLE 0
#define LINK_DETECT 1
#define LINK_NEGOTIATE 2
#define LINK_NORMAL 3
#define LINK_BUF_SIZE (256*1024)
#define CSM_BUF_SIZE (192*1024)
#define MAX_CSM_FRAME 2048
#define LINK_MAX_PACKET 4096
#define SESSION_QUEUE_LEN 32
#define RX_PACKET_BUDGET 16
#define RX_TIME_BUDGET_US 4000
#define IN_XFER_SIZE 64
#define TX_QUEUE_LEN 8
#define IAP2_MARKER_LEN 6
#define MALLOC_CAP_SPIRAM 0
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define USB_TRANSFER_STATUS_COMPLETED 0
static const uint8_t IAP2_MARKER[]={255,85,2,0,238,16};
typedef uint32_t TickType_t;
typedef struct { uint8_t *p; size_t len, cap; } pb_t;
typedef struct { uint32_t generation; uint16_t len; uint8_t session; uint8_t payload[]; } session_packet_t;
typedef struct { int status, actual_num_bytes, num_bytes; uint8_t data_buffer[64]; } usb_transfer_t;
static usb_transfer_t *s_in_xfer;
static size_t s_report_remaining, s_link_len, s_csm_len;
static uint8_t link_buf[LINK_BUF_SIZE],csm_buf[CSM_BUF_SIZE];
static uint8_t *s_link_buf=link_buf,*s_csm_buf=csm_buf;
static uint32_t s_link_generation;
static bool s_reset_pending,s_rx_overflow,s_connected,s_rx_more,s_partial_waiting;
static TickType_t s_partial_since;
static void *s_task;
static void xTaskNotifyGive(void *t){(void)t;}
static int s_rx_stream,s_session_queue;
static void *s_session_lock,*s_tx_lock;
static void *s_ctrl_lock=(void*)1,*s_ctrl_done=(void*)2;
static bool s_ctrl_inflight;
static int ctrl_completion,ctrl_held;
static int s_state;
static uint8_t s_sent_psn,s_last_recv_psn,s_tx_seq;
static uint16_t s_peer_max_len,s_peer_retransmit,s_peer_ack_timeout;
static TickType_t s_next_marker,s_next_syn,s_last_rx,s_tx_deadline;
static uint8_t *s_tx_active,*s_txq[TX_QUEUE_LEN];
static size_t s_tx_active_len,s_tx_offset,s_tx_packet_len,s_txq_len[TX_QUEUE_LEN];
static uint8_t s_tx_active_session,s_txq_session[TX_QUEUE_LEN];
static int s_tx_retries,s_txq_tail,s_txq_head;
static bool s_tx_busy,s_hid_ready,s_identified,s_auth_ok,s_playing;
static char s_title[128],s_artist[128],s_album[128];
static uint64_t s_track_key;
static char s_library_id[128],s_library_sub_id[128],s_library_revision[128];
static bool s_library_subscribed,s_library_resynced;
static int s_library_items;
static iap2_link_stats_t s_stats;
static uint8_t stream[128*1024];
static size_t stream_len,stream_cap=sizeof(stream);
static session_packet_t *queue[32],*held;
static unsigned q_count,q_cap=32,delivered,csms,ack_count,data_count;
static uint32_t now_ms,last_ack,last_seq,time_step;
static uint64_t micros,csum;
static bool oom,write_error;
static uint8_t delivered_data[65536],tx_data[65536];
static size_t delivered_len,tx_len;
static int xSemaphoreTake(void *a,int b){(void)b;if(a==s_ctrl_done){int r=ctrl_completion;ctrl_completion=0;return r;}if(a==s_ctrl_lock)ctrl_held++;return 1;}
static void xSemaphoreGive(void *a){if(a==s_ctrl_lock)ctrl_held--;}
static size_t xStreamBufferSpacesAvailable(int s){(void)s;return stream_cap-stream_len;}
static size_t xStreamBufferSend(int s,const void *p,size_t n,int t){
    (void)s;(void)t;if(n>stream_cap-stream_len)n=stream_cap-stream_len;
    memcpy(stream+stream_len,p,n);stream_len+=n;return n;
}
static size_t xStreamBufferReceive(int s,void *p,size_t n,int t){
    (void)s;(void)t;if(n>stream_len)n=stream_len;memcpy(p,stream,n);
    memmove(stream,stream+n,stream_len-n);stream_len-=n;return n;
}
static unsigned uxQueueSpacesAvailable(int q){(void)q;return q_cap-q_count;}
static unsigned uxQueueMessagesWaiting(int q){(void)q;return q_count;}
static int xQueueSend(int q,const void *p,int t){
    (void)q;(void)t;if(q_count>=q_cap)return 0;queue[q_count++]=*(session_packet_t *const*)p;return 1;
}
static int xQueueReceive(int q,void *p,int t){
    (void)q;(void)t;if(!q_count)return 0;*(session_packet_t**)p=queue[0];
    memmove(queue,queue+1,--q_count*sizeof(*queue));return 1;
}
static void *heap_caps_malloc(size_t n,int cap){(void)cap;return oom?NULL:malloc(n);}
static void csm_queue(const uint8_t *p,size_t n){assert(n<=sizeof(tx_data));memcpy(tx_data,p,n);tx_len=n;}
static int64_t esp_timer_get_time(void){micros+=time_step;return micros;}
static TickType_t xTaskGetTickCount(void){return now_ms;}
static void artwork_invalidate(void){}
static void ft_clear_all(void){}
static void set_status(const char*s){(void)s;}
static int usb_host_transfer_submit(usb_transfer_t*x){(void)x;return 0;}
static void usb_host_transfer_free(usb_transfer_t*x){(void)x;}
static int link_send(uint8_t ctl,uint8_t seq,uint8_t ack,uint8_t session,const uint8_t*p,size_t n){
    (void)session;last_ack=ack;last_seq=seq;
    if(n){data_count++;assert(tx_len+n<=sizeof(tx_data));memcpy(tx_data+tx_len,p,n);tx_len+=n;}
    else if(ctl==CONTROL_ACK)ack_count++;
    return write_error?-1:0;
}
static void handle_csm(uint16_t msg,const uint8_t *p,size_t n){
    csms++;if(msg==0x4c04&&n==4)csum+=(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static void csm_feed(const uint8_t *,size_t);
static void link_data(const uint8_t*p,size_t n,uint8_t session){
    delivered++;assert(n<=sizeof(delivered_data));memcpy(delivered_data,p,n);delivered_len=n;
    if(session==CONTROL_SESSION)csm_feed(p,n);
}
'''
DRIVER = r'''
void setup(void){
    reset_link();if(held){free(held);held=NULL;}
    memset(&s_stats,0,sizeof(s_stats));s_state=LINK_NORMAL;s_connected=true;
    s_last_recv_psn=0;s_sent_psn=99;s_report_remaining=0;
    stream_len=0;stream_cap=sizeof(stream);q_cap=32;
    delivered=csms=ack_count=data_count=now_ms=last_ack=last_seq=0;
    micros=csum=delivered_len=tx_len=0;time_step=1;oom=write_error=false;s_rx_overflow=false;s_reset_pending=false;
}
void feed(const uint8_t*p,size_t n){assert(xStreamBufferSend(0,p,n,0)==n);process_rx();}
void usb_feed(const uint8_t*p,size_t n){
    usb_transfer_t x={.actual_num_bytes=(int)n};assert(n<=64);memcpy(x.data_buffer,p,n);s_in_xfer=&x;in_done_cb(&x);process_rx();
}
void stale_usb(void){usb_transfer_t x={.status=1};s_in_xfer=NULL;in_done_cb(&x);assert(!s_reset_pending);}
int parse_peer(const uint8_t*p,size_t n){return parse_lsp(p,n);}
void drain(void){session_packet_t*p;while(xQueueReceive(0,&p,0))session_deliver(p);}
void hold(void){assert(xQueueReceive(0,&held,0));}
void deliver_held(void){session_deliver(held);held=NULL;}
void reset(void){reset_link();}
void pump(void){process_rx();}
void tx(void){tx_pump();}
void tick(uint32_t n){now_ms+=n;}
void settings(int id,int value){
    switch(id){case 0:q_cap=value;break;case 1:stream_cap=value;break;case 2:s_last_recv_psn=value;break;
    case 3:s_peer_max_len=value;break;case 4:write_error=value;break;case 5:time_step=value;break;
    case 6:oom=value;break;case 7:s_state=value;break;}
}
uint64_t value(int id){
    switch(id){case 0:return q_count;case 1:return delivered;case 2:return ack_count;
    case 3:return last_ack;case 4:return s_last_recv_psn;case 5:return s_link_len;
    case 6:return s_state;case 7:return s_stats.bad_headers;case 8:return s_stats.bad_payloads;
    case 9:return s_stats.rx_overflows;case 10:return s_stats.sequence_gaps;case 11:return s_stats.duplicates;
    case 12:return s_stats.queue_stalls;case 13:return s_stats.link_resets;case 14:return s_stats.hid_errors;
    case 15:return csms;case 16:return csum;case 17:return s_tx_busy;case 18:return last_seq;
    case 19:return data_count;case 20:return s_stats.retransmits;case 21:return s_report_remaining;
    case 22:return s_stats.rx_packets;case 23:return s_csm_len;case 24:return s_peer_max_len;default:return 0;}
}
size_t copy_data(uint8_t*p){memcpy(p,delivered_data,delivered_len);return delivered_len;}
size_t copy_tx(uint8_t*p){memcpy(p,tx_data,tx_len);return tx_len;}
void enqueue_tx(const uint8_t*p,size_t n){
    assert((s_txq_head+1)%TX_QUEUE_LEN!=s_txq_tail);
    s_txq[s_txq_head]=malloc(n);memcpy(s_txq[s_txq_head],p,n);
    s_txq_len[s_txq_head]=n;s_txq_session[s_txq_head]=10;s_txq_head=(s_txq_head+1)%TX_QUEUE_LEN;
}
int control_try(int done){s_ctrl_inflight=true;ctrl_completion=done;ctrl_held=0;bool ok=ctrl_acquire();if(ok)xSemaphoreGive(s_ctrl_lock);assert(ctrl_held==0);return ok;}
void lsp(uint8_t*p){size_t len;build_lsp(p,&len);assert(len==19);}
void initial_power(void){send_power_source_update();}
int power_request(int connected,int identified){s_connected=connected;s_identified=identified;return iap2_host_request_charging();}
'''


def packet(seq, payload=b'', control=64, ack=99, session=10):
    n = len(payload) + 10 if payload else 9
    head = struct.pack('>HHBBBB', 0xff5a, n, control, seq, ack, session)
    return head + bytes([-sum(head) & 255]) + payload + (bytes([-sum(payload) & 255]) if payload else b'')


class IAP2LinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc')
        if not cc:
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        (work/'esp_err.h').write_text('typedef int esp_err_t;\n')
        source = (ROOT/'firmware/main/iap2_host.c').read_text()
        names = ['ctrl_acquire', 'checksum', 'input_report_total', 'rx_store', 'in_done_cb', 'reset_link',
                 'send_ack', 'build_lsp', 'parse_lsp', 'csm_feed', 'session_enqueue',
                 'session_deliver', 'handle_link_packet', 'process_rx', 'tx_pump',
                 'pb_raw', 'pb_u16be', 'pb_param', 'pb_u8', 'pb_u16',
                 'csm_send', 'send_power_source_update']
        a = source.index('static const uint16_t s_in_report_sizes')
        table = source[a:source.index(';', a)+1]
        current = next(line for line in source.splitlines()
                       if line.startswith('#define IAP2_AVAILABLE_CURRENT_MA '))
        charging = function(source.replace('\nesp_err_t iap2_host_request_charging(',
                            '\nstatic esp_err_t iap2_host_request_charging('),
                            'iap2_host_request_charging').removeprefix('static ')
        harness = (PRELUDE + current + '\n' + table + '\n' +
                   '\n'.join(function(source,n) for n in names) + charging + DRIVER)
        cls.harness=harness
        (work/'link.c').write_text(harness)
        subprocess.run([cc, '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I'+str(work), '-I'+str(ROOT/'firmware/main'),
                        str(work/'link.c'), '-o', str(work/'link.so')], check=True)
        cls.lib = ctypes.CDLL(str(work/'link.so'))
        cls.lib.value.argtypes = [ctypes.c_int]; cls.lib.value.restype = ctypes.c_uint64
        for name in ('feed','usb_feed','enqueue_tx','parse_peer'):
            getattr(cls.lib,name).argtypes = [ctypes.c_void_p,ctypes.c_size_t]
        cls.lib.settings.argtypes = [ctypes.c_int,ctypes.c_int]
        cls.lib.tick.argtypes = [ctypes.c_uint32]
        cls.lib.power_request.argtypes = [ctypes.c_int, ctypes.c_int]
        for name in ('copy_data','copy_tx'):
            getattr(cls.lib,name).argtypes = [ctypes.c_void_p]
            getattr(cls.lib,name).restype = ctypes.c_size_t

    def setUp(self): self.lib.setup()
    def v(self,n): return self.lib.value(n)
    def feed(self,b): self.lib.feed(b,len(b))
    def data(self,name='copy_data'):
        out=ctypes.create_string_buffer(65536);n=getattr(self.lib,name)(out);return out.raw[:n]

    def test_reset_during_receive_does_not_underflow(self):
        self.feed(packet(0,control=16)+packet(1,b'old'))
        self.assertEqual((self.v(5),self.v(6),self.v(13),self.v(0)),(0,1,1,0))
        self.lib.pump();self.assertEqual(self.v(5),0)

    def test_ack_after_safe_enqueue_before_application_processing(self):
        self.feed(packet(1,b'payload'))
        self.assertEqual((self.v(0),self.v(1),self.v(2),self.v(3)),(1,0,1,1))
        self.lib.drain();self.assertEqual(self.data(),b'payload')

    def test_large_hid_report_final_two_bytes_are_preserved(self):
        for report_id,total in [(9,258),(10,386),(11,514),(12,768)]:
            self.lib.setup()
            body=bytes((i*37+19)&255 for i in range(total-12))
            report=bytes([report_id,0])+packet(1,body)
            self.assertEqual(len(report),total)
            for pos in range(0,total,64):
                chunk=report[pos:pos+64];self.lib.usb_feed(chunk,len(chunk))
            self.lib.drain()
            self.assertEqual(self.data(),body)
            self.assertEqual((self.v(14),self.v(21),self.v(1)),(0,0,1))

    def test_overflow_discards_whole_fragment_and_recovers_link(self):
        self.lib.settings(1,1)
        report=bytes([4,0])+packet(1,b'abc')+bytes(3)
        self.lib.usb_feed(report,len(report))
        self.assertEqual((self.v(9),self.v(13),self.v(5),self.v(0)),(1,1,0,0))

    def test_truncated_hid_report_resets_framing(self):
        report=bytes([9,0])+packet(1,bytes(246))
        self.lib.usb_feed(report[:64],64)
        self.lib.usb_feed(b'',0)
        self.assertEqual((self.v(14),self.v(21),self.v(13)),(1,0,1))

    def test_truncated_valid_header_expires_and_resynchronizes(self):
        truncated=packet(1,bytes(100))[:20]
        self.feed(truncated+packet(1,b'recovered'))
        self.assertEqual(self.v(0),0)
        self.lib.tick(8000);self.lib.pump();self.lib.drain()
        self.assertEqual(self.data(),b'recovered')

    def test_corrupt_header_length_does_not_swallow_next_packet(self):
        bad=bytearray(packet(1,b'bad'));bad[2:4]=b'\xff\xff'
        self.feed(bytes(bad)+packet(1,b'good'))
        self.lib.drain();self.assertEqual(self.data(),b'good');self.assertEqual(self.v(7),1)

    def test_bad_payload_and_garbage_resynchronize(self):
        bad=bytearray(packet(1,b'bad'));bad[-1]^=1
        self.feed(bytes(10000)+bytes(bad)+packet(1,b'good'))
        for _ in range(5): self.lib.pump()
        self.lib.drain();self.assertEqual(self.data(),b'good');self.assertEqual(self.v(8),1)

    def test_fragmented_link_frame(self):
        frame=packet(1,b'fragmented')
        for b in frame: self.feed(bytes([b]))
        self.lib.drain();self.assertEqual(self.data(),b'fragmented')

    def test_duplicate_gap_and_sequence_wrap(self):
        self.lib.settings(2,254)
        self.feed(packet(255,b'A'));self.feed(packet(255,b'A'))
        self.feed(packet(1,b'C'));self.assertEqual(self.v(3),255)
        self.feed(packet(0,b'B'));self.feed(packet(1,b'C'))
        self.lib.drain()
        self.assertEqual((self.v(1),self.v(10),self.v(11),self.v(4)),(3,1,1,1))

    def test_pure_ack_does_not_consume_receive_sequence(self):
        self.feed(packet(1));self.feed(packet(1,b'first'))
        self.lib.drain();self.assertEqual(self.data(),b'first');self.assertEqual(self.v(1),1)

    def test_full_queue_and_allocation_failure_are_not_acknowledged(self):
        self.lib.settings(0,1)
        self.feed(packet(1,b'one'));self.feed(packet(2,b'two'))
        self.assertEqual((self.v(2),self.v(4)),(1,1))
        self.lib.drain();self.lib.settings(6,1);self.feed(packet(2,b'two'))
        self.assertEqual((self.v(2),self.v(4)),(1,1))
        self.lib.settings(6,0);self.feed(packet(2,b'two'));self.lib.drain()
        self.assertEqual(self.data(),b'two');self.assertEqual(self.v(12),2)

    def test_old_session_work_is_discarded_after_reset(self):
        self.feed(packet(1,b'old'));self.lib.hold();self.lib.reset();self.lib.deliver_held()
        self.assertEqual(self.v(1),0)

    def test_tx_respects_peer_packet_limit(self):
        payload=bytes(range(100));self.lib.enqueue_tx(payload,len(payload));self.lib.settings(3,30)
        for i in range(5):
            self.lib.tx();self.assertEqual(self.v(18),100+i)
            self.feed(packet(0,ack=100+i))
        self.assertEqual(self.data('copy_tx'),payload);self.assertEqual(self.v(17),0)

    def test_failed_tx_retries_same_sequence_and_payload(self):
        self.lib.enqueue_tx(b'keep',4);self.lib.settings(4,1);self.lib.tx()
        self.assertEqual((self.v(17),self.v(18)),(1,100))
        self.lib.tick(4000);self.lib.settings(4,0);self.lib.tx()
        self.assertEqual((self.v(18),self.v(20)),(100,1))
        self.assertEqual(self.data('copy_tx'),b'keepkeep')
        self.feed(packet(0,ack=100));self.assertEqual(self.v(17),0)

    def test_eak_is_link_control_and_retransmits_outstanding_data(self):
        self.lib.enqueue_tx(b'keep',4);self.lib.tx()
        self.feed(packet(77,b'\x65',control=96,ack=99));self.lib.tx()
        self.assertEqual((self.v(0),self.v(4),self.v(18),self.v(20)),(0,0,100,1))

    def test_parser_packet_and_time_budgets_yield(self):
        self.feed(b''.join(packet(i,b'x') for i in range(1,21)))
        self.assertEqual(self.v(22),16)
        self.lib.drain();self.lib.pump();self.assertEqual(self.v(22),20)
        self.lib.settings(5,4000)
        self.feed(packet(21,b'y'));self.assertEqual(self.v(22),20)
        self.lib.settings(5,1);self.lib.pump();self.assertEqual(self.v(22),21)

    def test_negotiation_advertises_bounded_window_and_packet_size(self):
        out=ctypes.create_string_buffer(19);self.lib.lsp(out)
        self.assertEqual(out.raw[1],1);self.assertEqual(int.from_bytes(out.raw[2:4],'big'),4096)
        self.assertEqual(out.raw[9],1)

    def test_stale_usb_completion_cannot_reset_replacement_device(self):
        self.lib.stale_usb();self.assertEqual(self.v(13),0)

    def test_invalid_negotiation_does_not_replace_current_limits(self):
        self.lib.settings(3,200)
        invalid=bytes([1,1,0,9,0,0,1,244,4,1])
        self.assertFalse(self.lib.parse_peer(invalid,len(invalid)))
        self.assertEqual(self.v(24),200)

    def test_timed_out_control_buffer_waits_for_transfer_completion(self):
        self.assertEqual(self.lib.control_try(0),0)
        self.assertEqual(self.lib.control_try(1),1)

    def test_initial_and_manual_power_updates_advertise_500ma_and_allow_charging(self):
        # Decode the wire frame, including both mandatory initial parameters.
        for manual in (False, True):
            with self.subTest(manual=manual):
                self.lib.setup()
                if manual:
                    self.assertEqual(self.lib.power_request(1,1),0)
                else:
                    self.lib.initial_power()
                frame = self.data('copy_tx')
                self.assertEqual(struct.unpack('>HHH',frame[:6]),(0x4040,len(frame),0xae03))
                self.assertEqual(struct.unpack('>HHH',frame[6:12]),(6,0,500))
                self.assertEqual(struct.unpack('>HHB',frame[12:]),(5,1,1))

    def test_manual_power_update_requires_a_connected_identified_device(self):
        for connected,identified in ((0,0),(0,1),(1,0)):
            with self.subTest(connected=connected,identified=identified):
                self.assertEqual(self.lib.power_request(connected,identified),0x103)
                self.assertEqual(self.data('copy_tx'),b'')

    def test_control_message_reassembles_across_link_packets(self):
        csm=struct.pack('>HHH',0x4040,9006,0x4c04)+bytes(9000)
        for i,pos in enumerate(range(0,len(csm),4000)):
            self.feed(packet(i+1,csm[pos:pos+4000]));self.lib.drain()
        self.assertEqual((self.v(15),self.v(23)),(1,0))

    def test_corrupt_stream_under_address_and_undefined_sanitizers(self):
        work=Path(self.tmp.name)
        main=r"""
int main(void){
    uint32_t seed=7;uint8_t bytes[64];setup();
    for(unsigned i=0;i<10000;i++){
        for(unsigned j=0;j<sizeof(bytes);j++){seed=seed*1664525u+1013904223u;bytes[j]=seed>>24;}
        if(i%100==0){reset_link();s_state=LINK_NORMAL;}
        feed(bytes,sizeof(bytes));drain();assert(s_link_len<=LINK_BUF_SIZE);
    }
    setup();uint8_t rst[9]={255,90,0,9,16,0,0,0,0};rst[8]=checksum(rst,8);
    feed(rst,9);assert(s_link_len==0);return 0;
}
"""
        (work/'asan.c').write_text(self.harness+main)
        exe=work/'asan'
        subprocess.run([shutil.which('cc'),'-std=c11','-O1','-g','-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all','-I'+str(work),'-I'+str(ROOT/'firmware/main'),
                        str(work/'asan.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=15)

    def test_15000_library_messages_with_wraps_and_duplicates(self):
        for i in range(15000):
            csm=struct.pack('>HHHI',0x4040,10,0x4c04,i)
            frame=packet((i+1)&255,csm)
            self.feed(frame)
            if i%17==0:self.feed(frame)
            self.lib.drain()
        self.assertEqual((self.v(1),self.v(15),self.v(16)),(15000,15000,14999*15000//2))
        self.assertEqual((self.v(7),self.v(8),self.v(9),self.v(10),self.v(13)),(0,0,0,0,0))

if __name__=='__main__':unittest.main()
