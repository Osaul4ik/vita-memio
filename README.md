# vita-memio

<p align="center"><img src="assets/logo.png" alt="vita-memio: faster storage for PS Vita" width="720"></p>

**Faster storage for your PS Vita.** vita-memio makes a microSD card in an **SD2Vita** adapter and the official **Sony memory card** faster. Games load quicker, and copying files and installing VPKs take less time. Your files stay exactly as they are: nothing is converted, nothing is stored in a new way.

You install it once, switch on your storage in Settings, and that's it. Everything else is already set the way that works best.

- **What you need:** a PS Vita (or PS TV) with **HENkaku / Ensō** (taiHEN) on firmware **3.60–3.74**. It is tested on a PCH-1000 with firmware 3.65.
- **What it speeds up:** an SD2Vita and the Sony memory card. It never touches the internal storage, game cards, USB storage or Wi-Fi.
- **Not together with Vita Turbo Storage.** vita-memio is a fork of [Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage) by [Juan Garcia Alvite](https://github.com/juanjsebgarcia). Never use both at the same time: remove Vita Turbo Storage first.

## Is it safe?

- **Games get exactly the same data** as without the plugin. It only changes *how* the data is moved, not *what* is moved.
- **Writes are never held back.** When a game or VitaShell says a file is saved, it really is on the card.
- **If anything goes wrong,** that storage simply goes back to the normal (slower) way until the next restart.
- **No boot loops.** If the console ever restarts before the plugin has shown that it works, the next start loads nothing (*safe mode*). You can also hold **L** while the console starts to skip all plugins.

Still, as with any plugin that touches storage: **back up your memory card and SD card first.**

## Install

You need a file manager such as [VitaShell](https://github.com/TheOfficialFloW/VitaShell) to copy files and edit a text file on the Vita.

1. Download `vita-memio-<version>.zip` from the [latest release](https://github.com/Osaul4ik/vita-memio/releases/latest) and unpack it on your PC.
2. Copy these **three** files to the folder **`ur0:tai/`** on the Vita:
   - `vitamemio_kernel.skprx`
   - `vitamemio_hud.skprx`
   - `vitamemio.suprx`
3. Open your taiHEN config file in VitaShell (select it and press Cross to edit it). It is **`ux0:tai/config.txt`** if that file exists, otherwise **`ur0:tai/config.txt`**. Add these lines:

   ```
   *KERNEL
   ur0:tai/vitamemio_kernel.skprx

   *NPXS10015
   ur0:tai/vitamemio.suprx
   ```

   If your file already has a `*KERNEL` line, put `ur0:tai/vitamemio_kernel.skprx` under the existing one instead of adding a second `*KERNEL`. The same goes for `*NPXS10015`. Do **not** add `vitamemio_hud.skprx`: the plugin loads it by itself.
4. Restart the console.
5. Open **Settings → Devices → vita-memio** and switch on the storage you use:
   - **Faster SD2Vita** if your games are on a microSD in an SD2Vita;
   - **Faster Vita memory card** if they are on the Sony memory card.

Done. You don't need to change anything else.

**How do I know it works?** The row under the title says **Driver: active**. Press it to see the version, the status and how much memory the plugin uses. In a game, **SELECT + Right** opens the plugin's overlay (see [Overlay and HUD](#overlay-and-hud)).

The zip also contains `vitamemio_tester.vpk`, an optional app that measures your card (see [Tester](#tester)), and `SHA256SUMS` to check the downloaded files.

## Update

1. Open **Settings → Devices → vita-memio** and switch **Driver** off.
2. Restart the console.
3. Replace the three files in `ur0:tai/` with the new ones. **Always replace all three together.**
4. Delete the folders `ur0:data/vita-memio/` and `ux0:data/vita-memio/`.
5. Restart the console.
6. Open **Settings → Devices → vita-memio** again and switch your storage on.

Deleting the folders resets everything to the defaults: the settings, the per-game overlay settings and the card measurements. The plugin measures your card again by itself while you play. If you forget this step, nothing breaks: files from another version are simply deleted and the defaults are used.

## Uninstall

1. Switch **Driver** off and restart.
2. Remove the plugin's lines from `config.txt`.
3. Delete the three files from `ur0:tai/` and the folders `ur0:data/vita-memio/` and `ux0:data/vita-memio/`.
4. Restart.

## Problems

**Error C1-2569-2, or a crash right after switching the plugin on.** The console is out of kernel memory, usually because of many kernel plugins. Try these, one after another:
1. Set **Kernel memory** to **Low** and restart.
2. Switch off **Overlay**, **Read ahead** and **Overlapped reads**.
3. Remove kernel plugins you don't need.

**The status says "Driver: safe mode".** The console restarted (or crashed) last time before the plugin had shown that it works, so this start loaded nothing. Switch off whatever you changed last, press **Reset safe mode** and restart.

**The status says "Driver: not loaded".** `vitamemio_kernel.skprx` is not running: check that it is in `ur0:tai/` and listed under `*KERNEL` in the right `config.txt` (step 3 of [Install](#install)), then restart.

**The vita-memio page is missing from Settings.** `vitamemio.suprx` is not loaded: check the `*NPXS10015` lines in `config.txt`.

**A game shows broken graphics.** Set **GPU-safe reads** to **All**. If that does not help, switch the storage off and [open an issue](https://github.com/Osaul4ik/vita-memio/issues).

**Something else.** Switch on **Enable log**, reproduce the problem and attach `ur0:data/vita-memio/kernel.log` to an [issue](https://github.com/Osaul4ik/vita-memio/issues).

## How much faster

Measured with the tester (quick profile, every byte checked) on a PCH-1000, firmware 3.65, microSD in an SD2Vita, default settings. Higher is better.

| Test | Unit | Without plugin | vita-memio 1.6.0 |
|---|---|---:|---:|
| Reading a big file in 1 MiB pieces | MiB/s | 14.9 | 28.9 |
| Reading a big file in 128 KiB pieces | MiB/s | 14.3 | 33.0 |
| Random reads of 4 KiB | per second | 379 | 438 |
| Random reads of 32 KiB | per second | 361 | 425 |
| Random reads of 128 KiB | per second | 114 | 143 |
| Graphics data, 1 MiB pieces | MiB/s | 21.0 | 30.2 |
| Graphics data, 128 KiB pieces | MiB/s | 19.7 | 49.4 |
| Reading many small files | files/s | 207 | 321 |
| Creating many small files | files/s | 46 | 113 |
| Writing a big file in 1 MiB pieces | MiB/s | 14.1 | 13.6–17.3 |

The *Without plugin* figures are from an earlier run on the same card. Your card will give other numbers, especially for writes, which also change from run to run as the card does its own housekeeping.

**In games** the gain depends on how the game reads its files. The figures below are **not loading-screen times.** They come from the plugin's own log and add up *all* the time a game spent waiting for the card during one play session: every read, in loading screens and while you play (a game streams textures, sounds and map data the whole time).

- **Play time:** how long the session was.
- **Waited for the card:** all reads of the session together, with vita-memio.
- **Saved:** how much longer those same reads would have taken without *Read ahead* and *reading again from memory*. The plugin knows this because it measures, for each kind of read, how long it takes from the card. The basic speed-up of the faster path comes on top and is not counted here, so the real gain over playing without vita-memio is even a little bigger.

Example: in 87 seconds of Catherine, the game waited 7 seconds in total for the card; without read ahead the same reads would have taken about 53 seconds. That does not mean one loading screen got 46 seconds shorter: the time is spread over tens of thousands of small reads, so you notice it as shorter loading, less stutter and fewer pauses while the game streams data.

| Game | Play time | Waited for the card | Saved | Waited less by |
|---|---:|---:|---:|---:|
| Catherine: Full Body (PCSG01179) | 87 s | 7.0 s | 46.2 s | 86% |
| GTA San Andreas (port) | 156 s | 16.1 s | 23.1 s | 58% |
| PCSB00404 | 131 s | 3.1 s | 16.3 s | 83% |
| GTA Vice City (port) | 114 s | 7.3 s | 5.2 s | 41% |
| PCSB01257 | 45 s | 1.4 s | 1.1 s | 43% |
| PCSB00867 | 103 s | 2.9 s | 2.1 s | 42% |
| Persona 4 Golden (PCSB00245) | 74 s | 1.7 s | 0.9 s | 33% |
| Hatsune Miku: Project DIVA F 2nd (PCSB00554) | 252 s | 5.4 s | 2.6 s | 32% |
| Gravity Rush (PCSF00024) | 103 s | 21.6 s | 1.0 s | 4% |

One session per game with vita-memio 1.6.0 on the console above; your figures will differ with your card and how you play. Games that read their files in many tiny pieces, such as Catherine and the GTA ports, gain the most: those pieces now come from memory in about 15 µs instead of about 0.5 ms from the card. Gravity Rush jumps around its files, so there is little to read ahead; it gets only the basic speed-up.

## Settings

Everything is in **Settings → Devices → vita-memio**. Changes work at once, except where the table says *after a restart*. **For normal use you only need the first two rows**: the rest is already set the way that works best.

| Setting | What it does | Default |
|---|---|---|
| Driver | Whether the plugin starts with the console. Off: from the next start it loads nothing, but this page stays so you can switch it back on. *After a restart* | On |
| Faster SD2Vita / memory card | Speeds up that storage. Switch on the one your games are on | Off |
| Kernel memory | How much memory the plugin keeps for itself: **Full**, **Medium** or **Low**. See [Kernel memory](#kernel-memory). *After a restart* | Medium |
| *Advanced* | | |
| GPU-safe reads | Makes sure the graphics chip sees the data a game loads. **Non-DMA** is faster; if a game ever shows broken graphics, choose **All** | Non-DMA |
| Overlapped reads | Reads big files in parts, so the card never waits for the copying. **Auto** tunes itself to your card | Auto |
| Copy on the system core | Lets the console's spare core help with copying. With *DMA copy* on, the DMA chip does almost all of it anyway. Keep **C + NC R/A**: *Off* and *Cached only* also switch off [reading again from memory](#reading-again-from-memory) | C + NC R/A |
| DMA copy | The console's DMA chip copies the data, about twice as fast as the processor. Switches itself off if anything goes wrong | On |
| Read ahead | Reads the next part of a file before the game asks for it: **Adaptive**, **On** or **Off**. See [Read ahead](#read-ahead) | Adaptive |
| Small transfers too | Also speeds up transfers under 32 KiB, such as opening folders and small files | On |
| Overlay (SELECT + arrows) | A menu and an information panel over any game. Off works at once; On *after a restart* if it was off when the console started | On |
| Profiles for system apps | The home screen and the system's apps get their own settings and learning, like games | On |
| *Developer settings* | | |
| Verify transfers | Reads every transfer a second time the normal way and compares. Slow, for checking only | Off |
| Enable log | Writes `ur0:data/vita-memio/kernel.log`, with a summary of each game's reads when you leave it. Useful for bug reports | Off |
| Statistics | Collects timings for the tester app | Off |

**The status row** (*Driver: active* and so on): press it to see the version, the status, the kernel memory the plugin uses and how much is still free (measured the moment you press it), and the storage driver.

**Reset safe mode** appears only when the plugin is in safe mode (see [Problems](#problems)).

### Kernel memory

The plugin keeps a little of the console's kernel memory for its buffers. You can choose how much:

- **Medium** (640 KiB) — **the default, best for most people.** The SD2Vita and the memory card share one set of buffers. A game only reads from one card at a time, so in games it is as fast as *Full*.
- **Full** (768 KiB) — each storage gets its own buffers. A little faster only when you copy between the SD2Vita and the memory card (for example in VitaShell).
- **Low** (512 KiB) — like *Medium*, but [reading again from memory](#reading-again-from-memory) keeps less, so it helps a bit less. Choose it if you get error C1-2569-2 or other plugins run out of memory.

The change works after a restart.

### Read ahead

A memory card is slowest when it has to wait for the next request. While a game reads a file in order, vita-memio reads the next part early, so it is already in memory when the game asks for it. Data read early is thrown away as soon as anything writes over it, so a game never gets old data. Read ahead is used for the game you are playing; other apps (the system, music in the background) are still sped up, just without it.

- **Adaptive** (the default) learns while you play when reading ahead saves time in this game, and switches it off where it doesn't. It starts learning afresh at every launch; if you go to the home screen and come back, it goes on where it was.
- **On** always reads ahead after a few reads in a row. **Off** never does.

### Reading again from memory

Many commercial games read the same graphics data again a moment later. vita-memio keeps the latest such reads in memory and gives them to the game from there instead of reading the card again. There is nothing to set: it works for commercial games (title IDs starting with PCS), not for homebrew, Adrenaline or the system's apps, and switches itself off in a game where it does not pay off. It needs *Read ahead* (Adaptive or On) and *Copy on the system core* at C + NC R/A.

## Overlay and HUD

In any game:
- **SELECT + Right** opens the overlay. Pressed again, it closes the overlay and shows the HUD (a small information panel).
- **SELECT + Left** goes back. **SELECT + Up/Down** closes everything.
- In the overlay, **Up/Down** choose a line, **Left/Right** or **Cross** change it. While it is open, the game gets no buttons.

The overlay changes settings **for this game only**. Each line says *Global* (as in Settings) until you change it: white is the standard value, blue changed but not saved, green saved. **Save** keeps them for the next time you play this game. *Show advanced* adds more lines.

- **Tuning** (*Auto* by default) measures how fast your card and console are, so *Adaptive* knows when reading ahead pays off. The results are kept in `ux0:data/vita-memio/tuning.bin`. Press Triangle twice on this line to forget them and measure again.

What the HUD shows:

```
SD2VITA R: 21.8 W:  0.0 MB/s
R/W: 909/1 MiB On Err: 0
R/A: Adaptive  TitleID: PCSF00024
C:5:5/16/3 NC:3:2/16/3 Auto
Core 3 B C: 0%  NC: 0%  DMA: 98%
```

- **R / W:** read and write speed right now. **R/W:** how much was read and written while the HUD was shown. **Err:** errors (should be 0).
- **C / NC:** read ahead for normal memory (C) and graphics memory (NC); `-` means Adaptive switched it off there. **Auto** is green while Tuning measures.
- **Core 3 C / NC:** how much of the copying the spare core did. **DMA:** how much the DMA chip did. DMA near 100% and Core 3 near 0% is normal.

## Tester

`vitamemio_tester.vpk` is an optional app that measures your own card: reads and writes of several sizes, many small files, and reads the way games do them, with every byte checked. Install it like any VPK. Each result is added to `ur0:data/vita-memio/results.txt`, so you can compare runs. It never changes the plugin's settings. For copy timings, switch on *Statistics* first.

## Good to know

- **Memory:** 768, 640 or 512 KiB for the buffers (see [Kernel memory](#kernel-memory)), plus about 40 KiB with *Read ahead* and about 40 KiB with the *Overlay*. It is set aside when the console starts.
- **Starting up:** while the console starts, everything goes the normal way; the plugin comes on once the home screen is up.
- **Restart and power off:** before the console restarts (from VitaShell, HENkaku or the system) or switches off, the plugin puts every storage back the normal way, lets its reads finish and saves its files, so the cards are shut down cleanly.
- **Files:** on the internal storage it keeps only its settings, the boot guard and the log (when on); everything else goes to `ux0:data/vita-memio/`. Files from another version are not converted: they are deleted and the defaults are used.
- **Consoles:** tested on one PCH-1000 with firmware 3.65. PCH-2000 and PS TV use the same storage system but are untested.

## Disclaimer

This software is provided **"as is", without warranty of any kind**, and you use it at your own risk. **Back up your memory card and SD card before installing.** See the [MIT License](LICENSE).

## For developers

```sh
make build        # the modules and the tester, in the pinned VitaSDK Docker image
make test         # host tests (no VitaSDK needed)
make config       # prints the config.txt lines
```

GitHub Actions builds every push. *Run workflow* on the **build** workflow has a **Session statistics in the log** switch: off leaves the log's `session …` lines out, for a smaller kernel module and less counting on every transfer; such a build's version ends in `-nodiag`. Locally: `make build VM_DIAG=OFF`. See [CONTRIBUTING.md](CONTRIBUTING.md) for more.

## Credits

All credit for the original project goes to **[Juan Garcia Alvite](https://github.com/juanjsebgarcia)**, the author of [Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage): the technique, the research, the original implementation, its testing and documentation.

- **Built on:** [taiHEN](https://github.com/yifanlu/taiHEN) by Yifan Lu; [HENkaku](https://github.com/henkaku/henkaku) and [Ensō](https://github.com/TheOfficialFloW/enso) by the HENkaku team and TheFloW (the Settings page is added the way HENkaku's own Settings plugin does it, [`plugin/user.c`](https://github.com/henkaku/henkaku/blob/master/plugin/user.c)); [VitaSDK](https://vitasdk.org) and its [headers](https://github.com/vitasdk/vita-headers).
- **Ideas and references:** [IOStaging](https://github.com/bythos14) by bythos14 (the staging-buffer idea the original project credits); [YAMT](https://github.com/SKGleba/yamt-vita) by SKGleba (the layout names of the Devices page); [PSVshell](https://github.com/Electry/PSVshell) by Electry (the look of the tester and the HUD). No YAMT or PSVshell code is used.
- **Included:** a subset of [Terminus Font](https://terminus-font.sourceforge.net/) by Dimitar Toshkov Zhekov, under the SIL Open Font License 1.1 (`include/OFL-Terminus.txt`, also inside the VPK).
- **Artwork:** the logo, the tester's icon and LiveArea are original, drawn by `tools/gen_art.py`; their lettering is set in [Inter](https://rsms.me/inter/) by Rasmus Andersson (SIL Open Font License 1.1).
- **Tools:** [vitacompanion](https://github.com/devnoname120/vitacompanion) by devnoname120, [VitaShell](https://github.com/TheOfficialFloW/VitaShell) by TheFloW, [cppcheck](https://cppcheck.sourceforge.io/), [QEMU](https://www.qemu.org/) and GCC's sanitizers.

## License

MIT, as the original project: see [LICENSE](LICENSE) and the [original license](https://github.com/juanjsebgarcia/vita-turbo-storage/blob/main/LICENSE). The Terminus Font subset in `include/vm_font_terminus.h` stays under the SIL Open Font License 1.1.
