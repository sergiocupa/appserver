# Third-party notices

This repository redistributes source code written by others. Each item below keeps
its own license and its own copyright holders; the project's own `LICENSE` does not
cover them and does not sublicense them.

The list is about **source redistribution**. Patent licensing for H.264 and H.265 is
a separate matter, not addressed here.

---

## Codecs

| directory | project | license | license file | what is redistributed |
|---|---|---|---|---|
| `codecs/x265/` | x265 (MulticoreWare) | **GPL v2** | `COPYING` | full source |
| `codecs/vpx/` | libvpx (The WebM Project) | BSD 3-clause | `LICENSE` | full source |
| `codecs/opus/` | Opus (Xiph.Org and others) | BSD 3-clause | `COPYING` | full source |
| `codecs/dav1d/` | dav1d (VideoLAN) | BSD 2-clause | `COPYING` | full source |
| `codecs/svtav1/` | SVT-AV1 (Alliance for Open Media) | BSD 3-Clause Clear + AOM patent license | `LICENSE.md` | full source |
| `codecs/openh264/` | OpenH264 (Cisco Systems) | BSD 2-clause | `LICENSE` | headers and export files only |
| `codecs/libde265/` | libde265 (struktur AG, Dirk Farin) | **LGPL v3 or later** | `COPYING` | headers only |
| `test/H26XDesktopRendererTest/deps/sdl2/` | SDL2 (Sam Lantinga) | zlib | `LICENSE.txt` | headers only (binaries are not redistributed) |

`codecs/libyuv/` is built from the copy inside `codecs/vpx/third_party/libyuv`
(BSD 3-clause), covered by the libvpx entry.

---

## x265 — the one with strings attached

x265 is under the **GNU General Public License v2**. Redistributing its source, as
this repository does, is allowed: the `COPYING` file and every copyright notice are
intact and unmodified.

The obligation that matters is about the **combined work**. A program linked with
x265 must itself be distributed under the GPL.

In this project x265 is reached only through `codec_h265_plugin`, a module loaded at
run time. Nothing else links or references it. A build shipped **without that module**
does not distribute the combined work, and the GPL obligation does not arise. If you
do intend to ship it, either accept the GPL for the whole distribution or obtain a
commercial x265 license from MulticoreWare.

---

## Modified third-party files

Only one third-party file in this repository differs from its original:

**`codecs/openh264/include/wels/codec_app_def.h`** — the field `float rPsnr[3]`,
added by OpenH264 2.6.0 at the end of `SLayerBSInfo`, is commented out. The DLL used
here is 2.5.1, and because `SFrameBSInfo` embeds `sLayerInfo[128]` inline, the extra
field changed the stride between layers (40 → 56 bytes) and misaligned the reads. The
reason is written in the file itself, and the BSD notice was kept in full.

Everything else is byte-for-byte as published upstream.

---

## Attribution

The BSD, zlib and Clear BSD licenses here require that redistributions keep the
copyright notice, the conditions and the disclaimer. They are kept, both inside each
source file and in the license file of each directory.
