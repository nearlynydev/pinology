# License and provenance notices

Pinology is an independent, AI-assisted experimental project. It is not
affiliated with or endorsed by Synology, Realtek, Raspberry Pi or QEMU.
Their names and trademarks remain the property of their respective owners.

## Project license

The combined project is distributed under **GNU GPL version 2**, as reproduced
in [LICENSE](LICENSE). Existing file-level `GPL-2.0-or-later` notices remain
valid for those files; they do not override GPL-2.0-only upstream material.
New Pinology launcher, test and documentation contributions use GPL-2.0-only
unless a file explicitly states otherwise. Third-party notices below remain
applicable. No exclusive authorship of upstream work is claimed.

## Sources and dependencies

| Component | Role / provenance | Terms |
| --- | --- | --- |
| [QEMU](https://www.qemu.org/) 11.1.1 | Emulator downloaded at build time; maintained board models and integration patches are under `kernel/stock-board/qemu` | GPLv2 overall; individual upstream files may have other compatible terms |
| [libslirp](https://gitlab.freedesktop.org/slirp/libslirp) 4.9.5 | User-mode networking; small host-forward backlog patch | BSD-style terms; retained [license](LICENSES/libslirp-LICENSE.txt) and [credits](LICENSES/libslirp-COPYRIGHT.txt) |
| [virtual-dsm](https://github.com/vdsm/virtual-dsm) | Inspiration for delivery/settings; its pinned container supplies `extract.py` when building our Docker runtime | [MIT notice](LICENSES/virtual-dsm-MIT.txt); not the ARM board implementation |
| Synology GPL / Realtek Linux sources | Register maps, tables and interface behavior used in the RTD1619B models and vendor-partition reader | Original GPL terms; see details below |
| Debian, Homebrew and Python dependencies | Local/container toolchain and runtime libraries, downloaded separately | Respective component licenses; no blanket relicensing |

The build scripts pin source archive hashes. Container bases and the extractor
are pinned by digest. Apt/Homebrew packages are not fully version-locked; this
is a repeatable build recipe, **not** a bit-for-bit reproducibility claim.

### Realtek/Synology source references

Reference archive: Synology GPL `7.2-72806/rtd1619b/linux-5.10.x.txz`, from the
[Synology GPL source distribution](https://global.synologydownload.com/download/ToolChain/Synology%20NAS%20GPL%20Source/7.2-72806/rtd1619b/linux-5.10.x.txz),
SHA-256 `01e8abeb04ac17ac804c719b19975d209e4db87020e1594a1c5e0e72968746b5`.
It is not included in this repository. Relevant reference files include:

- `drivers/clk/realtek/clk-rtd1619b-cc.c`, `clk-rtd1619b-ic.c`, `clk-pll.c`,
  `clk-det.c`, `clk-regmap-gate.c` and `reset.c` (clock/reset tables and protocols).
- `drivers/gpio/gpio-rtd.c` and Realtek interrupt-multiplexer drivers (GPIO/IRQ).
- `drivers/nvmem/rtk-efuse.c` (OTP protocol), carrying
  `Copyright (C) 2016-2020 Realtek Semiconductor Corporation` and original
  author credit `Cheng-Yu Lee <cylee12@realtek.com>`, GPL-2.0-only.
- `rtk_sb2_sem.c`, `rtk_sb2_inv.c`, `chip.c` (SB2/SoC behavior).
- `drivers/mtd/mtdpart.c` (vendor partition format and additive checksum).

Other upstream copyright holders and notices remain in their original source
files. The project emulates selected interfaces, not every hardware feature.
Stock DTB register addresses were also used as research inputs. No private
OTP contents, extracted kernel, DTB or proprietary userspace is distributed.

Retained reference-source credits also include Realtek Semiconductor
Corporation (clock/reset sources, 2017-2020; IRQ mux, 2017-2021; SB2, 2017-2019),
Cheng-Yu Lee (clock/OTP/SB2 source author), and Andreas Färber (GPIO, 2017;
chip identification, 2017-2019). The adapted clock, OTP and SB2 files use
GPL-2.0-only to respect their reference-source restrictions.

## DSM and distribution boundaries

**DSM is not covered by Pinology's GPL license.** No DSM PAT, kernel, ramdisk,
firmware dump, license, activation credential or serial number is supplied.
Users obtain software separately and must assess their rights under the
[Synology EULA](https://www.synology.com/en-global/company/legal/terms_EULA)
and applicable law. Owning a physical device or entering its serial number
does not automatically establish permission for this use.

The initial public release is source-only. Local build commands create bundles
and Docker images for the operator; they are not automatically published.
Before redistributing any compiled bundle/image, provide corresponding source,
patches, build scripts and all dependency notices required by its licenses.
A link to this repository alone is not a substitute for complete corresponding
source for every redistributed copyleft component. No promise of legal
clearance or official support is made.
