"""Optional Chromium tests: pip install playwright && playwright install chromium.

All device APIs are mocked; these tests never contact or write to hardware.
"""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
import unittest

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sync_playwright = None

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sync_playwright, 'Optional Playwright dependency unavailable')
class WebUITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        class QuietHandler(SimpleHTTPRequestHandler):
            def log_message(self, *args):
                pass
        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), partial(QuietHandler, directory=str(ROOT / 'assets/web')))
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.addClassCleanup(cls.server.server_close)
        cls.addClassCleanup(cls.server.shutdown)
        cls.pw = sync_playwright().start()
        cls.addClassCleanup(cls.pw.stop)
        cls.browser = cls.pw.chromium.launch()
        cls.addClassCleanup(cls.browser.close)
        cls.base = f'http://127.0.0.1:{cls.server.server_port}'

    def setUp(self):
        self.page = self.browser.new_page(viewport={'width': 1440, 'height': 1000})
        self.addCleanup(self.page.close)
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.writes = []
        self.status = dict(firmware='test', title='<script>track</script>', artist='Artist 日本語',
                           album='Album', play_status=2, iap2_mode=True, iap2_identified=True,
                           sd_ready=True, screen_on=True, usb_mode=2, ip='192.0.2.1', uptime_s=100,
                           device_battery=-1, device_charging=False, iap2_link={'rx_packets': 200})
        self.config = dict(volume=65, brightness=70, screen_off_minutes=0, theme=0,
                           sound_mode=2, debug_log=False, iap2_mode=True, usb_mode=2,
                           screen_flip=0, wifi_mode=0, ap_ssid='Tab5', charge_enabled=True, qc_high=False)
        self.power = dict(ina226_ok=True, bus_mv=7900, current_ma=-550, chg_en=True)
        self.page.route('**/api/**', self.api)
        self.page.route('**/debug', lambda route: route.fulfill(path=str(ROOT / 'assets/web/debug.html'), content_type='text/html'))
        self.page.add_init_script('''window.EventSource=class {
          constructor(url) { this.url=url; window.stream=this; }
          close() { this.closed=true; }
        };''')

    def api(self, route):
        request = route.request
        path = request.url.split('/api/', 1)[1]
        if request.method == 'POST':
            self.writes.append((path, request.post_data))
            if path == 'display/mark':
                self.status['display'].update(marker=1, marker_uptime_ms=100000)
                route.fulfill(json=self.status['display'])
                return
            if path == 'config':
                self.config.update(json.loads(request.post_data))
            route.fulfill(body='OK')
        elif path == 'status':
            route.fulfill(json=self.status)
        elif path == 'config':
            route.fulfill(json=self.config)
        elif path == 'power':
            route.fulfill(json=self.power)
        elif path == 'display/trace':
            route.fulfill(body='seq,uptime_ms,pwm_duty,pwm_hz\n1,100000,409,5000\n',
                          content_type='text/csv', headers={'Content-Disposition':'attachment; filename=tab5-display.csv'})
        else:
            route.fulfill(body='log line', headers={'X-Log-Seq': '1'})

    def open(self, path='/'):
        self.page.goto(self.base + path)
        self.page.wait_for_function('document.getElementById("chips").textContent.includes("test")' if path == '/debug'
                                    else 'document.getElementById("np_artist").textContent.includes("Artist")')

    def test_dashboard_metadata_and_battery_current(self):
        self.open()
        self.assertEqual(self.page.locator('#np_title').inner_text(), '<script>track</script>')
        self.assertIn('日本語', self.page.locator('#np_artist').inner_text())
        self.assertIn('550 mA into battery', self.page.locator('#power_rows').inner_text())
        self.assertEqual(self.page.locator('#charge_state').text_content(), 'Charging')
        self.assertIn('Unavailable', self.page.locator('#device_rows').inner_text())
        for current, state in [(0,'Idle'), (300,'Discharging')]:
            self.power['current_ma'] = current
            self.page.evaluate('refresh()')
            self.page.wait_for_function('(s)=>document.getElementById("charge_state").textContent===s', arg=state)
        self.power['ina226_ok'] = False
        self.page.evaluate('refresh()')
        self.page.wait_for_function('document.getElementById("charge_state").textContent==="Unavailable"')
        self.assertFalse(self.errors)

    def test_unsaved_settings_survive_polling_and_screen_off_saves(self):
        self.open()
        self.page.locator('#screen_off_minutes').select_option('60')
        self.page.locator('#debug_log').check()
        self.page.locator('#ap_ssid').fill('Draft SSID')
        self.page.locator('#brightness').focus()
        self.page.evaluate('refresh()')
        self.assertEqual(self.page.locator('#screen_off_minutes').input_value(), '60')
        self.assertTrue(self.page.locator('#debug_log').is_checked())
        self.assertEqual(self.page.locator('#ap_ssid').input_value(), 'Draft SSID')
        self.page.get_by_role('button', name='Save', exact=True).click()
        self.page.wait_for_function('!dirty.screen_off_minutes')
        self.assertEqual(self.config['screen_off_minutes'], 60)
        self.assertEqual(json.loads(self.writes[-1][1]), {'screen_off_minutes': 60, 'debug_log': True, 'ap_ssid': 'Draft SSID'})

    def test_named_controls_and_mobile_layout(self):
        self.open()
        self.page.locator('#btn_shuffle').click()
        self.page.get_by_role('button', name='VOL +', exact=True).click()
        self.page.get_by_role('button', name='Turn screen off', exact=True).click()
        self.page.get_by_role('button', name='Wake screen', exact=True).click()
        self.assertEqual([p for p,b in self.writes], ['key?k=shuffle','key?k=volup','key?k=screenoff','key?k=wake'])
        self.page.set_viewport_size({'width': 390, 'height': 844})
        self.assertTrue(self.page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.assertFalse(self.errors)

    def test_display_recorder_and_marker_download(self):
        self.status['display'] = dict(available=True, pwm_duty=409, pwm_hz=5000,
            pwm_duty_changes=0, pwm_freq_changes=0, vsync_gap_us=17461,
            vsync_max_gap_us=17470, vsync_gaps_over_25ms=0, render_last_us=100,
            render_max_us=1200, dma_wait_timeouts=0, draw_errors=0,
            rotation_errors=0, internal_free=80000, internal_largest=32000, marker=0)
        self.open('/debug')
        self.assertIn('409 / 1023 · 5000 Hz', self.page.locator('#display_rows').inner_text())
        with self.page.expect_download() as download:
            self.page.locator('#mark_flicker').click()
        self.assertEqual(download.value.suggested_filename, 'tab5-display.csv')
        self.assertEqual(self.writes[0][0], 'display/mark')
        self.page.wait_for_function('document.getElementById("display_rows").textContent.includes("#1")')
        self.assertFalse(self.errors)

    def test_debug_log_resume_filter_and_gestures(self):
        self.open('/debug')
        self.assertIn('Charging', self.page.locator('#battery_metrics').inner_text())
        self.page.evaluate('stream.onmessage({lastEventId:"5",data:"usb ready"}); stream.onmessage({lastEventId:"5",data:"duplicate"}); stream.onmessage({lastEventId:"6",data:"audio ready"})')
        self.page.wait_for_function('document.getElementById("live_log").textContent.includes("audio")')
        self.assertNotIn('duplicate', self.page.locator('#live_log').inner_text())
        self.page.locator('#log_filter').fill('USB')
        self.assertEqual(self.page.locator('#live_log').inner_text(), 'usb ready')
        self.page.locator('#log_pause').click()
        self.page.locator('#log_pause').click()
        self.assertTrue(self.page.evaluate('stream.url.endsWith("since=6")'))
        self.page.get_by_text('Keypad gestures', exact=True).click()
        self.page.locator('#knob_hold').click()
        self.page.wait_for_function('held.knob')
        self.page.get_by_role('button', name='Held turn +', exact=True).click()
        self.page.get_by_role('button', name='Cancel holds', exact=True).click()
        self.page.wait_for_function('!held.knob')
        self.assertEqual([p for p,b in self.writes], ['keystate?k=knob&down=1','keystate?k=knobcw&down=1','keystate?k=cancel&down=0'])
        self.page.set_viewport_size({'width': 390, 'height': 844})
        self.assertTrue(self.page.evaluate('document.documentElement.scrollWidth <= innerWidth'))
        self.assertFalse(self.errors)


if __name__ == '__main__':
    unittest.main()
