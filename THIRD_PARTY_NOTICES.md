# Third-party notices

Playload Handheld is MIT-licensed (see `LICENSE`). It contains, or its release
archive bundles, the following third-party software and data. License texts are
in `licenses/` (in the release archive: `playload/licenses/`).

## Built into the player

| Component | Where | License |
|---|---|---|
| [Playload](https://github.com/kitao/playload) by Takashi Kitao: `engine.js`, `sound.js`, `glyphs.js`, `crt.js` (unmodified copies of v0.9.2) | `js/playload/`, embedded in the player and copied to `node/builtin/` | MIT (`PLAYLOAD-LICENSE.txt`) |
| Parts adapted from Playload: the AY-3-8910 model (`src/ay.c`, from `sound.js`), the Node.js/QuickJS hosts and the CRT shader setup (from `crt.js`) | `src/`, `js/` | MIT (`PLAYLOAD-LICENSE.txt`) |
| [QuickJS-ng](https://github.com/quickjs-ng/quickjs) 0.17.0 by Fabrice Bellard, Charlie Gordon, Ben Noordhuis, Saúl Ibarra Corretgé and contributors | `vendor/quickjs/`, linked into the player | MIT (`QUICKJS-LICENSE.txt`) |
| [SDL 2](https://www.libsdl.org/) headers (2.32.0) | `vendor/sdl2/include/`; the SDL library itself is provided by the device at run time | zlib (`vendor/sdl2/LICENSE.txt`) |

### Data inside Playload's `glyphs.js`

Playload's character set is built from these sources (see Playload's own license notes):

| Source | License |
|---|---|
| [Public Pixel](https://ggbot.itch.io/public-pixel-font) by GGBotNet | CC0 (`public-pixel-LICENSE.txt`) |
| [Unscii-8](https://github.com/viznut/unscii) by viznut | Public domain / CC0 (`unscii-README.md`) |
| [Misaki Gothic](https://littlelimit.net/misaki.htm) by Num Kadoma | Use, modification and redistribution permitted, including commercial use (`misaki-bdf-readme.txt`) |
| Emoji colors derived from [Twemoji](https://github.com/jdecked/twemoji) | CC BY 4.0 (`twemoji-color-LICENSE.txt`) |
| Character names from [Unicode data](https://www.unicode.org/Public/17.0.0/) | Unicode License v3 (`unicode-LICENSE.txt`) |

Also from Playload: the DB8 palette by Richard "DawnBringer" Fhager, Timothy
Lottes' public-domain CRT shader (as adapted in Playload's `crt.js`), and
Matthew Westcott's public-domain AY volume measurements.

## Bundled in the release archive

| Component | Where | License |
|---|---|---|
| [Node.js](https://nodejs.org/) v24.22.1, official linux-arm64 build, unmodified | `playload/node/node` | MIT, plus the licenses of the components Node.js bundles (`NODEJS-LICENSE.txt`) |
| `libdl.so.2`, `libpthread.so.0` from Debian 12 package `libc6` 2.36-9+deb12u14 (GNU C Library), unmodified | `playload/node/lib/` | LGPL-2.1-or-later (`LGPL-2.1.txt`, `debian/libc6.copyright`) |
| `libstdc++.so.6`, `libgcc_s.so.1` from Debian 12 packages `libstdc++6` and `libgcc-s1` 12.2.0-14+deb12u1 (GCC), unmodified | `playload/node/lib/` | GPL-3.0-or-later with the GCC Runtime Library Exception 3.1 (`GPL-3.0.txt`, `debian/gcc-12-base.copyright`) |

These libraries are included only because Node.js needs them and some
handhelds do not ship them. They are the exact files from Debian's binary
packages (`tools/package.sh` downloads them with pinned SHA-256 checksums).

### Source code of the GNU libraries

The complete corresponding source code is available from Debian:

- glibc 2.36-9+deb12u14: <https://snapshot.debian.org/package/glibc/2.36-9%2Bdeb12u14/>
- gcc-12 12.2.0-14+deb12u1: <https://snapshot.debian.org/package/gcc-12/12.2.0-14%2Bdeb12u1/>

## Bundled games

| Game | License |
|---|---|
| `chiptune`, `glyph-quest`, `jump`, `shooter`, `snake`: Playload's example games by Takashi Kitao, as Playload game URLs | MIT (`PLAYLOAD-LICENSE.txt`) |
| `brr3kb` (Bit-Rate Rush 3KB) by game-de-it | MIT (`LICENSE`) |

Playload Handheld is an unofficial project and is not affiliated with or
endorsed by the Playload project or its author.
