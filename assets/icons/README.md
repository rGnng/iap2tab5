The Albums menubar icon in [albums.svg](albums.svg) is the **disc** icon from
[Simple Line Icons](https://github.com/thesabbir/simple-line-icons).
The supplied SVG path is retained as provided and converted to a font glyph by
`tools/gen_icon_font.py` when `tools/gen_icons.sh` runs.

Copyright (c) 2016 Sabbir Ahmed & All Contributors.
Licensed under the [MIT license](LICENSE.simple-line-icons), copied from the
[upstream license](https://github.com/thesabbir/simple-line-icons/blob/master/LICENSE.md).

The notification icons `bluetooth-x.svg` and `bluetooth-connecting.svg` are
from [Andrés Raxón's icons](https://github.com/andrsrxn/icons), obtained from
the [Iconify collection](https://github.com/iconify/icon-sets/blob/master/json/andrsrxn.json).
Copyright (c) 2026 Andrés Raxón (andrsrxn), [MIT license](LICENSE.andrsrxn).
Their original paths and translucent fills are retained.

`arrow-repeat.svg`, `battery-charging.svg`, `battery-full.svg`, `battery-low.svg`,
and `plug.svg` are from [Bootstrap Icons 1.13.2](https://github.com/twbs/icons/tree/v1.13.2/icons),
under the [MIT license](../fonts/LICENSE.bootstrap-icons).
Run `tools/gen_notification_icons.py` with CairoSVG 2.8.2 and Pillow 12.0.0
to regenerate the 32 px alpha images in `app/ui/notification_icons.h`.
The UI recolors these images for the active theme and rotates the sync icon.
