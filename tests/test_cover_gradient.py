"""Color/quality oracles and immutable buffer handoff for the real generator."""
import ctypes as C
from pathlib import Path
import random
import statistics
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
        cls.lib.cover_gradient_render.argtypes = [C.POINTER(Color), C.c_void_p, C.c_uint, C.c_uint, C.c_size_t, C.c_uint32]
        cls.lib.cover_gradient_render_wash.argtypes = cls.lib.cover_gradient_render.argtypes + [C.c_uint32]
        cls.lib.cover_gradient_album_hash.argtypes = [C.c_char_p]
        cls.lib.cover_gradient_album_hash.restype = C.c_uint32
        cls.lib.cover_gradient_scale_cpu.argtypes = [C.c_void_p, C.c_uint, C.c_uint, C.c_size_t,
                                                    C.c_void_p, C.c_uint, C.c_uint]
        cls.lib.cover_gradient_request.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.c_uint,
                                                  C.c_size_t, C.c_int, C.c_uint32, C.c_uint8, C.c_int, C.c_uint32]
        cls.lib.cover_gradient_request.restype = C.c_uint64
        cls.lib.cover_gradient_poll.argtypes = [C.c_uint64, C.POINTER(C.POINTER(C.c_uint16))]
        cls.lib.cover_gradient_finish.argtypes = [C.c_void_p, C.c_uint, C.c_uint,
                                                 C.c_uint32, C.c_uint8, C.POINTER(C.c_uint16)]

    def setUp(self):
        self.lib.cover_gradient_cancel()

    def palette(self, values):
        samples = (Sample*SAMPLES)(*(Sample(*value) for value in values))
        palette = (Color*3)()
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
            self.assertEqual(palette, [color]*3)
        values = [(255,0,0,0)]*(SAMPLES-1) + [(0,0,255,255)]
        self.assertEqual(self.palette(values), (True, [(0,0,255)]*3))
        self.assertFalse(self.palette([(255,0,0,0)]*SAMPLES)[0])

    def test_palette_preserves_dominant_color_and_distinct_accents(self):
        colors = [(210,20,30), (25,40,210), (30,190,40)]
        values = [(*colors[0],255)]*600 + [(*colors[1],255)]*300 + [(*colors[2],255)]*124
        ok,palette = self.palette(values)
        self.assertTrue(ok)
        self.assertEqual(palette[0],colors[0])
        self.assertEqual(set(palette),set(colors))

    def test_small_accents_survive_neutral_borders_but_isolated_noise_does_not(self):
        values = [(128,128,128,255)]*900 + [(240,30,60,255)]*100 + [(30,80,230,255)]*24
        ok,palette = self.palette(values)
        self.assertTrue(ok)
        self.assertIn((240,30,60),palette)
        self.assertIn((30,80,230),palette)
        _,palette = self.palette([(128,128,128,255)]*1023+[(255,0,0,255)])
        self.assertTrue(all(max(p)-min(p)<=1 for p in palette))

    def render(self, colors, width=320, height=180, tint=0):
        palette = (Color*3)(*(Color(*p) for p in colors))
        pixels = (C.c_uint8*(width*height*3))()
        self.lib.cover_gradient_render(palette,pixels,width,height,width*3,tint)
        return pixels

    def wash(self, colors, album, width=320, height=180, tint=0):
        palette = (Color*3)(*(Color(*p) for p in colors))
        pixels = (C.c_uint8*(width*height*3))()
        seed = self.lib.cover_gradient_album_hash(album.encode('utf8'))
        self.lib.cover_gradient_render_wash(palette,pixels,width,height,width*3,tint,seed)
        return pixels

    def test_album_hash_is_stable_for_utf8_names_and_empty_metadata(self):
        for album in ('','Album','Another album','深海より愛を込めて'):
            expected = 2166136261
            for byte in album.encode('utf8'):expected = ((expected^byte)*16777619)&0xffffffff
            self.assertEqual(self.lib.cover_gradient_album_hash(album.encode('utf8')),expected)
        self.assertEqual(self.lib.cover_gradient_album_hash(None),2166136261)

    def test_album_wash_spreads_the_main_color_without_brightness_hotspots(self):
        colors = [(230,80,25),(200,35,160),(20,120,220)]
        old = self.render(colors)
        old_luma = [(29*old[i]+150*old[i+1]+77*old[i+2])/256 for i in range(0,len(old),3)]
        for album in ('Album','Another album','深海より愛を込めて'):
            pixels = self.wash(colors,album)
            luma = [(29*pixels[i]+150*pixels[i+1]+77*pixels[i+2])/256 for i in range(0,len(pixels),3)]
            self.assertLess(statistics.mean(luma),statistics.mean(old_luma))
            self.assertLess(statistics.pstdev(luma),1.5)
            self.assertLess(max(luma)-min(luma),4)
            rgb = list(zip(pixels[0::3],pixels[1::3],pixels[2::3]))
            warm = sum(r>g and r>b for b,g,r in rgb)/len(rgb)
            cool = sum(b>g and b>r for b,g,r in rgb)/len(rgb)
            self.assertGreater(warm,0.5)  # Main hue remains the broad base.
            self.assertGreater(cool,0.15)  # Accents must remain visibly distinct.
            self.assertGreater(max(pixels[0::3])-min(pixels[0::3]),30)
            self.assertGreater(max(pixels[2::3])-min(pixels[2::3]),30)
            light = self.wash(colors,album,tint=0xf4f7fb)
            light_luma = [(29*light[i]+150*light[i+1]+77*light[i+2])/256 for i in range(0,len(light),3)]
            self.assertGreater(min(light_luma),218)
            self.assertLess(max(light_luma),224)

    def test_album_wash_placement_is_deterministic_and_neutral_art_stays_neutral(self):
        colors = [(210,85,20),(180,30,190),(20,140,220)]
        a = bytes(self.wash(colors,'Album'))
        self.assertEqual(a,bytes(self.wash(colors,'Album')))
        self.assertNotEqual(a,bytes(self.wash(colors,'Another album')))
        neutral = self.wash([(100,100,100)]*3,'Album')
        self.assertTrue(all(neutral[i]==neutral[i+1]==neutral[i+2] for i in range(0,len(neutral),3)))

    def test_style_and_album_seed_are_part_of_wash_cache_identity(self):
        image = (C.c_uint16*SAMPLES)(*([0xf800]*600+[0x001f]*300+[0x07e0]*124))
        def request(style,album):
            seed = self.lib.cover_gradient_album_hash(album.encode('utf8'))
            return self.lib.cover_gradient_request(image,C.sizeof(image),32,32,64,0,0,153,style,seed)
        def poll(key):
            pixels = C.POINTER(C.c_uint16)()
            self.assertEqual(self.lib.cover_gradient_poll(key,C.byref(pixels)),1)
            return C.string_at(pixels,1280*720*2)
        lights = request(0,'Album');original = poll(lights)
        self.assertEqual(request(0,'Another album'),lights)
        self.assertEqual(poll(lights),original)
        wash = request(1,'Album');washed = poll(wash)
        self.assertNotEqual(wash,lights)
        self.assertEqual(request(1,'Album'),wash)
        self.assertEqual(poll(wash),washed)
        other = request(1,'Another album')
        self.assertNotEqual(other,wash)
        self.assertNotEqual(poll(other),washed)

    def test_color_lights_keep_text_regions_readable_in_dark_and_light_themes(self):
        colors = [(230,80,25), (200,35,160), (20,120,220)]
        for tint,light in ((0,False),(0xf4f7fb,True),(0xc0c0c0,True)):
            pixels = self.render(colors,tint=tint)
            for y in range(180):
                for x in range(320):
                    b,g,r = pixels[(y*320+x)*3:(y*320+x)*3+3]
                    luma = (77*r+150*g+29*b)/256
                    self.assertGreaterEqual(luma,174 if light else 0)
                    self.assertLessEqual(luma,255 if light else 97)
                    if x>=224 and 45<=y<=99:
                        self.assertGreaterEqual(luma,213 if light else 0)
                        self.assertLessEqual(luma,255 if light else 45)
            if not light:
                left = [pixels[(y*320+x)*3:(y*320+x)*3+3] for y in range(0,140,10) for x in range(0,140,10)]
                self.assertGreater(max(max(p)-min(p) for p in left),65)
        neutral = self.render([(100,100,100)]*3)
        self.assertTrue(all(neutral[i]==neutral[i+1]==neutral[i+2] for i in range(0,len(neutral),3)))

    def test_small_light_field_upscales_without_seams_or_excessive_color_error(self):
        colors = [(210,85,20), (180,30,190), (20,140,220)]
        small = self.render(colors)
        for y in range(180):
            for x in range(1,320):
                for c in range(3):
                    self.assertLessEqual(abs(small[(y*320+x)*3+c]-small[(y*320+x-1)*3+c]),8)
        full = self.render(colors,1280,720)
        scaled = (C.c_uint8*(1280*720*3))()
        self.lib.cover_gradient_scale_cpu(small,320,180,960,scaled,1280,720)
        rng = random.Random(9371)
        for x,y in [(0,0),(1279,719),(0,719),(1279,0)] + [(rng.randrange(1280),rng.randrange(720)) for _ in range(100)]:
            for c in range(3):self.assertLessEqual(abs(scaled[(y*1280+x)*3+c]-full[(y*1280+x)*3+c]),2)

    def test_result_cache_supersession_and_displayed_buffer_ownership(self):
        def request(pixel):
            image = (C.c_uint16*SAMPLES)(*([pixel]*SAMPLES))
            return self.lib.cover_gradient_request(image,C.sizeof(image),32,32,64,0,0,0,0,0)
        def poll(key):
            pixels = C.POINTER(C.c_uint16)()
            state = self.lib.cover_gradient_poll(key,C.byref(pixels))
            return state,pixels
        a = request(0xf800)
        state, front = poll(a)
        self.assertEqual(state, 1)
        original_value = front[0]
        self.assertNotEqual(original_value,0)
        original_address = C.cast(front,C.c_void_p).value
        self.assertEqual(request(0xf800), a)
        self.assertEqual(C.cast(poll(a)[1],C.c_void_p).value, original_address)
        b = request(0x07e0)
        self.assertNotEqual(a,b)
        self.assertEqual(front[0],original_value)  # Worker cannot overwrite a displayed image.
        c = request(0x001f)  # Replace an unconsumed result with newer artwork.
        self.assertEqual(poll(a)[0], 3)
        self.assertEqual(poll(b)[0], 3)
        state, current = poll(c)
        self.assertEqual(state, 1)
        self.assertNotEqual(current[0],original_value)
        self.assertNotEqual(C.cast(current,C.c_void_p).value, original_address)
        self.lib.cover_gradient_cancel()
        self.assertEqual(poll(c)[0], 3)
        self.assertEqual(request(0x001f), c)
        self.assertEqual(C.cast(poll(c)[1],C.c_void_p).value,C.cast(current,C.c_void_p).value)

    def test_benchmark_small_light_field_generation(self):
        colors = [(200,50,80),(30,140,200),(180,120,30)]
        timings = []
        for w,h in [(1280,720),(320,180)]:
            start = time.perf_counter()
            for _ in range(3):self.render(colors,w,h)
            timings.append((time.perf_counter()-start)/3*1000)
        print(f'\nHost ambient field: full={timings[0]:.2f} ms, small={timings[1]:.2f} ms')

    def finish(self, color, tint, alpha, width=64, height=64):
        count = width*height
        raw = (C.c_uint8*(count*3+16))(*([color[2],color[1],color[0]]*count+[0xa5]*16))
        lut = (C.c_uint16*(3*16*256))()
        self.lib.cover_gradient_finish(raw,width,height,tint,alpha,lut)
        self.assertEqual(list(raw[count*3:]),[0xa5]*16)
        pixels = C.cast(raw,C.POINTER(C.c_uint16))
        return list(pixels[:count])

    def test_blue_noise_is_static_balanced_and_has_no_four_pixel_grid(self):
        for color in ((0,0,0),(255,255,255),(18,52,86),(173,80,137)):
            for tint,alpha in ((0,76),(0xf4f7fb,108),(0xc0c0c0,115)):
                a = self.finish(color,tint,alpha)
                self.assertEqual(a,self.finish(color,tint,alpha))
                for channel,(shift,maximum,color_shift) in enumerate(((11,31,16),(5,63,8),(0,31,0))):
                    actual = sum((p>>shift)&maximum for p in a)/len(a)
                    target = (tint>>color_shift)&255
                    expected = (color[channel]*(255-alpha)+target*alpha)*maximum/(255*255)
                    self.assertLessEqual(abs(actual-expected),0.032)
        a = self.finish((37,92,181),0,76)
        different = sum(a[y*64+x]!=a[(y%4)*64+x%4] for y in range(64) for x in range(64))
        self.assertGreater(different,1000)

    def test_final_quantization_preserves_dark_gradient_precision(self):
        means = []
        for red in range(256):
            pixels = self.finish((red,0,0),0,153)
            mean = sum(p>>11 for p in pixels)/len(pixels)
            self.assertLessEqual(abs(mean-red*.4*31/255),.032)
            means.append(mean)
        self.assertGreaterEqual(len(set(means)),190)
        self.assertTrue(all(a<=b for a,b in zip(means,means[1:])))

    def test_finish_preserves_black_white_and_handles_odd_widths(self):
        for color,tint,expected in (((0,0,0),0,0),((255,255,255),0xffffff,0xffff)):
            self.assertEqual(self.finish(color,tint,255,3,5),[expected]*15)
        self.assertEqual(self.finish((255,0,0),0,0,3,5),[0xf800]*15)

    def test_theme_tint_is_in_cache_identity_and_not_reapplied(self):
        image = (C.c_uint16*SAMPLES)(*([0xf800]*SAMPLES))
        def request(tint,alpha):
            return self.lib.cover_gradient_request(image,C.sizeof(image),32,32,64,0,tint,alpha,0,0)
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
