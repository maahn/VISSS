# Handover — VISSS → Emergent Vision / eSDK Pro port

Written 2026-08-07 for session continuity. Read this before touching `data-acquisition-emergent/`.

## What this is

Porting VISSS's C++ data acquisition pipeline off the Teledyne DALSA GigE-V SDK (`data-acquisition/`,
reference-only, do not edit) onto Emergent Vision cameras via eSDK Pro (`data-acquisition-emergent/`,
where all new work goes). Read these three docs in `data-acquisition-emergent/` first if picking this
up cold:

- `PROCESSING_SPEC_teeldyne.md` — SDK-agnostic spec of what the old pipeline does, stage by stage.
- `new-sdk-reference.md` — eSDK Pro capability map.
- `pipeline-to-esdkpro-mapping.md` — old→new stage mapping + gap list. Some gaps are now resolved
  (see below); the doc itself hasn't been updated to reflect that.

## Environment (this box)

- Ubuntu 22.04.5, eCapturePro 1.6.1 (eSDK 4.07.01/02), CUDA 12.9, NVIDIA driver 595.84.
- eSDK Pro **is actually installed** at `/opt/EVT` — unlike the old Teledyne SDK, this project *can*
  be built and run here, including against real cameras.
- 2× Emergent HR-2000SM cameras (serials `2016987`, `2016988`), 1624×1304 mono8 @ **485fps**. Both
  on *one* PC via a dual-port Mellanox ConnectX-6 Lx (`enp129s0f0np0`/`enp129s0f1np1`) — this is the
  "combined leader+follower" topology from `install_commands_bullseye.md` §3a, not the production
  two-PC split.
- `EMERGENT_DIR=/opt/EVT`, `ECAPTURE_PRO_DIR=/opt/EVT/eCapturePro` needed to build; non-login shells
  (including my Bash tool) don't source `/etc/profile.d/`, so `build_and_install.sh` defaults them.
- **I cannot run `sudo`** — no passwordless sudo for this shell. Every install/systemctl step gets
  handed to the user as an explicit command.

## Build & test

```bash
cd /home/visss/VISSS/data-acquisition-emergent
./build_and_install.sh              # builds all 3 projects, installs plugins, restarts the server (sudo)
./build_and_install.sh --no-install  # build-only, what I can run myself
./build_and_install.sh --clean       # wipe build/ dirs first
```

```bash
cd visss-data-acquisition-EVT
./build/visss-data-acquisition-EVT -s 127.0.0.01 $PWD -i 20 -b 15000 -e p1
```

CLI flags: `-s <ip> <path>` (repeatable), `-t <sec>` (optional, default = run until Ctrl+C/SIGTERM),
`-b <kbps>`, `-i <sec>` (rollover interval, default 600), `-n <name>` (default VISSS), `-e <preset>`
(nvenc preset, default `p1`).

## Architecture

```
CameraTask → MotionDetectTask (motion_detect/) → RecordTask (record/)
```

