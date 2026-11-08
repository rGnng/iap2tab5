"""Color/quality oracles and immutable buffer handoff for the real generator."""
import ctypes as C
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SAMPLES = 32 * 32


class Sample(C.Structure):
    _fields_ = [(c, C.c_uint8) for c in 'rgba']


class Color(C.Structure):
    _fields_ = [(c, C.c_uint8) for c in 'rgb']


def unpack(pixel):
    return ((pixel >> 11 & 31) * 255 / 31,
            (pixel >> 5 & 63) * 255 / 63, (pixel & 31) * 255 / 31)


def reference(palette, x, y, width, height):
    tx, ty = x / max(width - 1, 1), y / max(height - 1, 1)
    wx, wy = ((1-tx)**2, 2*tx*(1-tx), tx**2), ((1-ty)**2, 2*ty*(1-ty), ty**2)
    grid = ((0, 3, 1), (2, 4, 3), (1, 2, 0))
    return [sum(wy[i]*wx[j]*palette[grid[i][j]][c] for i in range(3) for j in range(3))
            for c in range(3)]


class CoverGradientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        so = Path(cls.tmp.name) / 'gradient.so'
        subprocess.run(['cc', '-std=c11', '-O2', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(ROOT/'app/backend/cover_gradient.c'),
                        str(ROOT/'app/backend/cover_gradient_math.c'), '-o', str(so)],
                       check=True, capture_output=True)
        cls.lib = C.CDLL(str(so))
        cls.lib.cover_gradient_sample.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.c_uint,
                                                  C.c_size_t, C.c_int, C.POINTER(Sample)]
        cls.lib.cover_gradient_sample.restype = C.c_bool
        cls.lib.cover_gradient_palette.argtypes = [C.POINTER(Sample), C.POINTER(Color)]
        cls.lib.cover_gradient_palette.restype = C.c_bool
        cls.lib.cover_gradient_render.argtypes = [C.POINTER(Color), C.c_void_p, C.c_uint, C.c_uint, C.c_size_t]
        cls.lib.cover_gradient_scale_cpu.argtypes = [C.c_void_p, C.c_uint, C.c_uint, C.c_size_t,
                                                    C.c_void_p, C.c_uint, C.c_uint]
        cls.lib.cover_gradient_request.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.c_uint,
                                                  C.c_size_t, C.c_int, C.c_uint32, C.c_uint8]
        cls.lib.cover_gradient_request.restype = C.c_uint64
        cls.lib.cover_gradient_poll.argtypes = [C.c_uint64, C.POINTER(C.POINTER(C.c_uint16))]
        cls.lib.cover_gradient_finish.argtypes = [C.POINTER(C.c_uint16), C.c_uint, C.c_uint,
                                                 C.c_uint32, C.c_uint8]

    def setUp(self):
        self.lib.cover_gradient_cancel()

    def palette(self, values):
        samples = (Sample*SAMPLES)(*(Sample(*value) for value in values))
        palette = (Color*5)()
        ok = self.lib.cover_gradient_palette(samples, palette)
        return ok, [(c.r, c.g, c.b) for c in palette]

    def test_rgb565_stride_channel_order_and_cell_centers(self):
        values = [0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0x8410]
        raw = (C.c_uint8*18)(*([0xcc]*18))
        for y in range(2):
            for x in range(3):
                p = values[y*3+x]
                raw[y*9+x*2:y*9+x*2+2] = (p & 255, p >> 8)
        samples = (Sample*SAMPLES)()
        self.assertTrue(self.lib.cover_gradient_sample(raw, len(raw), 3, 2, 9, 0, samples))
        for y in range(32):
            for x in range(32):
                p = values[((2*y+1)*2//64)*3 + (2*x+1)*3//64]
                r, g, b = p >> 11, (p >> 5) & 63, p & 31
                s = samples[y*32+x]
                self.assertEqual((s.r, s.g, s.b, s.a),
                                 ((r<<3)|(r>>2), (g<<2)|(g>>4), (b<<3)|(b>>2), 255))

    def test_argb8888_and_invalid_sources_do_not_read_past_buffer(self):
        raw = (C.c_uint8*4)(10, 20, 30, 40)
        samples = (Sample*SAMPLES)()
        self.assertTrue(self.lib.cover_gradient_sample(raw, 4, 1, 1, 4, 1, samples))
        self.assertEqual((samples[0].r, samples[0].g, samples[0].b, samples[0].a), (30, 20, 10, 40))
        for ptr, size, w, h, stride, fmt in [(raw,3,1,1,4,1), (raw,4,0,1,4,1),
                                           (raw,4,1,0,4,1), (raw,4,2,1,2,0),
                                           (raw,4,1,2,4,1), (None,4,1,1,4,1),
                                           (raw,4,2**32-1,2**32-1,2**32-1,1),
                                           (raw,4,1,1,4,99)]:
            self.assertFalse(self.lib.cover_gradient_sample(ptr,size,w,h,stride,fmt,samples))

    def test_palette_handles_black_white_and_transparency(self):
        for color in [(0,0,0), (255,255,255), (2,3,4), (40,80,170)]:
            ok, palette = self.palette([(*color,255)]*SAMPLES)
            self.assertTrue(ok)
            self.assertEqual(palette, [color]*5)
        values = [(255,0,0,0)]*(SAMPLES-1) + [(0,0,255,255)]
        self.assertEqual(self.palette(values), (True, [(0,0,255)]*5))
        self.assertFalse(self.palette([(255,0,0,0)]*SAMPLES)[0])

    def test_palette_preserves_distinct_artwork_colors(self):
        colors = [(210,20,30), (30,190,40), (25,40,210), (210,180,30), (140,40,190)]
        ok, palette = self.palette([(*colors[i%5],255) for i in range(SAMPLES)])
        self.assertTrue(ok)
        self.assertEqual(set(palette), set(colors))
        self.assertEqual(palette, sorted(palette, key=lambda p: (77*p[0]+150*p[1]+29*p[2])>>8))

    def test_small_mesh_matches_original_bezier_and_upscale_quality(self):
        colors = [(10,20,30), (230,25,45), (40,190,60), (60,70,210), (220,190,110)]
        palette = (Color*5)(*(Color(*p) for p in colors))
        small = (C.c_uint8*(320*180*3))()
        self.lib.cover_gradient_render(palette, small, 320, 180, 320*3)
        rng = random.Random(9371)
        for x,y in [(0,0),(319,179),(0,179),(319,0),(160,90)] + [(rng.randrange(320),rng.randrange(180)) for _ in range(60)]:
            expected = reference(colors,x,y,320,180)
            actual = [small[(y*320+x)*3+c] for c in (2,1,0)]
            for a,b in zip(actual,expected): self.assertLessEqual(abs(a-b), 1)
        large = (C.c_uint16*(1280*720))()
        self.lib.cover_gradient_scale_cpu(small,320,180,960,large,1280,720)
        for x,y in [(0,0),(1279,719),(0,719),(1279,0)] + [(rng.randrange(1280),rng.randrange(720)) for _ in range(60)]:
            expected = reference(colors,x,y,1280,720)
            for c,(a,b) in enumerate(zip(unpack(large[y*1280+x]),expected)):
                self.assertLessEqual(abs(a-b), 5 if c==1 else 9)

    def test_result_cache_supersession_and_displayed_buffer_ownership(self):
        def request(pixel):
            image = (C.c_uint16*SAMPLES)(*([pixel]*SAMPLES))
            return self.lib.cover_gradient_request(image,C.sizeof(image),32,32,64,0,0,0)
        def poll(key):
            pixels = C.POINTER(C.c_uint16)()
            state = self.lib.cover_gradient_poll(key,C.byref(pixels))
            return state,pixels
        a = request(0xf800)
        state, front = poll(a)
        self.assertEqual(state, 1)
        self.assertEqual(front[0], 0xf800)
        original_address = C.cast(front,C.c_void_p).value
        self.assertEqual(request(0xf800), a)
        self.assertEqual(C.cast(poll(a)[1],C.c_void_p).value, original_address)
        b = request(0x07e0)
        self.assertNotEqual(a,b)
        self.assertEqual(front[0], 0xf800)  # Worker cannot overwrite a displayed image.
        c = request(0x001f)  # Replace an unconsumed result with newer artwork.
        self.assertEqual(poll(a)[0], 3)
        self.assertEqual(poll(b)[0], 3)
        state, current = poll(c)
        self.assertEqual(state, 1)
        self.assertEqual(current[0], 0x001f)
        self.assertNotEqual(C.cast(current,C.c_void_p).value, original_address)
        self.lib.cover_gradient_cancel()
        self.assertEqual(poll(c)[0], 3)
        self.assertEqual(request(0x001f), c)
        self.assertEqual(C.cast(poll(c)[1],C.c_void_p).value,C.cast(current,C.c_void_p).value)

    def test_benchmark_small_mesh_generation(self):
        palette = (Color*5)(*(Color(20+i*30,180-i*20,40+i*35) for i in range(5)))
        timings = []
        for w,h in [(1280,720),(320,180)]:
            pixels = (C.c_uint8*(w*h*3))()
            start = time.perf_counter()
            for _ in range(5): self.lib.cover_gradient_render(palette,pixels,w,h,w*3)
            timings.append((time.perf_counter()-start)/5*1000)
        print(f'\nHost Bezier mesh: full={timings[0]:.2f} ms, small={timings[1]:.2f} ms')

    def test_bayer_finish_is_static_and_keeps_tile_average_close_to_tint(self):
        for pixel in (0,0xffff,0x1234,0xad31):
            for tint,alpha in ((0,153),(0xf4f7fb,216),(0xc0c0c0,230)):
                a = (C.c_uint16*64)(*([pixel]*64))
                b = (C.c_uint16*64)(*([pixel]*64))
                self.lib.cover_gradient_finish(a,8,8,tint,alpha)
                self.lib.cover_gradient_finish(b,8,8,tint,alpha)
                self.assertEqual(list(a),list(b))
                for y in range(8):
                    for x in range(8):self.assertEqual(a[y*8+x],a[(y%4)*8+x%4])
                for shift,maximum,color_shift in ((11,31,16),(5,63,8),(0,31,0)):
                    actual = sum((p>>shift)&maximum for p in a)/64
                    original = (pixel>>shift)&maximum
                    target = (tint>>color_shift)&255
                    expected = original*(255-alpha)/255 + target*maximum*alpha/(255*255)
                    self.assertLessEqual(abs(actual-expected),0.251 if maximum==63 else 0.126)
        original = [0,0xffff,0x1234,0xad31]
        pixels = (C.c_uint16*4)(*original)
        self.lib.cover_gradient_finish(pixels,2,2,0,0)
        self.assertEqual(list(pixels),original)

    def test_bayer_finish_retains_more_dark_gradient_levels(self):
        means = []
        for red in range(32):
            pixels = (C.c_uint16*16)(*([red<<11]*16))
            self.lib.cover_gradient_finish(pixels,4,4,0,153)
            mean = sum(p>>11 for p in pixels)/16
            self.assertLessEqual(abs(mean-red*.4),.126)
            means.append(mean)
        self.assertEqual(len(set(means)),32)
        self.assertTrue(all(a<b for a,b in zip(means,means[1:])))
        self.assertGreater(len(set(means)),len({round(red*.4) for red in range(32)}))

    def test_strong_texture_spreads_midtones_but_preserves_black_and_white(self):
        image = (C.c_uint16*16)(*([(12<<11)|(24<<5)|12]*16))
        self.lib.cover_gradient_finish(image,4,4,0,153)
        for shift,mask,minimum_spread in ((11,31,3),(5,63,6),(0,31,3)):
            levels = [(p>>shift)&mask for p in image]
            self.assertGreaterEqual(max(levels)-min(levels),minimum_spread)
        for pixel,tint in ((0,0),(0xffff,0xffffff)):
            image = (C.c_uint16*15)(*([pixel]*15))
            self.lib.cover_gradient_finish(image,3,5,tint,255)
            self.assertEqual(list(image),[pixel]*15)

    def test_theme_tint_is_in_cache_identity_and_not_reapplied(self):
        image = (C.c_uint16*SAMPLES)(*([0xf800]*SAMPLES))
        def request(tint,alpha):
            return self.lib.cover_gradient_request(image,C.sizeof(image),32,32,64,0,tint,alpha)
        def poll(key):
            pixels = C.POINTER(C.c_uint16)()
            self.assertEqual(self.lib.cover_gradient_poll(key,C.byref(pixels)),1)
            return pixels
        dark = request(0,153)
        pixels = poll(dark)
        sample = list(pixels[:8])
        self.assertEqual(request(0,153),dark)
        self.assertEqual(list(poll(dark)[:8]),sample)
        light = request(0xf4f7fb,216)
        self.assertNotEqual(dark,light)
        self.assertGreater(sum(unpack(poll(light)[0])),sum(unpack(sample[0])))
        self.assertEqual(list(pixels[:8]),sample)


if __name__ == '__main__':
    unittest.main()
