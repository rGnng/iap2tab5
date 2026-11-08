"""Compile the production web key parser; named commands must stay distinct."""
import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WebInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = (ROOT / 'firmware/main/web_server.c').read_text()
        parser = source[source.index('static bool web_key_parse'):source.index('static esp_err_t key_post')]
        header = (ROOT / 'app/ui/ui.h').read_text()
        enum = re.search(r'typedef enum \{.*?\} ui_key_t;', header, re.S).group()
        work = Path(cls.tmp.name)
        (work / 'parser.c').write_text('#include <stdbool.h>\n#include <stddef.h>\n#include <string.h>\n' + enum + parser + '''
int parse(const char *name) { ui_key_t key; return web_key_parse(name,&key) ? (int)key : -1; }
''')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared',
                        '-fPIC', str(work / 'parser.c'), '-o', str(work / 'parser.so')], check=True)
        cls.lib = ctypes.CDLL(str(work / 'parser.so'))
        cls.lib.parse.argtypes = [ctypes.c_char_p]

    def parse(self, value):
        return self.lib.parse(value.encode())

    def test_shuffle_does_not_move_down(self):
        self.assertGreaterEqual(self.parse('shuffle'), 0)
        self.assertNotEqual(self.parse('shuffle'), self.parse('s'))

    def test_named_controls_and_legacy_aliases(self):
        for name, alias in [('up','w'), ('down','s'), ('enter','\r'), ('back','b'),
                            ('queue','p'), ('tabprev','['), ('tabnext',']'),
                            ('nowplaying','1'), ('albums','2'), ('artists','3'),
                            ('songs','4'), ('playlists','5'), ('play','7')]:
            self.assertEqual(self.parse(name), self.parse(alias))
            self.assertGreaterEqual(self.parse(name), 0)

    def test_media_commands_are_distinct(self):
        values = [self.parse(k) for k in ['prev','next','play','star','shuffle','repeat','volup','voldown','mute']]
        self.assertEqual(len(set(values)), len(values))
        self.assertNotIn(-1, values)

    def test_unknown_names_never_fall_back_to_first_character(self):
        for name in ['', 'something', 'shuffle-extra', 'repeat-extra', 'wrong', 'playnext']:
            self.assertEqual(self.parse(name), -1)


if __name__ == '__main__':
    unittest.main()
