# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

VISSS (Video In Situ Snowfall Sensor) data acquisition software. The core deliverable is a
C++ program that captures frames from a GigE Vision camera in real time, detects motion, and
writes them to encoded video + metadata files. Everything else in the repo (Python GUI/launcher,
systemd units, sync/logging scripts) exists to run that binary reliably, unattended, on
field-deployed instrument PCs.

Related repos (not in this checkout):
- `VISSS_configuration` — per-deployment YAML/camera-config files consumed by the launcher.
- `VISSSlib` (https://github.com/maahn/VISSSlib) — downstream Python data processing.

## Build

The C++ program lives in `data-acquisition/` and **requires the Teledyne DALSA "GigE-V
Framework" SDK** to be installed at `/usr/dalsa/GigeV` (see `install_commands_bookworm.txt`),
plus a GenICam runtime referenced via the `GENICAM_ROOT_V3_0` env var (set by the SDK
installer, hence "if make fails, try `source /etc/profile`"). It will **not** compile on a
machine without that SDK (e.g. this cannot be built/tested on macOS or a plain Linux dev box).

```bash
cd data-acquisition
make            # builds visss-data-acquisition and visss-data-acquisition-dryrun
make clean      # removes obj/*
```

The build links against system OpenCV4 (`pkg-config opencv4`, no CUDA), Boost, libpcap, ffmpeg
libs, and the DALSA `GevApi`/GenICam libraries. After linking, `make` runs
`sudo setcap cap_sys_nice,cap_ipc_lock,cap_net_raw+eip` on the resulting `visss-data-acquisition`
binary — real-time scheduling and raw-socket access are required at runtime, not just root.

There is no automated test suite. `visss-data-acquisition-dryrun` (reads a pre-recorded video
file + companion CSV of timestamps/frame IDs instead of a live camera) is the closest thing to
a test harness — it exercises the storage/encoding/motion-detection pipeline without hardware.
`scripts/testffmpeg.py` is a small manual ffmpeg-parameter probe, not a test.

## Running / debugging the binary directly

`visss-data-acquisition` takes two positional args (camera config file, camera serial) plus
flags; see `params` in `data-acquisition/src/visss-data-acquisition.cpp` for the authoritative
list (`-o` output path, `-t` storage threads, `-e` ffmpeg encoding opts with `@` standing in for
spaces, `--cpu*` thread-affinity pinning, `--resetDHCP`, etc.). In normal operation nothing
invokes it directly — see the launcher chain below.

## Architecture

### Two-machine PTP-synced deployment

A VISSS instrument is normally two PCs, a "leader" and a "follower" camera, kept in sync via
PTP (`ptp4l`) and `phc2sys` (see `scripts/services/visss_ptp_{master,slave}@.service`,
`visss_sync_*@.service`, `scripts/VISSS_INTERFACES.visss3.template`). The C++ binary reads PTP
sync status from the camera itself (`ptpStatus` GenICam feature) and refuses to proceed
(`--noptp=0`, the default) until the camera reports "Slave". This dependency on external PTP
daemons plus camera-side PTP is why clock/timestamp bugs need to be reasoned about across both
the systemd PTP chain and the C++ capture loop, not just the C++ side.

### C++ capture pipeline (`data-acquisition/src/`)

- `visss-data-acquisition.h` — shared state and utilities included by every other C++ file:
  camera/GenICam feature globals, `MatMeta` (the per-frame struct that flows through the
  pipeline), timestamp formatting, `PrintThread` (mutex-guarded `std::cout` wrapper), CLI-parsed
  globals (`storeVideo`, `nStorageThreads`, `encoding`, ...). Deliberately uses free-standing
  global state rather than a config object/DI — that's the established pattern here, not
  something to "fix" in isolation.
- `frame_queue.h` — bounded thread-safe queue (`MatMeta` producer/consumer) between capture and
  storage. `max_queue_size` silently drops frames (sets `global_error`) rather than blocking the
  capture thread.
- `visss-data-acquisition.cpp` — `main()` (CLI parsing, camera open/config, PTP wait loop) and
  `ImageCaptureThread` (the GigE Vision capture loop). Spawns `nStorageThreads` instances of
  `storage_worker_cv` reading from one or more `frame_queue`s.
- `storage_worker_cv.h` — one instance per storage thread. Per frame: motion detection via
  cumulative histogram of the frame-to-frame absdiff, timestamp/status text overlay, pipes raw
  frame bytes to an `ffmpeg` subprocess via `popen()` for encoding (not `cv::VideoWriter` —
  deliberate, for better thread control per the README changelog), writes a companion `.txt`
  metadata file, and periodically shows a live preview window. New file is opened every
  `new_file_interval` seconds; on rollover the previous file is renamed from a `tmp/` staging
  path to its final path and a `*_latest` symlink is updated via `create_symlink()`.
- `visss-data-acquisition-dryrun.cpp` — alternate `main()`/capture thread that reads frames from
  a video file + CSV instead of a camera, but reuses `frame_queue`/`storage_worker_cv` unchanged.
  Any fix made in the shared headers automatically applies to both binaries; logic specific to
  `main()` (e.g. signal handling) has to be duplicated deliberately in both `.cpp` files.

Threads run with explicit real-time scheduling (`SCHED_RR`) and optional CPU-affinity pinning
(`--cpuserver`, `--cpustream`, `--cpustorage`, `--cpuffmpeg`, `--cpuother`) — this is a
latency-sensitive real-time capture path, not just a batch job, so avoid adding unbounded-latency
operations (blocking I/O, locks held across syscalls) on the capture thread or inside the
per-frame storage loop.

### Python launcher/GUI (`data-acquisition/launch_visss_data_acquisition.py` + `.sh`)

`launch_visss_data_acquisition.py` is a Tkinter GUI (`runCpp` class per configured camera) that
turns a YAML camera config (from the sibling `VISSS_configuration` repo) into
`--UPPERCASE=value` args for `launch_visss_data_acquisition.sh`, which translates those into the
binary's actual short flags and then **loops forever, restarting the binary 5s after any
non-zero exit**. Consequences worth knowing:
- Any crash is self-healing at the process level, which is why crash *frequency*/root cause
  matters more than any single crash being fatal.
- Every (re)start resets the C++ side's frame-ID tracking, so a "missed frames between 0 and N"
  log line right after a (re)start is very likely a false positive (the camera's own frame
  counter doesn't reset just because the software restarted), not real dropped frames.
- The GUI also handles NIC MTU/IRQ tuning (`scripts/gev_nettweak`, CPU IRQ pinning) before
  launching, and logs the C++ subprocess's stdout through Python's `logging` module (lines are
  classified by their `ERROR|`/`FATAL|`/`INFO|`/`DEBUG|`/`STATUS|` prefix — keep emitting that
  prefix convention from C++ if you add new log lines that should route to the right level).

### Other components (independent of the camera pipeline)

- `sonic/` — serial datalogger + gzip cron job for a separate "Sonic" sensor; unrelated to the
  camera/GenICam code path.
- `sync/rsync.py` — simple recent-days rsync-style archival script.
- `scripts/services/*` — systemd units for PTP, NIC tuning, and periodic link-speed checks.
- `install_commands_*.{txt,md}` — per-OS-version deployment runbooks (Ubuntu 18.04/20.04, Debian
  Bullseye/Bookworm); these are the source of truth for the exact package/SDK setup, not just
  historical notes.

## Gotchas specific to this codebase

- Do not assume a change compiles just because it looks correct — there is no CI here and the
  DALSA SDK dependency means most dev environments (including this Claude Code sandbox) cannot
  build the project at all. State that limitation explicitly rather than claiming a build was
  verified when it was not.
- `storage_worker_cv.h`/`frame_queue.h`/`visss-data-acquisition.h` are shared between the live
  binary and the dryrun binary — a fix in one of those headers fixes both; logic inside `main()`
  in the two `.cpp` files is not shared and needs the same fix applied twice if relevant to both
  (e.g. signal handling).
- Several globals (`cameraTemperature`, `ptp_status`, `transferQueueCurrentBlockCount`,
  `transferMaxBlockSize`) are written by the capture thread and read by every storage thread;
  they're guarded by `cameraStatusMutex` — keep new cross-thread shared state guarded the same
  way rather than adding more bare globals.
- The on-disk metadata file format is versioned (`# VISSS file format version: X.Y` header
  written in `storage_worker_cv::add_meta_data`) and consumed downstream by `VISSSlib` — treat
  changes to the metadata layout as a compatibility-breaking change worth bumping that version
  for, and check the README's version history for the existing convention.