Both `MotionDetectTask` and `RecordTask` are custom `PluginTask`s, built as **separate `.so` files**
loaded by the `eCaptureProServer` daemon (`/opt/EVT/eCapturePro/eSdkPro/plugins/`) — **not** compiled
into the client. This is required, not a style choice: `main.cpp` always calls
`System::ConnectServer(ip)`, even for `127.0.0.1` — that's the "remote server" model in eSDK Pro's
terms regardless of actual IP, and only `.so`-loaded plugins are visible to it. (`plugin_local`'s
in-process pattern doesn't apply here.)

### `motion_detect/` — `MotionDetectTask`

- Extends each frame with a 64px black border, draws status-bar text (`site | timestamp | name | ID`)
  via a custom CUDA bitmap-font kernel (`font8x14.h`, generated with a throwaway Python/PIL script —
  *not* hand-transcribed, to avoid glyph-correctness risk).
- **Border color is currently unconditionally black.** Old pipeline varies it (black/gray) by a
  moving-pixel flag from motion detection — not implemented yet, see "Not yet implemented" below.

### `record/` — `RecordTask`

Replaces eSDK Pro's built-in `NvencTask` entirely. **Why**: `NvencTask`'s native segmented-recording
rotates the video file but not its `recordingTimestamps.txt`/`recordingMetadata.json` sidecars — those
are constructed once per whole session (confirmed via symbols in `libeSdkPro.so`: `NvencTaskWorker`
has one constructor call, not one per segment). VISSS needs frame-exact video/timestamp sync, so we
encode ourselves via `libavcodec`'s `hevc_nvenc` + `libavformat` mux, writing both in the same
`Process()` call — sync-by-construction.

Key design points (all verified against the real installed SDK/ffmpeg, not assumed):

- **Encoder session opened once in `Init()`**, kept alive for the task's whole lifetime.
  `avcodec_open2()` (NVENC session creation) is genuinely slow (100ms+); at 485fps that's enough time
  to drop 50-100 frames if done lazily on the first `Process()` call. This required `Width`/`Height`
  becoming task params (client-supplied) instead of lazily inferred from the first frame.
- **Rollover swaps the muxer, not the encoder.** `hevc_nvenc` declares
  `AV_CODEC_CAP_ENCODER_FLUSH` — confirmed by writing a tiny standalone test program against the
  installed `libavcodec`. So a rollover: drains pending packets into the closing file →
  `avcodec_flush_buffers()` → opens a fresh muxer for the next file. No NVENC session reopen, no
  dropped frames at rollover boundaries.
- **`forced-idr=1` AVOption is required**, not just `pict_type=AV_PICTURE_TYPE_I` on the first frame
  of a new segment. Without it, every segment after the first rollover opened to a black/corrupt
  frame in players — HEVC allows non-IDR "keyframes" that still reference content before them, so the
  flushed-and-reused encoder needs to be told explicitly to emit a *true* IDR at segment boundaries.
- **`rc=cbr` must be set explicitly.** `-rc` defaults to "let the preset decide," and at `preset=p1`
  that wasn't CBR — measured output was ~2.7× over the requested bitrate until fixed.
- mono8→nv12 conversion needs **zero CUDA kernel code**: Y plane is a straight `cudaMemcpy2D`, U/V
  plane is `cudaMemset2D`'d to a constant 128 (neutral chroma). `hevc_nvenc` doesn't accept mono8/gray
  directly (verified against the encoder's actual supported-format list).
- First-frame `.jpg` snapshot per segment via `libavcodec`'s **`mjpeg`** encoder — needs
  `AV_PIX_FMT_YUVJ420P`, **not** `GRAY8` (`mjpeg` only supports `yuvj420p/422p/444p`, verified). Same
  Y=data/UV=128 trick, done once per rollover on the CPU (a small host round-trip here is fine — it's
  off the real-time per-frame path, unlike the main video encode).
- **NVENC preset is still being tuned** — this was the last thing in progress. `p1`: ~50% NVENC
  utilization, 0 dropped frames at 485fps (proven safe, current default). `p4`: ~90% utilization,
  dropped frames returned (too aggressive). **Next step: try `p2`/`p3`** to find the actual ceiling.
  `rc-lookahead=0` and `max_b_frames=0` were deliberately left untouched during this tuning pass so a
  regression is attributable to one change, not three — those are the next levers if more headroom is
  found and more quality is wanted.
- File naming matches the old pipeline's convention (`PROCESSING_SPEC_teeldyne.md` §3.22) with two
  deliberate differences: `.mp4` not `.mkv` (matches what's actually produced), and thread-id
  hardcoded to `_0` (round-robin thread-splitting isn't ported — full doc comment in `recordtask.h`).
- Timestamp `.txt`: one `timestamp_us,frame_id` CSV row per frame actually submitted to the encoder.
  **Not yet the full old column set** (histogram bins, camera status) — deferred until motion
  detection exists.
- **Logging gotcha, already solved**: `LogMessage()` calls from inside a plugin go nowhere visible —
  `eCaptureProServer`'s systemd unit has `StandardOutput=null`/`StandardError=null`, and there is no
  relay from server-side `LogMessage()` to the connected client (confirmed by testing, not assumed;
  `"Missed Save"` appearing in the client terminal is the *client-side* eSDK Pro library's own
  independent tracking, not a relayed server message). Fix: `RecordTask` publishes events via four
  separate `StringTaskParam`s (`LastSessionEvent`, `LastSegmentClosed`, `LastSegmentStarted`,
  `LastSnapshot` — separate, not one, because events fire faster than the client's 200ms poll and
  would coalesce in a shared param), which `main.cpp` polls and prints. There's a real
  `ECAPTURE_PRO_LOGLEVEL` env var + the systemd `StandardOutput=null` line if anyone wants the
  server's *own* detailed logs later (via `journalctl`), but that's a separate, unused-so-far path.

### `visss-data-acquisition-EVT/` — client (`main.cpp`)

- **PTP sync is mandatory**, not an opt-in flag: waits up to 30s per camera for `PtpStatus == "Slave"`,
  throws (aborting startup) if not reached. `PtpMode=TwoStep` only — there is no "Master" role for
  eSDK Pro cameras (confirmed via the vendor's `EVT_PTP` example); they only ever sync as a slave to
  an external grandmaster (the Mellanox NIC, here). Does **not** set `TriggerMode`/`AcquisitionMode`
  (unlike the `record_nvenc` example it started from) — matches the old VISSS leader's free-running,
  untriggered design.
- **PTP infrastructure was actually missing and had to be set up**: no `ptp4l` was running at all.
  Installed `linuxptp`, then set up the *real* systemd services from `scripts/services/` per
  `install_commands_bullseye.md` §3a ("combined leader and follower computer"): both
  `visss_ptp_master@{LEADER,FOLLOWER}_NIC_VISSS` and both
  `visss_sync_systemclock_to@{LEADER,FOLLOWER}_NIC_VISSS`. Created `/home/visss/VISSS_INTERFACES.env`
  (plain file, not the usual symlink to a `visss_config` checkout, since that sibling repo isn't
  present on this box) mapping `LEADER_NIC_VISSS=enp129s0f0np0`, `FOLLOWER_NIC_VISSS=enp129s0f1np1`.
- `-t` is optional now, default = run until interrupted. `SIGINT`/`SIGTERM` handler sets an atomic
  flag; the main poll loop (200ms) checks it and calls `pipeline.Stop()` cleanly, which finalizes
  whatever `RecordTask` segment is currently open rather than losing/corrupting the tail.
- Logging convention matches the **old pipeline's exact format** (`PrintThread`, `storage_worker_cv.h`/
  `visss-data-acquisition.h`), for a future Python launcher to parse the same way
  `launch_visss_data_acquisition.py` already does: `LEVEL[-id] | timestamp | message`, left-anchored,
  levels `INFO`/`WARNING`/`ERROR`/`BASH`. Helper: `LogLine(level, id, message)` in `main.cpp`.
- Periodic (30s) camera status log: `SensTemp` (**`Int32CameraParam`, not Float** — real feature list
  confirmed by the user) + `PtpStatus`. `PHYSNRMargin` (meant as a network-link-health substitute for
  the old pipeline's Teledyne-only transport diagnostics) turned out to **not exist on this camera**
  despite being in Emergent's docs — removed. `RoCENackCount` was flagged as a possible alternative
  from the vendor's transport-layer docs page but never confirmed present — worth checking in
  eCapture Pro's parameter browser if this is revisited.

## Not yet implemented (known gaps, in rough priority order)

1. **Actual motion detection** (`PROCESSING_SPEC_teeldyne.md` §3.18: absdiff + non-uniform 7-bin
   histogram) — `MotionDetectTask`'s border is unconditionally black; should vary by moving-pixel
   flag once this exists. This also unblocks the fuller `.txt` metadata columns.
2. **Recording-decision filtering** (§3.21) — currently every frame gets recorded; no
   motion-based/heartbeat-frame filtering yet.
3. **NVENC preset tuning** — try `p2`/`p3` next (see above).
4. Generic runtime-typed camera-parameter setter for loading a per-deployment config file (§3.2) —
   confirmed a **core feature** by the project owner, not started at all.
5. `--resetDHCP` equivalent (§3.3) — unclear if eSDK Pro exposes this; not investigated.
6. Rotation (§3.19) — optional, not implemented.
7. Digital I/O line-pulse config purpose (old config's `LineSelector`/`outputLineSource` etc.) — open
   question, never resolved with the project owner.
8. `install_commands_evt.md` — **done, 2026-08-10** (see that file). Reconstructed from the dev
   box's actual installed state (`apt-mark showmanual`, dpkg/DKMS/systemd queries), not written
   live during the original install, so headings are left empty rather than guessed at wherever
   that reconstruction couldn't recover the original choice (BIOS settings, exact CUDA-toolkit
   install method, etc.). Also see `ARCHITECTURE.md` (new, same date) for a code-level reference
   to the C++ pipeline, cross-linking rather than duplicating each component's own `README.txt`.

## Working-style notes for whoever picks this up

- The user wants **small, verified, incremental steps** — build and test on real hardware after each
  change, not speculative multi-feature leaps.
- **Verify against the real installed SDK/ffmpeg/headers before committing to a specific parameter
  name or value.** Several real bugs this session came from guessing instead of checking (wrong
  temperature parameter name/type, assuming `mjpeg` accepts `GRAY8`, missing `forced-idr`) — the
  fix each time was writing a tiny local test (`ffmpeg -h encoder=...`, a standalone `avcodec_find_encoder`
  probe, `strings`/`nm` on `libeSdkPro.so`) rather than guessing again.
- **I cannot `sudo`** in this environment — always hand those exact commands to the user rather than
  attempting them.
- The user is deliberate about the real-time pipeline staying GPU-resident end to end ("we must stay
  on GPU") — the one accepted exception is the once-per-rollover JPEG snapshot (host round-trip is
  fine there since it's off the 485fps hot path, not per-frame).
- Occasional messages come in German — respond in kind when that happens.
