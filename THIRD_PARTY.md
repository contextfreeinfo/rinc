# Third-party code in Rinc

Rinc's own code is dedicated to the public domain under CC0 1.0 (see LICENSE). These parts come from
others and keep their own licenses; their notices travel with builds that include them (`rinc -license`
on the desktop, and a comment in each web page).

| What | Where | License |
|---|---|---|
| TLV320DAC3100 register settings, from wili8jam (which took them from fruitjam-doom) | `src/rp2350/audio.c` | MIT (below) |
| HDMI data island packet encoding, from pico_hdmi | `src/rp2350/hdmi_packet.c`, `.h` | Unlicense (public domain) |
| FatFs R0.15a, by ChaN (the SD card's filesystem) | `lib/fatfs` (board builds) | 1-clause BSD style (`lib/fatfs/LICENSE.txt`) |
| Pico-PIO-USB | `lib/Pico-PIO-USB` (submodule; board builds) | MIT, Copyright (c) 2021 sekigon-gonnoc |
| Raspberry Pi Pico SDK | linked into board builds | BSD 3-Clause, Copyright (c) Raspberry Pi Ltd |
| TinyUSB (in the Pico SDK) | linked into board builds | MIT, Copyright (c) Ha Thach (tinyusb.org) |
| SDL 3 | fetched and linked into desktop builds | zlib, Copyright (c) Sam Lantinga |
| wasi-libc (from zig) | linked into web builds | MIT / Apache 2.0 with LLVM exception / CC0 parts |
| rio | `lib/rio` (submodule) | CC0 1.0 |

The 5x8 font (`src/core/font.c`, from spryjam's font files) and the apps are Rinc's own, under CC0.

## wili8jam (MIT)

```
MIT License

Copyright (c) 2026 Freewili

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
