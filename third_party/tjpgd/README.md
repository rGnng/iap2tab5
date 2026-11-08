TJpgDec R0.03 by ChaN, from https://elm-chan.org/fsw/tjpgd/arc/tjpgd3.zip
(archive SHA-256: `052fe3efbc9a8be29f31597ad009c5b51a4f6905878eb28569e0ab3d46d0c013`).

Original license is retained at the top of the source. The 2023 grayscale
conversion patch from https://elm-chan.org/fsw/tjpgd/patches.html is applied.
Local configuration enables scaling, RGB565 output, and level 1 fast decode.

The complete upstream scaling implementation is needed: LVGL 9.6 disables
scaling and removes its pixel-reduction code, so setting its JD_USE_SCALE
to 1 alone produces incorrect pixels. Firmware builds this copy with private
entry-point names through `scaled_tjpgd.c`; stock LVGL is unchanged.
