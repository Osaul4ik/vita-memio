# vita-memio

Faster SD2Vita, memory card and internal storage on the PS Vita, as a taiHEN plugin for HENkaku/Ensō on firmware **3.60–3.74**.

vita-memio is a fork of **[Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage)** by [Juan Garcia Alvite](https://github.com/juanjsebgarcia). The technique, the research behind it and the original implementation are his; see the original repository for the full documentation.

## How it works

Before and after every transfer, the system's storage driver cleans and invalidates the CPU cache over the whole caller buffer, and that work is not overlapped with the transfer. For large transfers vita-memio hands the system's own, unmodified read and write functions a non-cacheable staging buffer instead (the driver skips the cache work for it) and copies between the two with the CPU. The driver still issues every command, handles every error and runs the DMA.

A caller buffer that is itself non-cacheable (games load data for the graphics chip into such memory) stays on the standard path: the driver has no cache work there, and without the copy it is faster (SD2Vita, 1 MiB reads: 21.0 MiB/s standard vs 16.5 MiB/s through a staging buffer).

- Writes are never cached or deferred: a write has reached the card before the call returns.
- Small transfers (under 32 KiB, which includes the file system's own bookkeeping) take the standard path, unless **Small transfers too** is switched on.
- Any error switches that storage back to the standard path for the session and finishes the request the standard way.
- On an unknown firmware nothing is installed.
- Game cards, USB storage and Wi-Fi are not touched.

**The one difference from the standard path.** What the CPU reads is always identical, but after a faster read the data can stay in the CPU cache for a while before it reaches memory (the standard driver's DMA writes memory directly). A game that loads a file straight into cached memory the graphics chip reads, without flushing the cache itself, could in principle draw stale data (non-cacheable graphics memory takes the standard path and is not affected). **GPU-safe reads** removes the difference: each read's copy is written out to memory before the call returns. Check what it costs with the tester (the *copy* time per block) and switch it on while playing.

## Performance

| Test | Original: stock | Original: Turbo Storage | vita-memio (my console) |
|---|---:|---:|---:|
| SD2Vita (microSD) reads, 1 MiB requests | 13.3 MB/s | 16.8 MB/s | **17.7 MiB/s** |
| SD2Vita (microSD) reads, 128 KiB requests | 12.4 MB/s | 15.7 MB/s | **16.8 MiB/s** |
| SD2Vita (microSD) writes | 4.4–5.4 MB/s | 6.0–6.3 MB/s | **14.5–16.9 MiB/s** |
| SD2Vita (microSD) reads into non-cacheable memory, 1 MiB | | | 21.0 MiB/s (standard path, see above) |
| Sony memory card reads, 1 MiB requests | 10.5 MB/s | 13.8 MB/s | **14.1 MiB/s** |
| Sony memory card writes, 1 MiB requests | 9.1 MB/s | 10.0 MB/s | **13.2–13.8 MiB/s** |
| Internal storage reads | 14.2–15.5 MB/s | 19.0–21.1 MB/s | **19.2–21.3 MiB/s** |
| Internal storage writes | 7.4–8.1 MB/s | 8.2–8.8 MB/s | **7.6 MiB/s** |

- **Original:** the figures published in the [original README](https://github.com/juanjsebgarcia/vita-turbo-storage#headline-figures). PCH-1000, firmware 3.60, a 128 GB microSD in an SD2Vita, an official 4 GB memory card; 64 MiB per run, stock and Turbo Storage alternated.
- **vita-memio:** my console on firmware 3.65, with the plugin on, measured with the included tester: my microSD in an SD2Vita (`ux0:`), the official Sony memory card (`uma0:`) and the internal storage (`ur0:`). Quick profile: a 16 MiB file and three runs, every byte checked; a range means the averages of separate sessions. For comparison, the stock SD2Vita on the same console reads 1 MiB requests at 13.6 MiB/s.

Different microSD cards write at very different speeds, so the SD2Vita write row compares cards more than plugins. Results depend on the console, the card and the workload.

## Compared with the original

Against Vita Turbo Storage as of 29 October 2026 (commit [`e48f309`](https://github.com/juanjsebgarcia/vita-turbo-storage/commit/e48f309)).

**Added**

- **Tester app** (`vitamemio_tester.vpk`): sequential, random and small-file tests on one storage, every byte checked. It shows where the time goes per block (system call vs. CPU copy), the plugin's live counters, verify results and buffer-busy counts, and saves every run to `results.txt`.
- **Driver status row** at the top of the Settings page: active, not loaded, safe mode, firmware not supported, or could not start. The page also appears, and says so, when the kernel module is missing.
- **GPU-safe reads**: an option that writes read data out of the CPU cache to memory, removing the one difference the original lists under its limitations.
- **Small transfers too**: an option to speed up transfers under 32 KiB (the file system's own bookkeeping) as well.
- **Verify transfers** as a Settings switch (in the original it is set over the command port only).
- **Statistics** switch: the per-transfer timers and data counters can be turned off.
- **PSVshell-style HUD**: a black panel with coloured values in the Terminus font, sized for the game's resolution, next to the original look (*HUD style*).
- **Host tests** for the storage path, settings, HUD drawing and tester engine, run with sanitizers and an ARM build under QEMU on every push.

**Changed**

- The Settings page is under **Settings > Devices** (the original: Settings > System), with an **Advanced** sub-page.
- Only the kernel module and the Settings app load the plugin: no `*main` line in `config.txt`.
- Reads copy out of the staging buffer with 8-word burst loads instead of `memcpy` (SD2Vita: 4.5 to 4.2 µs per 512-byte block).
- Staging buffers are allocated only for the storages that are switched on (the original always allocates 768 KiB).
- Logging is off unless **Enable log** is on, and then it records one line per start plus problems.
- The HUD panel is composed once per text change and only copied each frame.

**Removed**

- Developer mode and its TCP command port (1340).

**In the original but not here**

- The HUD's corner choice (here always top right), *In-game HUD* and *HUD recent activity only*.

## Install

Build artifacts (GitHub Actions, or `make build`): `vitamemio_kernel.skprx`, `vitamemio.suprx` and the tester `vitamemio_tester.vpk`.

1. Copy `vitamemio_kernel.skprx` and `vitamemio.suprx` to `ur0:tai/`. Always install the two together.
2. Add to `ur0:tai/config.txt` (`make config` prints these lines):

   ```
   *KERNEL
   ur0:tai/vitamemio_kernel.skprx

   *NPXS10015
   ur0:tai/vitamemio.suprx
   ```
3. Restart the console. Everything is switched off until you switch it on in Settings.
4. Optional: install `vitamemio_tester.vpk` with VitaShell to measure your own storage.

## Settings

**Settings > Devices > vita-memio**

| Row | What it does | Default |
|---|---|---|
| Driver: … | Whether the kernel module runs: active, not loaded, safe mode, firmware not supported, or could not start. Select it for the details | |
| Faster SD2Vita | The faster path for a microSD in an SD2Vita adapter | Off |
| Faster memory card | The faster path for the official PS Vita memory card | Off |
| Faster internal storage | The faster path for the console's own storage | Off |

**Advanced**

| Row | What it does | Default |
|---|---|---|
| GPU-safe reads | After each faster read, writes the data out of the CPU cache to memory, as the standard path leaves it (see below). Costs some read speed for reads into ordinary memory; non-cacheable memory (such as the graphics memory) takes the standard path anyway | Off |
| Overlapped reads (experimental) | Splits each faster read in two: while the card reads the second part, the CPU already copies the first, hiding most of the copy (and of the GPU-safe cleaning). Uses a second 128 KiB buffer and a helper thread per storage (on the system core, which games do not use), made when it is first switched on and kept until the console restarts | Off |
| Small transfers too | Also speeds up transfers under 32 KiB: the file system's own bookkeeping (folders, file entries, opening and creating files). Experimental: test it with Verify transfers | Off |
| Verify transfers | Reads every faster transfer again the standard way and compares. Slower; for checking | Off |
| Enable log | Writes `kernel.log` and `user.log`: one line per start, otherwise only problems. Off: nothing is written | Off |
| Statistics | Times every faster transfer (system call vs. copy) and counts the data moved, for the tester. Off saves that work on every transfer; the HUD counts its own speeds while it is on | On |
| Storage HUD | Live speed of each storage in use, in the top right corner: Off, Compact, or Extended (adds totals and errors) | Off |
| HUD style | Classic (white text) or PSVshell (a black panel with coloured values) | PSVshell |
| Reset safe mode | Ends safe mode; vita-memio starts normally after the next restart | |

Settings apply at once, with no restart (in safe mode or on an unsupported firmware they are saved and take effect at the next normal start). They are kept in `ur0:data/vita-memio/`, together with the logs and the tester's results.

**Safe mode.** If a start does not reach the home screen and stay up there for 30 seconds, the next start installs nothing, so a broken setup cannot loop (restarting by hand within those 30 seconds also leads to safe mode once). The Driver row then shows safe mode. Settings can still be changed in safe mode: switch off the option that caused it, then **Reset safe mode** and restart. Holding L while the console starts skips all plugins.

## Tester

The tester (`vitamemio_tester.vpk`, "vita-memio Tester") runs reads and writes of several sizes on one storage, then creates, reads and deletes many small files (files per second: mostly the file system's own small transfers). It checks every byte and appends the result to `ur0:data/vita-memio/results.txt`. It never changes the plugin: switch a storage on or off in Settings, then run the test again to compare.

Its main screen shows live, for each storage, what the plugin is doing (state, data moved, errors, verified transfers, mismatches). Each result also shows:

- where the time goes per 512-byte block (the system call vs. the CPU copy), with **Statistics** on;
- the plugin's verify results;
- how many large transfers went the standard way because the staging buffer was busy.

It also reads the file into non-cacheable memory (*NC read 1M / 128K*), the kind games load data for the graphics chip into. vita-memio leaves such reads to the standard path, so these rows are the same with the plugin on and off; the ordinary rows show what the plugin (and, with **GPU-safe reads** on, the cache cleaning) does for ordinary memory.

To see what **Small transfers too** does, run the test with it off and with it on and compare the *files create / read / delete* rows.

Pick the storage that the test folder is on: on most setups both the SD2Vita and the memory card are `ux0:`. If the storage and folder don't match, the test says so.

## Build

```sh
make build        # the modules and the tester, in the pinned VitaSDK Docker image
make test         # host tests (no VitaSDK needed)
make config       # the config.txt lines
```

`make deploy`, `make log` and `make results` talk to vitacompanion on the console (`VITA_IP=...`). See [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits and projects used

All credit for the original project goes to **[Juan Garcia Alvite](https://github.com/juanjsebgarcia)**, the author of [Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage): the technique, the research, the original implementation, testing and documentation.

**Built on**

- [taiHEN](https://github.com/yifanlu/taiHEN) by Yifan Lu: the hooking framework both modules use.
- [HENkaku](https://github.com/henkaku/henkaku) and [Ensō](https://github.com/TheOfficialFloW/enso) by the HENkaku team and TheFloW: the homebrew environment. How the Settings page is added and how its buttons are handled follows HENkaku's own Settings plugin ([`plugin/user.c`](https://github.com/henkaku/henkaku/blob/master/plugin/user.c)).
- [VitaSDK](https://vitasdk.org) and its [headers](https://github.com/vitasdk/vita-headers): the toolchain.

**References and ideas**

- [IOStaging](https://github.com/bythos14) by bythos14: the staging-buffer idea the original project credits as its inspiration.
- [YAMT](https://github.com/SKGleba/yamt-vita) by SKGleba: the layout names of the Devices page in Settings. No YAMT code is used.
- [PSVshell](https://github.com/Electry/PSVshell) by Electry: the look of the tester and of the PSVshell-style HUD. No PSVshell code is used.

**Included**

- [Terminus Font](https://terminus-font.sourceforge.net/) by Dimitar Toshkov Zhekov: a subset is used by the tester and the PSVshell-style HUD, under the SIL Open Font License 1.1 (`include/OFL-Terminus.txt`, also inside the VPK).

**Tools**

- [vitacompanion](https://github.com/devnoname120/vitacompanion) by devnoname120 for `make deploy` / `log` / `results`.
- [VitaShell](https://github.com/TheOfficialFloW/VitaShell) by TheFloW to install the tester.
- [cppcheck](https://cppcheck.sourceforge.io/), [QEMU](https://www.qemu.org/) and GCC's sanitizers for the tests on GitHub Actions.

## License

MIT, as the original project: see [LICENSE](LICENSE) and the [original license](https://github.com/juanjsebgarcia/vita-turbo-storage/blob/main/LICENSE). The Terminus Font subset in `include/vm_font_terminus.h` stays under the SIL Open Font License 1.1.
