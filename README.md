# vita-memio

<p align="center"><img src="assets/logo.png" alt="vita-memio: faster storage for PS Vita" width="720"></p>

**Faster storage for your PS Vita.** vita-memio speeds up a microSD in an SD2Vita and the Sony memory card. Copies, VPK installs and game loading get quicker, and your files stay exactly as they are.

It is a taiHEN plugin for HENkaku/Ensō, tested on firmware **3.65**, and a fork of **[Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage)** by [Juan Garcia Alvite](https://github.com/juanjsebgarcia), whose idea and research it builds on.

DO NOT use these two plugins together under any circumstances.

**Your data is safe by design.** A program reads exactly what it would read without the plugin. Writes are never delayed: when a write returns, the data is on the card. If anything goes wrong, that storage goes back to the normal way for the rest of the session.

## Install

Download `vita-memio-<version>.zip` from the [latest release](https://github.com/Osaul4ik/vita-memio/releases/latest) and unpack it. It holds the plugin (`vitamemio_kernel.skprx`, `vitamemio_hud.skprx`, `vitamemio.suprx`), the tester (`vitamemio_tester.vpk`) and `SHA256SUMS`.

1. Copy `vitamemio_kernel.skprx`, `vitamemio_hud.skprx` and `vitamemio.suprx` to `ur0:tai/`.
2. Add these lines to your taiHEN config (`ux0:tai/config.txt` if it exists, otherwise `ur0:tai/config.txt`). `vitamemio_hud.skprx` does not go there: the plugin loads it by itself.

   ```
   *KERNEL
   ur0:tai/vitamemio_kernel.skprx

   *NPXS10015
   ur0:tai/vitamemio.suprx
   ```
3. Restart the console.
4. Open **Settings > Devices > vita-memio** and switch on the storage you use. Everything else is already set as recommended.

Back up your cards first, as with any plugin that touches storage. **To update:** replace all three files and restart; always update them together.

**To uninstall:** switch off every storage (or *Driver*), remove the lines from `config.txt`, restart, then delete the three files and the folders `ur0:data/vita-memio/` and `ux0:data/vita-memio/`.

## Settings

Everything is in **Settings > Devices > vita-memio** and takes effect at once, except *Driver*.

| Setting | What it does | Default |
|---|---|---|
| Driver | Whether the plugin starts with the console. Off: from the next start it loads nothing; this page stays, so you can switch it back on | On |
| Faster SD2Vita / memory card | Speeds up that storage | Off |
| Low kernel memory | Shown while both storages are on. The two share one set of buffers: 384 KiB less kernel memory. The app in front comes first: its reads from either card are sped up in turn, other apps' go the standard way meanwhile. Copying between the cards gains less. Takes effect after a restart | Off |
| *Advanced* | | |
| GPU-safe reads | Makes sure the graphics chip sees the data. **All**: after every read. **Non-DMA**: not after *DMA copy*'s copies, faster. If a game shows graphics glitches, use All | Non-DMA |
| Overlapped reads | Faster reads. **Auto** tunes itself to your card, **Fixed** uses built-in tuning | Auto |
| Copy on the system core | Moves copying to the console's spare core, so the game's core is free sooner: *Off*, *Cached only*, *C + NC R/A* (plus graphics data read ahead), *C + NC All* (mostly for experimenting) | C + NC R/A |
| DMA copy | Copying is done by the DMA controller, in about half the time. Switches itself off if anything goes wrong | On |
| Read ahead | Reads the next part of a file before it is asked for: **Adaptive**, **On** or **Off**. See below | Adaptive |
| Small transfers too | Also speeds up transfers under 32 KiB | On |
| Overlay (SELECT + arrows) | A menu and HUD over any game. Off takes effect at once; On after a restart if it was off at the start | On |
| Profiles for system apps | The home screen and system apps get their own settings and learning, like games | On |
| *Developer settings* | | |
| Verify transfers | Reads every transfer again the normal way and compares. Slow, for checking only | Off |
| Enable log | Logs to `ur0:data/vita-memio/`, with a summary of each app's reads when it leaves the front | Off |
| Statistics | Collects timings for the tester app | Off |

If the plugin ever could not start properly, a **Reset safe mode** row appears under the status.

### Read ahead

While a file is read in order, the spare core reads the next part early, so the card does not sit idle between requests. Data read early is thrown away if anything writes over it, so it is never stale, and it is not read at all while a write runs. It takes about 40 KiB more memory per storage. Read ahead is for the app in front only: other apps' reads (the system's, music in the background) are still sped up, but they neither use nor break its streams.

**Adaptive** works out for each game, while you play, after how many reads in a row read ahead pays off (large reads, 32 KiB reads and very small reads, each on its own) and how big a block to read for very small ones. It also counts what read ahead actually saved in that game, for ordinary memory, graphics memory and very small reads. Where it comes out as a loss, read ahead switches off for that kind of read, and back on when the game's own figures show a gain. Everything it learns is kept for every game, also across restarts.

**On** uses fixed points: five large reads, sixteen of 32 KiB, or eight very small ones.

## Results in games

The plugin's own estimates from its log (PCH-1000, microSD in an SD2Vita, v1.5.0), from one short session per game of half a minute to four minutes, with loading screens. *Saved* is the time read ahead took off the game's waits for the card, on top of what the other settings give. These are rough figures, not stopwatch measurements, and they change with what you play.

| Game | Session | Reads took | Saved by read ahead |
|---|---:|---:|---:|
| PCSG01179 | 174 s | 9.4 s | 52.8 s (85%) |
| GTA San Andreas (port) | 87 s | 15.1 s | 22.5 s (60%) |
| PCSB00404 | 28 s | 2.8 s | 12.4 s (81%) |
| NFS Underground 2 (port) | 104 s | 11.7 s | 12.0 s (51%) |
| PCSB00419 | 82 s | 3.0 s | 2.0 s (40%) |
| PCSB00986 | 90 s | 2.9 s | 1.6 s (35%) |
| PCSE01254 | 134 s | 8.2 s | 1.5 s (16%) |
| PCSB00554 | 95 s | 4.7 s | 1.5 s (24%) |
| PCSB01030 | 104 s | 10.2 s | 1.1 s (10%) |
| PCSB00296 | 78 s | 5.8 s | 1.0 s (15%) |
| PCSF00024 | 167 s | 32.1 s | 0.8 s (2%) |
| PCSB00245 | 64 s | 2.1 s | 0.7 s (24%) |
| PCSB00724 | 67 s | 3.4 s | 0.3 s (8%) |
| PCSB00118 | 222 s | 6.0 s | 0.3 s (4%) |

The percentage is the share of the waiting it removed. Games that read files in many very small pieces gain the most. Most loading is not limited by storage, so a loading screen gets shorter by less than this.

## Overlay and HUD

In any game, **SELECT + Right** opens the overlay and, pressed again, closes it and shows the HUD. **SELECT + Left** goes back; **SELECT + Up/Down** closes everything. Up/Down choose a line, Left/Right or Cross change it. While the overlay is open, the game gets no buttons.

The overlay sets things **for this game only**. Each line is *Global* (as in Settings) until you change it: white is standard, blue changed but not saved, green saved. **Save** keeps them in `ux0:data/vita-memio/ra/`. *Show advanced* adds Overlapped reads and GPU-safe reads.

- **Tuning** (*Auto* by default) measures, for each storage, what read ahead saves on your console and what a read for nothing costs; Adaptive then uses these figures. They are kept with what Adaptive learned in `ux0:data/vita-memio/tuning.bin` (about 370 bytes per game), written only when something changed. Triangle, twice, forgets them.
- **R/A Test** (shown while Read ahead is *On* or *Off*): start it, play a few minutes including a loading screen, stop it. The plugin then saves for this game whether read ahead pays off and when it starts.

```
SD2VITA R: 21.8 W:  0.0 MB/s
R/W: 909/1 MiB On Err: 0
R/A: Adaptive  TitleID: PCSF00024
C:5:5/16/3 NC:3:2/16/3 Auto
Core 3 B C: 98%  NC: 0%
```

- **R / W:** speed now; **R/W:** total while the HUD or overlay was on screen.
- **C / NC:** where read ahead starts for cached and graphics memory, as *set:now* for large / 32 KiB / very small reads; `-` where Adaptive switched it off. **Auto** is green while Tuning measures.
- **Core 3 C / NC:** the share of copying done on the spare core. **B:** whether the game's buffers could be used there (green all, yellow partly, red not; the data is right either way).

## Tester

`vitamemio_tester.vpk` measures your own card: reads and writes of several sizes and many small files, every byte checked, each result added to `ur0:data/vita-memio/results.txt`. It never changes the plugin's settings. For copy timings, switch on *Statistics* first.

## Benchmarks

Tester, quick profile (16 MiB file, 3 runs, every byte checked), PCH-1000, firmware 3.65, microSD in an SD2Vita. Plugin: *Overlapped reads* Auto, *Copy on the system core* C + NC R/A, *Small transfers too* on.

| Test | Unit | Without plugin | GPU-safe on | GPU-safe on + Read ahead | GPU-safe off + Read ahead |
|---|---|---:|---:|---:|---:|
| Sequential read 1 MiB | MiB/s | 14.9 | 18.8 | 23.0 | 26.4 |
| Sequential read 128 KiB | MiB/s | 14.3 | 16.7 | 30.0 | 31.8 |
| Random read 4 KiB | IOPS | 379.1 | 371.9 | 371.7 | 409.9 |
| Random read 32 KiB | IOPS | 360.8 | 361.6 | 361.4 | 397.4 |
| Random read 128 KiB | IOPS | 113.9 | 133.2 | 133.3 | 141.7 |
| Random read 1 MiB | IOPS | 14.9 | 18.8 | 18.5 | 19.2 |
| NC read 1 MiB | MiB/s | 21.0 | 21.1 | 25.4 | 25.4 |
| NC read 128 KiB | MiB/s | 19.7 | 19.7 | 44.2 | 44.8 |
| NCphy read 1 MiB | MiB/s | 21.1 | 21.1 | 25.4 | 25.5 |
| CDRAM read 1 MiB | MiB/s | 21.0 | 21.0 | 26.2 | 26.3 |
| NC random read 1 MiB | IOPS | 21.0 | 21.0 | 20.6 | 20.6 |
| Files read | files/s | 206.8 | 209.6 | 247.3 | 250.0 |
| Files created | files/s | 45.8 | 41.9 | 39.6 | 44.1 |
| Files deleted | files/s | 147.7 | 154.6 | 124.4 | 143.9 |
| Sequential write 1 MiB | MiB/s | 14.1 | 17.9 | 15.3 | 15.2 |
| Random write 4 KiB | IOPS | 170.4 | 180.2 | 165.4 | 172.7 |
| Random write 32 KiB | IOPS | 322.4 | 391.0 | 321.8 | 326.0 |
| Random write 128 KiB | IOPS | 105.1 | 138.0 | 107.5 | 113.7 |
| Random write 1 MiB | IOPS | 14.6 | 17.6 | 13.6 | 16.4 |

*NC*, *NCphy* and *CDRAM* are the kinds of memory games keep graphics data in. Your card will give other numbers, especially for writes, which also vary between runs as the card does its own housekeeping. *Read ahead* and *GPU-safe reads* do not change how writes work.

## Error C1-2569-2

The system is out of kernel memory (it can also show as a crash right after switching the plugin on). Close other apps, switch on *Low kernel memory* or leave only one storage on (each takes up to 416 KiB), switch off *Read ahead*, *Overlapped reads*, *Copy on the system core* and *Overlay*, restart, and remove other kernel plugins if needed.

## Good to know

- Only the storages you switch on are touched; never the internal storage, game cards, USB storage or Wi-Fi.
- Memory: 256 KiB per storage (with *Low kernel memory*, one set for both), 128 KiB more with *Overlapped reads* or *Copy on the system core*, about 40 KiB with *Read ahead* and 40 KiB with *Overlay*.
- While the console starts, every transfer goes the standard way; the plugin comes on once the home screen is up. If the console restarts before the plugin has shown it works, the next start loads nothing, so a bad setting cannot cause a reboot loop. Holding **L** at start skips all plugins.
- Its files on the internal storage are only the settings, the boot guard and the log; what it learns goes to `ux0:data/vita-memio/`.
- Tested on one console: a PCH-1000 on firmware 3.65. PCH-2000 and PS TV use the same storage system but are untested.

## Disclaimer

This software is provided **"as is", without warranty of any kind**, and you use it at your own risk. **Back up your memory card and SD card before installing.** See the [MIT License](LICENSE).

## Build

```sh
make build        # the modules and the tester, in the pinned VitaSDK Docker image
make test         # host tests (no VitaSDK needed)
make config       # prints the config.txt lines
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for more.

## Credits

All credit for the original project goes to **[Juan Garcia Alvite](https://github.com/juanjsebgarcia)**, the author of [Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage): the technique, the research, the original implementation, its testing and documentation.

- **Built on:** [taiHEN](https://github.com/yifanlu/taiHEN) by Yifan Lu; [HENkaku](https://github.com/henkaku/henkaku) and [Ensō](https://github.com/TheOfficialFloW/enso) by the HENkaku team and TheFloW (the Settings page is added the way HENkaku's own Settings plugin does it, [`plugin/user.c`](https://github.com/henkaku/henkaku/blob/master/plugin/user.c)); [VitaSDK](https://vitasdk.org) and its [headers](https://github.com/vitasdk/vita-headers).
- **Ideas and references:** [IOStaging](https://github.com/bythos14) by bythos14 (the staging-buffer idea the original project credits); [YAMT](https://github.com/SKGleba/yamt-vita) by SKGleba (the layout names of the Devices page); [PSVshell](https://github.com/Electry/PSVshell) by Electry (the look of the tester and the HUD). No YAMT or PSVshell code is used.
- **Included:** a subset of [Terminus Font](https://terminus-font.sourceforge.net/) by Dimitar Toshkov Zhekov, under the SIL Open Font License 1.1 (`include/OFL-Terminus.txt`, also inside the VPK).
- **Artwork:** the logo, the tester's icon and LiveArea are original, drawn by `tools/gen_art.py`; their lettering is set in [Inter](https://rsms.me/inter/) by Rasmus Andersson (SIL Open Font License 1.1).
- **Tools:** [vitacompanion](https://github.com/devnoname120/vitacompanion) by devnoname120, [VitaShell](https://github.com/TheOfficialFloW/VitaShell) by TheFloW, [cppcheck](https://cppcheck.sourceforge.io/), [QEMU](https://www.qemu.org/) and GCC's sanitizers.

## License

MIT, as the original project: see [LICENSE](LICENSE) and the [original license](https://github.com/juanjsebgarcia/vita-turbo-storage/blob/main/LICENSE). The Terminus Font subset in `include/vm_font_terminus.h` stays under the SIL Open Font License 1.1.
