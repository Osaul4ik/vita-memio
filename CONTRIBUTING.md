# Contributing

Thanks for helping make PS Vita storage faster. Bug reports, measurements from
your own console and SD card, translations and code are all welcome.

## Ground rules

This plugin sits under every file on the console, so a few rules are
non-negotiable:

- **Never corrupt data.** The faster path must produce exactly what the standard
  path would. Any new path ships with verify-mode coverage (`kernel/stage.c`) and
  is tested with verify mode on before it is trusted.
- **Leave the driver alone.** Turbo Storage only changes the buffer handed to the
  system's own exported read/write functions. Don't re-implement, patch or reorder
  driver internals.
- **Never cache or defer writes.** A write must have reached the card before the
  call returns, as it does without the plugin.
- **Fail to standard.** Any error or doubt switches that storage back to the
  standard path and finishes the request the standard way. Unknown firmware means
  nothing is installed.
- **Keep hooks trivial.** No logging, file I/O or sleeping inside the I/O hooks;
  counters only. The 1 Hz worker logs.

## Building

```sh
make            # VitaSDK if $VITASDK is set, otherwise the pinned Docker image
make clean
```

Fix any new warnings in the code you touch. The build must stay clean.

## Testing on a Vita

You need HENkaku/Ensō on firmware 3.60–3.74 and
[vitacompanion](https://github.com/devnoname120/vitacompanion), which provides FTP
on :1337 and commands on :1338.

Turn on *Developer mode* in *Settings → System → Turbo Storage* so the plugin's
port 1340 is open.

```sh
make deploy VITA_IP=<vita>                 # upload to ur0:tai/ and reboot
make log    VITA_IP=<vita>                 # fetch the kernel and user logs
printf 'status\n' | nc -w 3 <vita> 1340    # live status and counters
printf 'set verify 1\n' | nc -w 3 <vita> 1340
```

**Test with verify mode on.** It re-reads every faster transfer the standard way
and compares; `status` shows `verified` and `mismatches` per storage. Any
mismatch is a bug: please report it.

A simple end-to-end check: download a large file from the Vita over FTP with
Turbo Storage off, again with it on, and compare the two copies.

If a change misbehaves, **hold L while the Vita boots** to skip all plugins, then
fix `ur0:tai/config.txt` or the module over FTP.

When you report a result, include your console model, firmware, storage manager
(YAMT, StorageMgr...), the SD card's size and speed class, the `status` output and
the relevant log lines.

## Translations

The Settings page is shown in all 20 Vita system languages. Only English is
written by a person; the rest are machine translations, and corrections from
native speakers are very welcome. The strings live in `tools/gen_i18n.py`: edit
them there, then run `python3 tools/gen_i18n.py` to regenerate
`settings_app/storageturbo_i18n_table.h`. Keep "Turbo Storage" and "SD2Vita"
untranslated. The HUD's words are in `kernel/hud_i18n.c` (plain ASCII, accents
dropped, Latin-script languages only).

## Pull requests

- One logical change per PR, with a clear description of what changed and how you
  tested it (on hardware, with verify mode, if it touches `kernel/stage.c`).
- Commit messages in the existing style: `storageturbo: <what changed>`,
  `docs: <what changed>`.
- By contributing you agree that your contribution is licensed under the MIT
  licence of this project.
