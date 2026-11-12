# vita-memio

<p align="center"><img src="assets/logo.png" alt="vita-memio: faster storage for PS Vita" width="720"></p>

**Faster storage for your PS Vita.** vita-memio speeds up a microSD in an SD2Vita and the Sony memory card. Copies, VPK installs and game loading get quicker, and your files stay exactly as they are.

It is a taiHEN plugin for HENkaku/Ensō, tested on firmware **3.65**, and a fork of **[Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage)** by [Juan Garcia Alvite](https://github.com/juanjsebgarcia), whose idea and research it builds on.

## What you get

Faster reads and writes, with the biggest gain in anything that reads files in order. Every measured value is under [Benchmarks](#benchmarks). Results depend on your microSD, and games usually gain less than a benchmark.

**Your data is safe by design.** A program reads exactly what it would read without the plugin. Writes are never delayed: when a write returns, the data is on the card. If anything goes wrong, that storage goes back to the normal way for the rest of the session.

## Install

Get `vitamemio_kernel.skprx`, `vitamemio_hud.skprx`, `vitamemio.suprx` and, if you want to measure your card, `vitamemio_tester.vpk` from the [latest release](https://github.com/Osaul4ik/vita-memio/releases/latest).

1. Copy `vitamemio_kernel.skprx`, `vitamemio_hud.skprx` and `vitamemio.suprx` to `ur0:tai/`.
2. Add these lines to your taiHEN config. If `ux0:tai/config.txt` exists, edit that one, otherwise `ur0:tai/config.txt`:

   ```
   *KERNEL
   ur0:tai/vitamemio_kernel.skprx

   *NPXS10015
   ur0:tai/vitamemio.suprx
   ```
   `vitamemio_hud.skprx` does not go in `config.txt`: the plugin loads it by itself.
3. Restart the console.
4. Open **Settings > Devices > vita-memio** and switch on the storage you use. Everything starts switched off.

That is all: the other defaults are the recommended ones. Back up your cards first, as with any plugin that touches storage.

**To update:** replace all three files and restart. Always update them together.

## Uninstall

1. In **Settings > Devices > vita-memio**, switch off every storage.
2. Remove the plugin's lines from `config.txt`.
3. Restart the console.
4. Delete `ur0:tai/vitamemio_kernel.skprx`, `ur0:tai/vitamemio_hud.skprx`, `ur0:tai/vitamemio.suprx` and the folder `ur0:data/vita-memio/`.

## Settings

Everything is in **Settings > Devices > vita-memio** and takes effect at once, no restart needed.

| Setting | What it does | Default |
|---|---|---|
| Driver | Shows whether the plugin is running. Select it for details | |
| Faster SD2Vita / memory card | Speeds up that storage | Off |

**Advanced**

| Setting | What it does | Default |
|---|---|---|
| GPU-safe reads | Makes sure the graphics chip always sees data correctly. **All**: after every read, costs a little speed. **Non-DMA**: not after *DMA copy*'s copies, faster; if a game shows graphics glitches, use All. Recommended for playing | Non-DMA |
| Overlapped reads | Faster reads. **Auto** tunes itself to your card, **Fixed** uses built-in tuning | Auto |
| Copy on the system core | Moves copying from the game's CPU core to the console's spare core, so the game's core is free sooner. See below | C + NC R/A |
| DMA copy | Copying is done by the console's DMA controller instead of the CPU, in about half the time. It switches itself off if anything goes wrong | On |
| Read ahead | Reads the next part of a file before it is asked for: **Adaptive**, **On** or **Off**. See below | Adaptive |
| Small transfers too | Also speeds up transfers under 32 KiB, the file system's own bookkeeping | On |
| Overlay (SELECT + arrows) | A small menu over any game, see below. Off takes effect at once; On after a restart, if it was off when the console started | On |

**Developer settings** (inside *Advanced*)

| Setting | What it does | Default |
|---|---|---|
| Verify transfers | Reads every transfer again the normal way and compares. Slow, for checking only | Off |
| Enable log | Writes logs to `ur0:data/vita-memio/` | Off |
| Statistics | Collects timings for the tester app; switch it on before testing | Off |

If the plugin ever could not start properly, a **Reset safe mode** row appears under *Driver*.

### Copy on the system core

| Choice | What runs on the spare core |
|---|---|
| Off | Nothing |
| Cached only | Copies of ordinary reads of 16 KiB or more |
| **C + NC R/A** | That, plus what *Read ahead* already read for graphics data. Without *Read ahead* it is the same as *Cached only* |
| C + NC All | That, plus the graphics data reads themselves. In my tests this only moved work between cores, so it is here mostly for experimenting |

In numbers: with *Read ahead* on, copying graphics data on the spare core took the work on the game's cores from 12.2 to 1.7 ms per MiB, at about 1% less speed. Ordinary 1 MiB reads cost the game's core 3 ms per MiB instead of 22.

### Read ahead

While a file is being read in order, the spare core reads the next part early, so the card does not sit idle between requests. It starts after five large reads in a row, or sixteen of 32 KiB; smaller reads in order are read in 32 KiB blocks from the eighth on.

Data read early is thrown away if anything writes over it, so it can never be stale, and *Verify transfers* checks it like everything else. It uses about 40 KiB more memory per storage.

It helps most when a game reads long stretches in order, and very little when a game jumps around. That varies a lot from game to game, which is what the test below is for.

**Adaptive** starts from the same point (the game's own from the R/A Test, or five) and moves it while you play: it starts sooner while what it reads early is used, and later while much of it is thrown away. It stays between two and eight reads and never switches itself off; a kind of read the test set to *never* stays never. While the R/A Test runs, read ahead uses the standard five for both kinds, and nothing adapts.

All benchmark values are under [Benchmarks](#benchmarks). The gain in real use is smaller.

## Overlay

*Overlay* in *Advanced* is on by default. Switched off, it is gone at once; switched on again, it works at once if it was on when the console started, else after a restart. In any game:

| Keys | |
|---|---|
| SELECT + Right | Opens the overlay; again: closes it and shows the HUD |
| SELECT + Left | From the HUD back to the overlay; again: closes it |
| SELECT + Up or Down | Closes everything |
| Up / Down | Choose a line |
| Left / Right, Cross | Change a value, or do the action |

A game that uses SELECT + Right itself will open the overlay too. While the overlay is open, the game gets no buttons.

The overlay sets things **for this game only**; each setting is *Global* until you change it. White means standard, blue changed but not saved, green saved and different from the standard. *Show advanced* adds Overlapped reads and GPU-safe reads. **Save** keeps your changes for this game.

### R/A test: let the plugin choose for your game

1. Open the overlay and choose **Start R/A Test**, ideally before the game loads.
2. Play for a few minutes, including a loading screen. The HUD shows how much was read.
3. Open the overlay and choose **Stop R/A Test**.

The plugin works out what read ahead brings this game and saves the result for it: on or off (*Adaptive* stays *Adaptive*), when to start (*C streak* for ordinary reads, *NC streak* for graphics data) and whether to copy on the spare core. If too little was read, it says so and changes nothing.

It judges from how the game reads and cannot see the frame rate. The result depends on what you played, so test the part you care about. Test during play only and it may decide read ahead is not worth it, because it never saw the loading.

## HUD

The HUD is part of the overlay and shows through its keys; with *Overlay* off there is no HUD. It shows, for the storage the game reads from:

```
SD2VITA R: 21.8 W:  0.0 MB/s
R/W: 909/1 MiB On Err: 0
R/A: On  TitleID: PCSF00024  B
Streak  C: 5  NC: 3
Core 3  C: 98%  NC: 0%
```

- **R / W:** speed right now. **R/W:** total read and written while the HUD or the overlay was on screen.
- **B:** the game's buffers for copying on the spare core, since it started (also in the overlay): **green** all usable; **yellow** some had a first page the system cannot share, so that part was copied on the game's core; **red** some could not be used at all, and were copied on the game's core. The data is right either way. Not shown while nothing has been copied there yet.
- **Streak:** reads in a row before read ahead starts. With *Adaptive* as set:now, for example `C: 5:6`: set at 5, at 6 now.
- **Core 3, C:** the share of copying done on the spare core since the HUD opened. Copies of graphics data from read ahead count here.
- **Core 3, NC:** the share of graphics data reads done on the spare core. It stays at 0% unless copying is on *C + NC All*.

## Tester

Install `vitamemio_tester.vpk` to measure your own card. It tests reads and writes of several sizes and many small files, checks every byte, and adds each result to `ur0:data/vita-memio/results.txt`. It never changes the plugin's settings: to compare, change a setting and run it again. For the copy timings in the report, switch on *Statistics* (Advanced > Developer settings) first.

## Benchmarks

Measured with the tester (quick profile: 16 MiB file, 3 runs, every byte checked) on a PCH-1000, firmware 3.65, with a microSD in an SD2Vita. With the plugin: *Overlapped reads* on *Auto*, *Copy on the system core* on *C + NC R/A*, *Small transfers too* on. Columns go from slowest to fastest.

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

*NC* is non-cacheable memory, as games use for graphics data; *NCphy* and *CDRAM* are other kinds of memory games keep it in.

- **Results depend on your microSD.** Cards differ a lot, especially in writes, so your numbers will not match these.
- **The gain from *Read ahead* is from the benchmark,** which reads files strictly in order. In real use it is smaller, and in games it depends on how each game reads: some gain a lot, some almost nothing.
- **Writes vary between runs,** because the card sometimes pauses to do its own housekeeping. *Read ahead* and *GPU-safe reads* do not change how writes work, so differences in those rows are not from them.
- Most game loading is not limited by storage, so games gain less than file copies.

## Error C1-2569-2

This error means the system is out of kernel memory. It can also appear as a crash right after you switch the plugin on, or when you open Settings while another app is running.

1. If another app is running, close it first.
2. Leave the plugin on for only one storage. Each storage you switch on takes 256 KiB.
3. Switch off *Read ahead*, *Overlapped reads* and *Copy on the system core*.
4. Switch off *Overlay*, then restart.
5. Remove other kernel plugins from your taiHEN config.

## Good to know

- Only the storages you switch on are touched. The internal storage, game cards, USB storage and Wi-Fi never are.
- Memory: 256 KiB per storage you switch on, 128 KiB more while *Overlapped reads* or *Copy on the system core* are in use, and about 40 KiB more with *Read ahead*. The overlay and the HUD take about 40 KiB more; with *Overlay* off, nothing.
- If the console does not stay on the home screen for 30 seconds after starting, the next start loads nothing, so a bad setting cannot trap you in a reboot loop. Holding **L** while the console starts skips all plugins.
- Tested on one console so far: a PCH-1000 on firmware 3.65. PCH-2000 and PS TV use the same storage system but are untested.
- The Settings page and the HUD are in English.

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

**Built on**

- [taiHEN](https://github.com/yifanlu/taiHEN) by Yifan Lu: the hooking framework.
- [HENkaku](https://github.com/henkaku/henkaku) and [Ensō](https://github.com/TheOfficialFloW/enso) by the HENkaku team and TheFloW. The Settings page is added the way HENkaku's own Settings plugin does it ([`plugin/user.c`](https://github.com/henkaku/henkaku/blob/master/plugin/user.c)).
- [VitaSDK](https://vitasdk.org) and its [headers](https://github.com/vitasdk/vita-headers).

**Ideas and references**

- [IOStaging](https://github.com/bythos14) by bythos14: the staging-buffer idea the original project credits as its inspiration.
- [YAMT](https://github.com/SKGleba/yamt-vita) by SKGleba: the layout names of the Devices page. No YAMT code is used.
- [PSVshell](https://github.com/Electry/PSVshell) by Electry: the look of the tester and of the PSVshell-style HUD. No PSVshell code is used.

**Included**

- [Terminus Font](https://terminus-font.sourceforge.net/) by Dimitar Toshkov Zhekov: a subset for the tester and the HUD, under the SIL Open Font License 1.1 (`include/OFL-Terminus.txt`, also inside the VPK).

**Artwork**

- The logo, the tester's icon and its LiveArea are original artwork drawn by `tools/gen_art.py`; their lettering is set in [Inter](https://rsms.me/inter/) by Rasmus Andersson (SIL Open Font License 1.1).

**Tools**

- [vitacompanion](https://github.com/devnoname120/vitacompanion) by devnoname120, [VitaShell](https://github.com/TheOfficialFloW/VitaShell) by TheFloW, [cppcheck](https://cppcheck.sourceforge.io/), [QEMU](https://www.qemu.org/) and GCC's sanitizers.

## License

MIT, as the original project: see [LICENSE](LICENSE) and the [original license](https://github.com/juanjsebgarcia/vita-turbo-storage/blob/main/LICENSE). The Terminus Font subset in `include/vm_font_terminus.h` stays under the SIL Open Font License 1.1.
