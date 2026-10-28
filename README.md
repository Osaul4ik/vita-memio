# vita-memio

**A fork of [Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage)**

[Original project and full documentation](https://github.com/juanjsebgarcia/vita-turbo-storage) · [Original author's profile](https://github.com/juanjsebgarcia)

</div>

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



## Credits

All credit for the original project goes to **[Juan Garcia Alvite](https://github.com/juanjsebgarcia)**, the author of [Vita Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage), including the original implementation, technical approach, testing and documentation.

The original project builds on [taiHEN](https://github.com/henkaku/taiHEN), [VitaSDK](https://vitasdk.org), and [vitacompanion](https://github.com/devnoname120/vitacompanion). The original author also credits [bythos14](https://github.com/bythos14) for the inspiration from IOStaging.

Please consult the upstream repository for the original project's license and full license terms: [MIT License](https://github.com/juanjsebgarcia/vita-turbo-storage/blob/main/LICENSE).
