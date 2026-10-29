# vita-memio

**A fork of [Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage)**

[Original project and full documentation](https://github.com/juanjsebgarcia/vita-turbo-storage) · [Original author's profile](https://github.com/juanjsebgarcia)

> [!IMPORTANT]
> This is not an independent implementation. The original project, including its design, research, implementation, benchmark results, and documentation, is the work of Juan Garcia Alvite, who deserves full credit. This fork is maintained for personal use on my own PS Vita.
>

## About the original project

Vita Turbo Storage is a taiHEN plugin for PS Vita that reduces storage-transfer overhead by changing the buffer passed to the system's existing storage routines. It is designed to improve read performance, with smaller gains for writes, while leaving the storage drivers themselves in place.

The upstream project supports HENkaku/Ensō on firmware **3.60–3.74** and includes a kernel module, a Settings page, an optional live read/write HUD, per-storage controls, diagnostics, and fallback handling.

Supported storage types in the original project:

- **SD2Vita** (microSD in the game-card slot)
- **Official PS Vita memory card**
- **Internal storage**

Game cards and USB storage are not accelerated.

## Performance

The original author reported the following results on the upstream test console: a PS Vita PCH-1000 running firmware 3.60, with a 128 GB microSD card in SD2Vita and an official 4 GB memory card.

| Test | Stock | Turbo Storage | Reported change |
|---|---:|---:|---:|
| SD2Vita reads, 1 MiB requests | 13.3 MB/s | 16.8 MB/s | +26% |
| SD2Vita reads, 128 KiB requests | 12.4 MB/s | 15.7 MB/s | +27% |
| SD2Vita writes | 4.4–5.4 MB/s | 6.0–6.3 MB/s | +17–36% |
| Vita memory card reads, 1 MiB requests | 10.5 MB/s | 13.8 MB/s | +32% |
| Internal storage reads | 14.2–15.5 MB/s | 19.0–21.1 MB/s | +34–36% |

**These are the upstream author's measurements, not independent benchmarks from this fork.** Results depend on the console, firmware, storage device, and workload. See the [original README](https://github.com/juanjsebgarcia/vita-turbo-storage#headline-figures) for the full benchmark tables, methodology, and limitations.

## What this fork changes

- Renamed to vita-memio (files, Settings page, data folder `ur0:data/vita-memio/`).
- A staging buffer (256 KiB of kernel memory) only for the storages that are switched on, allocated when one is first switched on.
- The Settings page is under **Settings > Devices**, with an **Advanced** sub-page (verify, logging, HUD, HUD style, safe-mode reset).
- The HUD sits in the top right corner and has a second look modelled on PSVshell's overlay (black panel, Terminus font, coloured values).
- Safe mode (the crash-loop guard) ends as soon as the home screen has loaded, instead of after a fixed 30 s, and can be reset from Settings.
- Logging is off unless **Enable log** is on.
- Faster copy out of the staging buffer on reads (8-word burst loads).
- The developer network mode (TCP command port) is removed; the user module now runs only in the Settings app.
- Logging is reduced to one line per start plus problems.
- A tester app (`vitamemio_tester.vpk`) that only measures, never changes the plugin, and shows the plugin's counters, verify results and where the time goes (system I/O vs. copy).

## Install (this fork)

Build artifacts (GitHub Actions, or `make`): `vitamemio_kernel.skprx`, `vitamemio.suprx` and the tester app `vitamemio_tester.vpk`.

1. Copy `vitamemio_kernel.skprx` and `vitamemio.suprx` to `ur0:tai/`.
2. Add to `ur0:tai/config.txt` (`make config` prints these lines):

   ```
   *KERNEL
   ur0:tai/vitamemio_kernel.skprx

   *NPXS10015
   ur0:tai/vitamemio.suprx
   ```
3. Reboot. The switches are in **Settings > Devices > vita-memio**:

   | Row | What it does |
   |---|---|
   | Faster SD2Vita / memory card / internal storage | the faster path per storage |
   | **Advanced >** Verify transfers | re-reads every faster transfer the standard way and compares (slower; for checking) |
   | **Advanced >** Enable log | writes `kernel.log` / `user.log` (the start line and problems only); off: nothing is written |
   | **Advanced >** Storage HUD | off, compact or extended, in the top right corner |
   | **Advanced >** HUD style | Classic (white text) or PSVshell (black panel, coloured values) |
   | **Advanced >** Reset safe mode | ends safe mode; vita-memio starts normally after the next restart |

   **Safe mode:** if a start never reaches the home screen, the next start installs nothing (safe mode) so a broken setup cannot loop. The guard is cleared once the home screen has loaded.
4. Optional: install `vitamemio_tester.vpk` with VitaShell to measure your own card (plugin off and on, set in Settings). Its main screen shows, live, what the plugin does for each storage, including the counters of **Verify transfers**.

Settings, logs and test results live in `ur0:data/vita-memio/`. With logging on, the log holds one line per start and otherwise only problems.

**Upgrading from an earlier vita-memio build:** the `*main` / `ur0:tai/vitamemio.suprx` lines are no longer needed (the developer network mode is gone); remove them. Left in, they do nothing. Settings saved by builds before the Advanced page (settings file version 1) are not read: everything starts at its default once, so switch your storages on again. Install the `.skprx` and the `.suprx` together: the Settings page uses a kernel call that older kernel modules do not have.

**Upgrading from a build named `storageturbo`:** replace the `storageturbo_kernel.skprx` / `storageturbo.suprx` lines in `config.txt` with the ones above and delete the old files from `ur0:tai/` (never load both); its settings are not taken over. The old tester (`TURBOTST0`) and `ur0:data/turbostorage/` can be deleted.

## Credits and projects used

All credit for the original project goes to **[Juan Garcia Alvite](https://github.com/juanjsebgarcia)**, the author of [Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage), including the original implementation, technical approach, testing and documentation. This fork is based on it.

**Built on**

- [taiHEN](https://github.com/yifanlu/taiHEN) by Yifan Lu: the hooking framework both modules use (kernel export hooks for the storage path, user import hooks for the Settings page).
- [HENkaku](https://github.com/henkaku/henkaku) and [Ensō](https://github.com/TheOfficialFloW/enso) by the HENkaku team and TheFloW: the homebrew environment the plugin runs in. The way the Settings page is added (wait for `system_settings_core.suprx`, then hook the layout loader and the registry calls) follows HENkaku's own Settings plugin (`plugin/user.c`).
- [VitaSDK](https://vitasdk.org) and its [headers](https://github.com/vitasdk/vita-headers): the toolchain; CI builds with a pinned VitaSDK Docker image.

**References and ideas**

- [IOStaging](https://github.com/bythos14) by bythos14: the staging-buffer idea the original project credits as its inspiration.
- [YAMT](https://github.com/SKGleba/yamt-vita) by SKGleba: where the Devices page's layout names (`peripherals_settings_plugin`, `peripherals_settings`) were learned, and the reason vita-memio also accepts YAMT's replacement Devices page. No YAMT code is used.
- [PSVshell](https://github.com/Electry/PSVshell) by Electry: the look of the tester (panels, title bar, status lines) and of the PSVshell-style HUD (black panel with rounded corners, Terminus font picked by the framebuffer size, coloured values) follows its overlay. No PSVshell code is used.
- [HENkaku's Settings plugin](https://github.com/henkaku/henkaku/blob/master/plugin/user.c): besides the page itself, the way a Settings button is handled (wrapping the page plugin's button function from `scePafToplevelInitPluginFunctions`), used for **Reset safe mode**.

**Included**

- [Terminus Font](https://terminus-font.sourceforge.net/) by Dimitar Toshkov Zhekov: the tester and the PSVshell-style HUD use a subset of it (8x14, 10x18 and 12x24 bold), under the SIL Open Font License 1.1 (`include/OFL-Terminus.txt`, also inside the VPK). The classic HUD uses its own 8x8 font.

**Tools**

- [vitacompanion](https://github.com/devnoname120/vitacompanion) by devnoname120: FTP and commands for `make deploy`, `make log` and `make results`.
- [VitaShell](https://github.com/TheOfficialFloW/VitaShell) by TheFloW: to install the tester VPK and copy the modules.
- [cppcheck](https://cppcheck.sourceforge.io/), [QEMU](https://www.qemu.org/) (the ARM copy test) and GCC's AddressSanitizer/UBSan for the host tests, run on GitHub Actions.

## License

MIT, as the original project: see [LICENSE](LICENSE) and the [upstream license](https://github.com/juanjsebgarcia/vita-turbo-storage/blob/main/LICENSE). The Terminus Font subset in `include/vm_font_terminus.h` stays under the SIL Open Font License 1.1.
