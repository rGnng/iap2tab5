"""Decode actual progressive covers through the firmware software fallback."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
try:
    from PIL import Image
except ImportError:
    Image = None

ROOT=Path(__file__).resolve().parents[1]

class JpegThumbnailTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if Image is None:
            raise unittest.SkipTest('Pillow unavailable')
        source=ROOT/'firmware/managed_components/espressif__libjpeg-turbo/libjpeg-turbo'
        if not (source/'CMakeLists.txt').exists():
            raise unittest.SkipTest('Configure firmware dependencies first')
        cls.work=tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.work.cleanup)
        path=Path(cls.work.name)
        build=path/'jpeg'
        subprocess.run(['cmake','-S',str(source),'-B',str(build),
                        '-DENABLE_SHARED=OFF','-DWITH_SIMD=OFF','-DWITH_TURBOJPEG=OFF',
                        '-DWITH_TOOLS=OFF','-DWITH_TESTS=OFF','-DCMAKE_POSITION_INDEPENDENT_CODE=ON'],
                       check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        subprocess.run(['cmake','--build',str(build),'--target','jpeg-static','-j6'],
                       check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        subprocess.run(['cc','-shared','-fPIC','-DJPEG_TEST_MEMORY_LIMIT=65536',
                        '-I'+str(build),'-I'+str(source/'src'),
                        str(ROOT/'firmware/main/jpeg_thumbnail.c'),
                        str(ROOT/'firmware/main/jpeg_backing_store.c'),
                        '-Wl,--wrap=jpeg_open_backing_store',str(build/'libjpeg.a'),
                        '-o',str(path/'jpeg.so')],check=True)
        cls.lib=C.CDLL(str(path/'jpeg.so'))
        cls.lib.jpeg_thumbnail_decode.argtypes=[C.c_char_p,C.c_void_p,C.c_uint]
        cls.lib.jpeg_thumbnail_decode.restype=C.c_bool

    def decode(self,path,size=54):
        out=(C.c_uint8*(size*size*2))()
        ok=self.lib.jpeg_thumbnail_decode(str(path).encode(),out,size)
        self.assertFalse(list(Path('/tmp').glob(f'.jpeg-{os.getpid():08x}-*.tmp')),
                         'Decoder must remove coefficient spill files')
        return ok,bytes(out)

    def fixture(self,mode='RGB',size=(513,377),progressive=True):
        path=Path(self.work.name)/'fixture.jpg'
        image=Image.new(mode,size,128 if mode=='L' else (230,45,90))
        image.save(path,quality=95,progressive=progressive)
        return path

    def test_progressive_color_odd_dimensions_and_rgb565_order(self):
        ok,out=self.decode(self.fixture())
        self.assertTrue(ok)
        pixel=int.from_bytes(out[:2],'little')
        self.assertGreater((pixel>>11)*8,210)
        self.assertLess(((pixel>>5)&63)*4,60)
        self.assertTrue(all(out[i:i+2]==out[:2] for i in range(0,len(out),2)))

    def test_progressive_grayscale_and_upsampling(self):
        for dimensions in ((900,700),(17,19)):
            with self.subTest(dimensions=dimensions):
                ok,out=self.decode(self.fixture('L',dimensions))
                self.assertTrue(ok)
                pixel=int.from_bytes(out[:2],'little')
                self.assertLess(abs((pixel>>11)*8-128),9)

    def test_baseline_and_large_now_playing_target(self):
        ok,out=self.decode(self.fixture(progressive=False),360)
        self.assertTrue(ok)
        self.assertEqual(len(out),360*360*2)

    def test_progressive_cmyk_with_coefficient_spill(self):
        path=self.fixture()
        image=Image.new('RGB',(513,377),(230,45,90)).convert('CMYK')
        image.save(path,quality=95,progressive=True)
        ok,out=self.decode(path)
        self.assertTrue(ok)
        pixel=int.from_bytes(out[:2],'little')
        self.assertGreater((pixel>>11)*8,210)
        self.assertLess(((pixel>>5)&63)*4,60)

    def test_corrupt_and_truncated_jpegs_are_rejected(self):
        path=self.fixture()
        original=path.read_bytes()
        for data in (b'not a jpeg',original[:len(original)//2],original[:-2]):
            with self.subTest(length=len(data)):
                path.write_bytes(data)
                self.assertFalse(self.decode(path)[0])
