"""Check production palettes: safe IDs and contrast across shared surfaces."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_album_view import function

ROOT = Path(__file__).resolve().parents[1]
COLORS = """menu_top_bg menu_top_fg menu_body_bg menu_body_fg
mini_bg mini_fg mini_muted mini_dim bg surface panel panel_alt border text muted dim
accent selection focus on_selection outline counter fast_scroll_band green amber red purple cyan
context_panel context_group context_border progress scrim
button_top button_bottom bevel_light bevel_shadow""".split()


class Theme(ctypes.Structure):
    _fields_ = [('id', ctypes.c_int)] + [
        (name, ctypes.c_uint32) for name in COLORS
    ] + [('scrim_opa', ctypes.c_uint8)]


def luminance(rgb):
    channels = [(rgb >> shift & 255) / 255 for shift in (16, 8, 0)]
    linear = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4
              for v in channels]
    return sum(v * w for v, w in zip(linear, (.2126, .7152, .0722)))


def contrast(a, b):
    light, dark = sorted((luminance(a), luminance(b)), reverse=True)
    return (light + .05) / (dark + .05)


class ThemeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc')
        if not cc:
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / 'theme.so'
        harness = Path(cls.tmp.name) / 'selection.c'
        harness.write_text(r'''
#include <stdbool.h>
#include "theme.h"
#define LV_STATE_USER_1 256
typedef struct obj {
    bool label;
    unsigned state, count;
    struct obj *children[3];
} lv_obj_t;
static const ui_theme_t *g_theme;
static int lv_label_class;
static bool lv_obj_check_type(lv_obj_t *o, const void *c) { (void)c; return o->label; }
static void lv_obj_add_state(lv_obj_t *o, unsigned s) { o->state |= s; }
static void lv_obj_remove_state(lv_obj_t *o, unsigned s) { o->state &= ~s; }
static unsigned lv_obj_get_child_count(lv_obj_t *o) { return o->count; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *o, unsigned i) { return o->children[i]; }
''' + function((ROOT / 'app/ui/ui.c').read_text(), 'theme_mark_selected') + r'''
static lv_obj_t nodes[5];
void reset_labels(void) {
    nodes[0] = (lv_obj_t){.state=32, .count=3,
        .children={&nodes[1], &nodes[2], &nodes[3]}};
    nodes[1] = (lv_obj_t){.state=32, .label=true};
    nodes[2] = (lv_obj_t){.state=32}; /* Artwork is never recolored. */
    nodes[3] = (lv_obj_t){.state=32, .count=1, .children={&nodes[4]}};
    nodes[4] = (lv_obj_t){.state=32, .label=true};
}
void select_labels(int theme, bool selected) {
    g_theme = ui_theme_get(theme);
    theme_mark_selected(&nodes[0], selected);
}
unsigned label_state(int index) { return nodes[index].state; }
''')
        subprocess.run([cc, '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'app/ui'), str(harness),
                        str(ROOT / 'app/ui/theme.c'), '-o', str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.lib.ui_theme_get.argtypes = [ctypes.c_int]
        cls.lib.ui_theme_get.restype = ctypes.POINTER(Theme)
        cls.lib.select_labels.argtypes = [ctypes.c_int, ctypes.c_bool]
        cls.lib.label_state.argtypes = [ctypes.c_int]
        cls.lib.label_state.restype = ctypes.c_uint

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_invalid_saved_id_falls_back_to_gray(self):
        for value in (-2147483648, -1, 3, 255, 2147483647):
            self.assertEqual(self.lib.ui_theme_get(value).contents.id, 0)

    def test_distinct_valid_palettes(self):
        themes = [self.lib.ui_theme_get(i).contents for i in range(3)]
        self.assertEqual([t.id for t in themes], [0, 1, 2])
        self.assertEqual(len({t.focus for t in themes}), 3)
        for t in themes:
            for name in COLORS:
                self.assertLessEqual(getattr(t, name), 0xFFFFFF)

    def test_recycled_rows_restore_label_colors_and_preserve_other_states(self):
        for theme in (1, 2):
            self.lib.reset_labels()
            self.lib.select_labels(theme, True)
            self.assertEqual([self.lib.label_state(i) for i in range(5)],
                             [32, 288, 32, 32, 288])
            self.lib.select_labels(theme, False)
            self.assertEqual([self.lib.label_state(i) for i in range(5)], [32] * 5)

    def test_gray_preserves_existing_label_colors(self):
        self.lib.reset_labels()
        self.lib.select_labels(0, True)
        self.assertEqual([self.lib.label_state(i) for i in range(5)], [32] * 5)

    def test_primary_and_metadata_text_contrast(self):
        pairs = [('text', bg) for bg in ('bg', 'panel', 'panel_alt',
                                        'context_panel', 'button_top', 'button_bottom')]
        pairs += [('muted', bg) for bg in ('panel', 'panel_alt', 'counter')]
        pairs += [('menu_top_fg', 'menu_top_bg'), ('menu_body_fg', 'menu_body_bg'),
                  ('mini_fg', 'mini_bg'), ('mini_muted', 'mini_bg'),
                  ('on_selection', 'focus'), ('on_selection', 'selection')]
        for i in range(3):
            t = self.lib.ui_theme_get(i).contents
            for fg, bg in pairs:
                with self.subTest(theme=i, foreground=fg, background=bg):
                    self.assertGreaterEqual(contrast(getattr(t, fg), getattr(t, bg)), 4.5)

    def test_fast_scroll_band_is_distinct_and_contrasts_with_the_cursor(self):
        for i in range(3):
            t=self.lib.ui_theme_get(i).contents
            with self.subTest(theme=i):
                self.assertNotEqual(t.fast_scroll_band,t.accent)
                self.assertGreaterEqual(contrast(t.fast_scroll_band,t.focus),3)

    def test_light_theme_scrims_keep_dark_text_legible(self):
        # Worst case: black cover behind a light scrim.
        for i in (1, 2):
            t = self.lib.ui_theme_get(i).contents
            rgb = sum(round((t.scrim >> shift & 255) * t.scrim_opa / 255) << shift
                      for shift in (16, 8, 0))
            self.assertGreaterEqual(contrast(t.text, rgb), 4.5)
            self.assertGreaterEqual(contrast(t.muted, rgb), 4.5)


if __name__ == '__main__':
    unittest.main()
