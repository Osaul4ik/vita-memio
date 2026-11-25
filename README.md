# vita-memio

<p align="center"><img src="assets/logo.png" alt="vita-memio: faster storage for PS Vita" width="720"></p>

**Faster storage for your PS Vita.** vita-memio speeds up a microSD in an SD2Vita and the Sony memory card. Copies, VPK installs and game loading get quicker, and your files stay exactly as they are.

It is a taiHEN plugin for HENkaku/Ensō, tested on firmware **3.65**, and a fork of **[Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage)** by [Juan Garcia Alvite](https://github.com/juanjsebgarcia), whose idea and research it builds on.

DO NOT use these two plugins together under any circumstances.

**Your data is safe by design.** Games and apps get exactly the data they would get without the plugin. Writes are never held back: when a write is done, the data is on the card. If anything goes wrong, that storage goes back to the normal way until the next restart.

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

Back up your cards first, as with any plugin that touches storage.

## Update

1. Open **Settings > Devices > vita-memio** and switch **Driver** off.
2. Restart the console.
3. Replace `vitamemio_kernel.skprx`, `vitamemio_hud.skprx` and `vitamemio.suprx` in `ur0:tai/` with the new ones. Always replace all three together.
4. Delete the folders `ur0:data/vita-memio/` and `ux0:data/vita-memio/`.
5. Restart the console.
6. Open **Settings > Devices > vita-memio** again and switch on the storage you use.

Deleting the folders sets everything back to the defaults: settings, the games' overlay profiles and *Tuning*'s measurements. Tuning measures your card again by itself while you play.

## Uninstall

Switch **Driver** off, remove the plugin's lines from `config.txt` and restart. Then delete the three files from `ur0:tai/` and the folders `ur0:data/vita-memio/` and `ux0:data/vita-memio/`.

## Settings

Everything is in **Settings > Devices > vita-memio** and takes effect at once, except *Driver*. You only need to switch on the storage you use: the rest is already set the way that works best.

| Setting | What it does | Default |
|---|---|---|
| Driver | Whether the plugin starts with the console. Off: from the next start it loads nothing, but this page stays so you can switch it back on | On |
| Faster SD2Vita / memory card | Speeds up that storage | Off |
| Low kernel memory | Shown while both storages are on. They share one set of buffers, so the plugin needs 384 KiB less memory. The game in front comes first; copying between the two cards gains less. Takes effect after a restart | Off |
| *Advanced* | | |
| GPU-safe reads | Makes sure the graphics chip sees the data a game loads. **Non-DMA** is faster; if a game ever shows graphics glitches, choose **All** | Non-DMA |
| Overlapped reads | Reads big files in parts, so the card never waits for the copying. **Auto** tunes itself to your card | Auto |
| Copy on the system core | Lets the console's spare core do copies that would otherwise keep the game's core busy. With *DMA copy* on, most copying is done by the DMA chip anyway, so this only handles what is left. Keep **C + NC R/A**: *Off* and *Cached only* also switch off reading again from memory (see below) | C + NC R/A |
| DMA copy | Copies are made by the console's DMA chip, about twice as fast as by the processor. Switches itself off if anything goes wrong | On |
| Read ahead | Reads the next part of a file before the game asks for it: **Adaptive**, **On** or **Off**. See below | Adaptive |
| Small transfers too | Also speeds up transfers under 32 KiB, such as opening folders and small files | On |
| Overlay (SELECT + arrows) | A menu and an information panel (HUD) over any game. Off takes effect at once; On after a restart if it was off when the console started | On |
| Profiles for system apps | The home screen and the system's apps get their own settings and learning, like games | On |
| *Developer settings* | | |
| Verify transfers | Reads every transfer a second time the normal way and compares. Slow, for checking only; nothing is learned meanwhile | Off |
| Enable log | Writes a log to `ur0:data/vita-memio/`, with a summary of each game's reads when you leave it | Off |
| Statistics | Collects timings for the tester app | Off |

If the plugin ever could not start properly, a **Reset safe mode** row appears under the status.

### Read ahead

A memory card is slowest when it has to wait for the next request. While a game reads a file in order, vita-memio reads the next part early, so it is already in memory when the game asks for it. It also recognises games that read a file in big and small pieces in turn, or that read again a little of what they just read. Data read early is thrown away if anything writes over it, so a game never gets old data. Read ahead works for the game in front; other apps (the system, music in the background) are still sped up but don't get it.

**Adaptive** (the default) learns while you play, separately for each kind of read, when reading ahead saves time in that game. Where it doesn't, it switches read ahead off for that kind of read, and back on when it would help again. Every launch starts learning afresh; if you go to the home screen and come back, the game goes on where it was.

**On** always reads ahead after a fixed number of reads in a row; **Off** never does.

### Reading again from memory

Many commercial games read the same graphics data again a moment later. vita-memio keeps the latest such reads in memory and gives them to the game from there instead of from the card. There is nothing to set:

- It uses the buffers of the storage you are not using at that moment (the SD2Vita's while you play from the memory card, or the other way round). If only one storage is on, it sets aside 384 KiB of its own the first time a game needs it.
- It works only for commercial games (title IDs starting with PCS), not for homebrew, Adrenaline or the system's apps.
- It switches itself off in a game where it does not pay off.
- It needs *Read ahead* (Adaptive or On) and *Copy on the system core* at C + NC R/A or C + NC All, and is not used with *Low kernel memory*.

## How much faster

Measured with the tester (quick profile, every byte checked) on a PCH-1000, firmware 3.65, with a microSD in an SD2Vita, default settings. Higher is better.

| Test | Unit | Without plugin | vita-memio 1.6.0 |
|---|---|---:|---:|
| Reading a big file in 1 MiB pieces | MiB/s | 14.9 | 28.7 |
| Reading a big file in 128 KiB pieces | MiB/s | 14.3 | 32.9 |
| Random reads of 4 KiB | per second | 379 | 444 |
| Random reads of 32 KiB | per second | 361 | 430 |
| Random reads of 128 KiB | per second | 114 | 144 |
| Graphics data, 1 MiB pieces | MiB/s | 21.0 | 29.9 |
| Graphics data, 128 KiB pieces | MiB/s | 19.7 | 48.0 |
| Reading many small files | files/s | 207 | 298 |
| Creating many small files | files/s | 46 | 116 |
| Writing a big file in 1 MiB pieces | MiB/s | 14.1 | 15.8 |

The *Without plugin* figures are from an earlier run on the same card. Your card will give other numbers, especially for writes, which also change from run to run as the card does its own housekeeping.

In games the gain depends on how the game loads. Below are the plugin's own estimates from its log (one short session per game, with loading screens, measured with 1.5.0; rough figures, not stopwatch times): how long the game still waited for the card, and how much waiting read ahead saved on top of that. 85% means the game waited 85% less than it would have without read ahead.

| Game | Still waited | Saved by read ahead |
|---|---:|---:|
| PCSG01179 | 9.4 s | 52.8 s (85%) |
| GTA San Andreas (port) | 15.1 s | 22.5 s (60%) |
| PCSB00404 | 2.8 s | 12.4 s (81%) |
| NFS Underground 2 (port) | 11.7 s | 12.0 s (51%) |
| Hatsune Miku: Project DIVA f (PCSB00419) | 3.0 s | 2.0 s (40%) |
| Hatsune Miku: Project DIVA F 2nd (PCSB00554) | 4.7 s | 1.5 s (24%) |
| Persona 4 Golden (PCSB00245) | 2.1 s | 0.7 s (24%) |
| Gravity Rush (PCSF00024) | 32.1 s | 0.8 s (2%) |

Games that read their files in many tiny pieces gain the most. Gravity Rush jumps around its files, so there is little to read ahead. A loading screen gets shorter by less than these figures, because most loading is not limited by the card.

## Overlay and HUD

In any game, **SELECT + Right** opens the overlay; pressed again, it closes it and shows the HUD. **SELECT + Left** goes back; **SELECT + Up/Down** closes everything. In the overlay, Up/Down choose a line, Left/Right or Cross change it. While it is open, the game gets no buttons.

The overlay sets things **for this game only**. Each line says *Global* (as in Settings) until you change it: white is standard, blue changed but not saved, green saved. **Save** keeps them for next time. *Show advanced* adds Overlapped reads and GPU-safe reads.

- **Tuning** (*Auto* by default) measures how fast your card and console are, so Adaptive knows when reading ahead pays off. The figures are kept in `ux0:data/vita-memio/tuning.bin`. Triangle, twice, forgets them and measures again.

```
SD2VITA R: 21.8 W:  0.0 MB/s
R/W: 909/1 MiB On Err: 0
R/A: Adaptive  TitleID: PCSF00024
C:5:5/16/3 NC:3:2/16/3 Auto
Core 3 B C: 0%  NC: 0%  DMA: 98%
```

- **R / W:** read and write speed now; **R/W:** how much was read and written while the HUD was shown.
- **C / NC:** read ahead for normal memory (C) and graphics memory (NC); `-` means Adaptive switched it off there. **Auto** is green while Tuning measures.
- **Core 3 C / NC:** how much of the copying the spare core did. **DMA:** how much the DMA chip did. With *DMA copy* on, DMA near 100% and Core 3 near 0% is normal.

## Tester

`vitamemio_tester.vpk` measures your own card: reads and writes of several sizes, many small files, and reads the way games do them, every byte checked. Each result is added to `ur0:data/vita-memio/results.txt`. It never changes the plugin's settings. For copy timings, switch on *Statistics* first.

## Error C1-2569-2

The console is out of kernel memory (it can also show as a crash right after switching the plugin on). Close other apps, switch on *Low kernel memory* or use only one storage, switch off *Read ahead*, *Overlapped reads*, *Copy on the system core* and *Overlay*, restart, and remove other kernel plugins if needed.

## Good to know

- Only the storages you switch on are touched; never the internal storage, game cards, USB storage or Wi-Fi.
- Memory: 256 KiB per storage (with *Low kernel memory*, one set for both), 128 KiB more with *Overlapped reads* or *Copy on the system core*, about 40 KiB with *Read ahead* and 40 KiB with *Overlay*. With only one storage on, reading again from memory takes 384 KiB more once a game has used it.
- While the console starts, everything goes the normal way; the plugin comes on once the home screen is up. If the console restarts before the plugin has shown it works, the next start loads nothing, so a bad setting cannot cause a reboot loop. Holding **L** at start skips all plugins.
- On the internal storage it keeps only its settings, the boot guard and the log; everything else goes to `ux0:data/vita-memio/`.
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