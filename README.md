# vita-memio

Faster SD2Vita, memory card and internal storage on the PS Vita, as a taiHEN plugin for HENkaku/Ensō on firmware **3.60–3.74**.

vita-memio is a fork of **[Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage)** by [Juan Garcia Alvite](https://github.com/juanjsebgarcia). The technique, the research behind it and the original implementation are his; see the original repository for the full documentation.

## How it works

Before and after every transfer, the system's storage driver cleans and invalidates the CPU cache over the whole caller buffer, and that work is not overlapped with the transfer. For large transfers vita-memio hands the system's own, unmodified read and write functions a non-cacheable staging buffer instead (the driver skips the cache work for it) and copies between the two with the CPU. The driver still issues every command, handles every error and runs the DMA.

- Writes are never cached or deferred: a write has reached the card before the call returns.
- Small transfers (under 32 KiB, which includes the file system's own bookkeeping) take the standard path.
- Any error switches that storage back to the standard path for the session and finishes the request the standard way.
- On an unknown firmware nothing is installed.
- Game cards, USB storage and Wi-Fi are not touched.

## Performance

| Test | Original: stock | Original: Turbo Storage | vita-memio (my console) |
|---|---:|---:|---:|
| SD2Vita reads, 1 MiB requests | 13.3 MB/s | 16.8 MB/s | **17.7 MiB/s** |
| SD2Vita reads, 128 KiB requests | 12.4 MB/s | 15.7 MB/s | **16.8 MiB/s** |
| SD2Vita writes | 4.4–5.4 MB/s | 6.0–6.3 MB/s | **14.5–16.9 MiB/s** |
| Memory card reads, 1 MiB requests | 10.5 MB/s | 13.8 MB/s | **14.1 MiB/s** |
| Memory card writes, 1 MiB requests | 9.1 MB/s | 10.0 MB/s | **13.2–13.8 MiB/s** |
| Internal storage reads | 14.2–15.5 MB/s | 19.0–21.1 MB/s | **19.2–21.3 MiB/s** |
| Internal storage writes | 7.4–8.1 MB/s | 8.2–8.8 MB/s | **7.6 MiB/s** |

- **Original:** the figures published in the [original README](https://github.com/juanjsebgarcia/vita-turbo-storage#headline-figures). PCH-1000, firmware 3.60, a 128 GB microSD in an SD2Vita, an official 4 GB memory card; 64 MiB per run, stock and Turbo Storage alternated.
- **vita-memio:** my console on firmware 3.65, with the plugin on, measured with the included tester. Quick profile: a 16 MiB file and three runs, every byte checked; a range means the averages of separate sessions. For comparison, the stock SD2Vita on the same console reads 1 MiB requests at 13.6 MiB/s.

Different cards write at very different speeds, so the write rows compare cards more than plugins. Results depend on the console, the card and the workload.

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
| Verify transfers | Reads every faster transfer again the standard way and compares. Slower; for checking | Off |
| Enable log | Writes `kernel.log` and `user.log`: one line per start, otherwise only problems. Off: nothing is written | Off |
| Storage HUD | Live speed of each storage in use, in the top right corner: Off, Compact, or Extended (adds totals and errors) | Off |
| HUD style | Classic (white text) or PSVshell (a black panel with coloured values) | PSVshell |
| Reset safe mode | Ends safe mode; vita-memio starts normally after the next restart | |

Settings apply at once, with no restart. They are kept in `ur0:data/vita-memio/`, together with the logs and the tester's results.

**Safe mode.** If a start never reaches the home screen, the next start installs nothing, so a broken setup cannot loop. The Driver row then shows safe mode. Holding L while the console starts skips all plugins.

## Tester

The tester (`vitamemio_tester.vpk`, "vita-memio Tester") runs reads and writes of several sizes on one storage, checks every byte, and appends the result to `ur0:data/vita-memio/results.txt`. It never changes the plugin: switch a storage on or off in Settings, then run the test again to compare.

Its main screen shows live, for each storage, what the plugin is doing (state, data moved, errors, verified transfers, mismatches). Each result also shows:

- where the time goes per 512-byte block (the system call vs. the CPU copy);
- the plugin's verify results;
- how many large transfers went the standard way because the staging buffer was busy.

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
