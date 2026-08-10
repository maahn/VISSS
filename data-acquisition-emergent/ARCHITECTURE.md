# Architecture reference — visss-data-acquisition-EVT / motion_detect / record

This is a code-level reference for the C++ pipeline in this directory, written from the position
of "what does this codebase actually do right now" — as opposed to `HANDOVER.md` (session-history
narrative of how it got built) or `pipeline-to-esdkpro-mapping.md`/`PROCESSING_SPEC_teeldyne.md`
(the old Teledyne system and the porting plan). Each component also has its own `README.txt` with
flag-by-flag/field-by-field detail and rationale; this document is the map connecting them, not a
replacement for them. When this drifts from the code, the code (and its own class-doc comments,
which are kept intentionally thorough) is the source of truth.

## Three processes, two address spaces

```
visss-data-acquisition-EVT (client, unprivileged)
        |  connects to
        v
eCaptureProServer (root daemon, loads plugins as .so)
        |
        +-- CameraTask -----> MotionDetectTask -----> RecordTask
            (built-in,        (motion_detect/)         (record/)
             vendor SDK)
```

- **`visss-data-acquisition-EVT/src/main.cpp`** — the only binary you actually run. Parses CLI
  flags, connects to one or more `eCaptureProServer` instances (always via the SDK's "remote
  server" model, even for `127.0.0.1` — there is no in-process/local-plugin mode used here),
  discovers and opens cameras, applies optional per-camera config files, waits for PTP lock, wires
  up the `CameraTask → MotionDetectTask → RecordTask` pipeline per camera, then polls in a loop
  printing status/relaying events until `Ctrl+C`/`SIGTERM` or `--maxframes` is reached.
- **`motion_detect/src/motiondetecttask.{h,cpp}`** — a plugin `.so`, loaded by `eCaptureProServer`
  (root), *not* linked into the client. One instance per camera. Does all per-frame processing:
  motion detection, border/status-bar overlay, optional rotation, recording-decision filtering,
  decimated live-preview downscale.
- **`record/src/recordtask.{h,cpp}`** — a second plugin `.so`, also root-side. One instance per
  camera. NVENC/HEVC encoding, file rollover, metadata `.txt` writing, first-frame `.jpg` snapshot,
  `*_latest_0.*` symlinks.

Because the plugins run inside the root `eCaptureProServer` process, they have **no console** —
`LogMessage()` calls inside them go nowhere visible (`StandardOutput=null` in the systemd unit).
Anything that needs to reach a human/log file has to be surfaced through the client instead — see
"The TaskParam sync limitation" below for why that's harder than it sounds.

## Client (`main.cpp`)

### CLI flags (current, `parseArgs()`)

| Flag | Meaning |
|---|---|
| `-s <ip> <path>` | Server + its recording path. Repeatable — one client connects to N servers/manages every camera on each. |
| `-b <kbps>` | NVENC target bitrate. Default 10000. |
| `-i <sec>` | File rollover interval, 0 = never. Default 600. |
| `-n <name>` | Instrument name (file naming, overlay text). Default `VISSS`. |
| `-e <preset>` | NVENC preset `p1`-`p7`. Default `p1` (see `record/README.txt` for the p1–p7 frame-drop measurements at 485fps). |
| `-r` | Rotate 90° CCW. |
| `-m <20\|30>` | Motion-detection histogram bin-edge table selector. |
| `-w` | Disable recording-decision filtering (write every frame). |
| `-l <ratio>` | Preview refresh cadence (every N frames). Default 70. |
| `--nopreview` | Disable the `cv::imshow` live window entirely. |
| `-q <count>` | Buffers pre-registered per `MotionDetectTask` output port (pipeline backpressure slack). Default 8. |
| `-c <serial> <path>` | Per-camera plain-text config file, repeatable. A camera with no matching `-c` keeps power-on defaults. |
| `--site <name>` | Site name in the overlay text. Default `none`. |
| `--maxframes <n>` | Debug: stop after ~n frames (elapsed-time estimate, not exact — see below). |
| `-h`/`--help` | Print help, exit immediately (checked before any startup work). |

No `--novideo`/`--nometadata`/`--resetDHCP`/`-p`/`--noptp`/`-t`/`--threads`/`--cpu*` equivalents
exist (see the flag-by-flag comparison against the old Teledyne CLI in this session's history, or
re-derive by diffing against `PROCESSING_SPEC_teeldyne.md` §5) — most either don't apply to this
architecture (no per-camera-process thread model, so no `-t`/`--cpu*`) or are open gaps (`--noptp`
was deliberately dropped: PTP is unconditionally mandatory here, not optional).

### Startup sequence (`main()`)

1. `-h`/`--help` short-circuit, before any SDK calls.
2. `System::Create()`, connect to each `-s` server.
3. Discover + open every camera on every server; apply that camera's `-c` config file if any
   (`ApplyCameraConfigFile`/`SetCameraParamGeneric` — see "Generic camera-param loader" below).
4. `setPtp()` — set `PtpMode=TwoStep` on every camera, poll `PtpStatus` until `"Slave"` (30x 1s
   retries), then `pipeline.SetPtpSyncMode(true)` so the pipeline itself also refuses to run if any
   camera loses sync later. **This never soft-degrades to free-running capture** — a lock failure
   throws and aborts startup, matching the old Teledyne pipeline's own requirement.
5. Per camera: create `CameraTask`, `MotionDetectTask`, `RecordTask`, wire the three ports
   (`InFrame`/`OutFrame`/`ShouldWrite`, plus `PreviewFrame` → `ImageDisplayTask` if preview is on),
   set every task param from the parsed CLI flags.
6. `pipeline.Start()`.
7. Poll loop (200ms tick): checks `--maxframes`, relays `RecordTask` segment-lifecycle events
   (currently non-functional, see below), re-polls camera temperature/PTP every 30s
   (`PollCameraStatus`), prints a per-camera `STATUS` heartbeat every ~1s (`PrintStatusHeartbeat`),
   pumps the `cv::imshow`/`cv::waitKey` event loop if preview is on.
8. On stop: `pipeline.Stop()` (graceful — lets `RecordTask` finalize whatever segment is open).

### The generic camera-param config loader

`-c <serial> <path>` applies a plain-text `FeatureName value` file (same format/spirit as the old
Teledyne pipeline's config file) to one specific camera. eSDK Pro has no type-erased/generic
`SetValue()` — every `Camera::GetParameter<T>(name)` needs `T` known at compile time. The loader
(`SetCameraParamGeneric`) works around this by trying each known `CameraParamType`
(UInt32/Int32/Float/Bool/Enum/String/Register/Command) in turn until one's `GetParameter<T>()`
doesn't throw — since `GetParameter<T>()` itself validates name+type, and every GenICam node has
exactly one true type, this reconstructs generic dispatch from the typed-only public API. Lines
are applied in file order (so a config can put a prerequisite toggle on an earlier line); the whole
file is processed and every failure logged before throwing, rather than failing on the first bad
line — matches the old pipeline's real error policy.

### The `TaskParam` sync limitation (read this before adding client-visible server-side data)

**Confirmed by testing, twice, independently**: a `TaskParam` value set from *inside* a plugin's
own code (`RecordTask`/`MotionDetectTask`'s `Process()` or other methods calling `SetValue()` on a
parameter *they themselves* created) never propagates back to the client's later
`GetParameter<T>(name).GetValue()` calls. Only client→server writes work through this API.

This was found twice:
1. A `FrameCount` `Int32TaskParam` added to `MotionDetectTask` for `--maxframes`, incremented every
   `Process()` call — client polling never saw it move past 0.
2. `RecordTask`'s pre-existing `LastSegmentStarted`/etc. `StringTaskParam` events — the client's
   poll loop (still in `main.cpp` today, `c_recordEventParamNames`) has *never* actually printed
   one, even for a segment provably opened moments earlier.

No vendor example demonstrates the reverse direction either, and there's no documented workaround.
The one **proven-live** channel for server→client-adjacent data in this codebase is a
`DataOutput`/`DataInput` port pair (like `ShouldWrite` between `MotionDetectTask`→`RecordTask`) or
a `FrameOutput`→`ImageDisplayTask` client callback (like the live preview) — both are push-based
port mechanisms, not a parameter read-back.

**Practical consequence**: `--maxframes` is an elapsed-time × known-camera-framerate *estimate*,
not a live frame count (see `PrintStatusHeartbeat`'s and `Params::m_maxFrames`'s comments in
`main.cpp`) — this is why, not a simplification for its own sake. The `STATUS` heartbeat (below)
has the same constraint.

### `STATUS` heartbeat (`PrintStatusHeartbeat`, ~1s cadence)

Added 2026-08-10 because the Python launcher's per-camera status widget/Clean-button gating needs
to see a line starting with `STATUS` to know the pipeline is actually alive (matching the old
Teledyne pipeline's own once-per-second `STATUS<id> | ...` line, `storage_worker_cv.h:743`). Can't
reproduce that old line's live queue-length/histogram content for the reason above, so it reports
what the client *can* honestly observe: real camera temperature/PTP status (direct GenICam reads,
which — unlike `TaskParam`s set by a plugin — do work reliably every time, confirmed throughout
this session) plus the same elapsed-time frame estimate as `--maxframes`, explicitly marked `~`.

## `motion_detect/` — `MotionDetectTask`

One instance per camera. Per-frame, in `Process()`:
1. Optional 90° CCW rotation of the content region (`Rotate` param) — before the border, so it
   doesn't rotate the status bar.
2. Motion detection: fused CUDA kernel (`kernel.cu`'s `motionDiffKernel`) does absdiff between this
   frame and the previous one plus a 7-bin cumulative histogram in one pass, then an adaptive
   per-bin threshold decides "moving" — a faithful port of the old pipeline's algorithm, including
   its integer-division quirks (`MinBrightChange` selects which of two bin-edge tables, `20` or
   `30`).
3. Recording-decision filtering (`WriteAllFrames` bypasses this): a frame is written if it's the
   first frame, motion was detected, or ~10s have passed since the last written frame (heartbeat,
   so a static scene still gets *some* footage). The decision — plus the 7 histogram counts and a
   computed queue-length figure — is packed into `ShouldWritePortPayload` and pushed on the
   `ShouldWrite` `DataOutput` port *every frame*, in lockstep with the main frame output. This is
   deliberate, not incidental: eSDK Pro's `FrameOutput` requires every registered buffer to be
   pushed every `Process()` cycle to stay recyclable — skipping `PushFrame()` on a filtered frame
   starves the buffer and stalls the whole pipeline (this was found and fixed earlier in the port;
   see the class doc comment for the exact symptom, a runaway "Missed Save" counter).
4. Status-bar overlay (site | timestamp | name | `Q:<queue>` | frame ID | `N.R.` if not moving),
   drawn into a border region above the frame via a custom CUDA bitmap-font kernel.
5. Decimated preview (`LiveRatio`, default 70; `NoPreview` disables): downscaled on GPU
   (`kernel.cu`'s `downscaleKernel`), downloaded to host, pushed on the `PreviewFrame` port to the
   client's `ImageDisplayTask` callback — only on previewed frames, so the extra work is rare.

Ports: `InFrame`/`OutFrame` (Cuda), `ShouldWrite` (Host `DataOutput`), `PreviewFrame` (Host
`FrameOutput`). Params: `Site`, `Name`, `Rotate`, `MinBrightChange`, `WriteAllFrames`, `Framerate`,
`LiveRatio`, `NoPreview`, `QueueDepth`.

## `record/` — `RecordTask`

One instance per camera, replacing eSDK Pro's built-in `NvencTask` (see `record/README.txt` for
why — short version: needed control over file naming/rollover/metadata that the built-in task
doesn't expose). Per-frame: reads `ShouldWrite` from the paired `DataInput` port, converts mono8→
NV12 on GPU (Y plane = `cudaMemcpy2D`, U/V = `cudaMemset2D` to constant 128 — no CUDA kernel
needed, since the source is grayscale-only), feeds the persistent NVENC session (opened once in
`Init()`, not per-rollover — NVENC session creation is too slow to do on the hot path at 485fps),
writes the companion `.txt` metadata row if `shouldWrite`.

**File rollover** (`NewFileIntervalSec`, tracked in the *frame-timestamp* domain, not wall clock —
a wall-clock debounce used to silently skip every other rollover at small intervals, fixed earlier
in the port): only the muxer (`AVFormatContext`) rotates; the encoder stays open, since
`hevc_nvenc` supports `AV_CODEC_CAP_ENCODER_FLUSH` so a rollover just drains pending packets into
the closing file. On rollover: `*_latest_0.mp4`/`.txt`/`.jpg` symlinks are atomically replaced
(`CreateSymlink` — write a `.tmp` symlink, rename over the real one).

**Metadata `.txt` format, version `e.1`** (bumped from `0.6` on 2026-08-10): header includes real
camera temperature/PTP status (relayed from the client's periodic GenICam reads — `RecordTask` has
no direct camera access), real camera-config filename (`CameraConfigName`, or `"none"`), and drops
the Teledyne-only transfer-diagnostic lines (`transferQueueCurrentBlockCount`/`transferMaxBlockSize`)
that were always `-99` here (no eSDK Pro equivalent found). See the class doc comment in
`record/src/recordtask.h` for the exact header/CSV-column layout — treat any further change to this
format as compatibility-breaking for downstream `VISSSlib` and bump the version again.

Params: `OutputRoot`, `Name`, `DeviceId`, `Width`, `Height`, `Framerate`, `BitrateKbps`,
`NewFileIntervalSec`, `Preset`, `MinBrightChange`, `Temperature`, `PtpStatus`, `CameraConfigName`,
plus the four (deliberately separate, not one) event params `LastSessionEvent`/
`LastSegmentClosed`/`LastSegmentStarted`/`LastSnapshot` — separate so events firing within one
`Process()` call don't coalesce before the client's next poll (moot in practice per the `TaskParam`
sync limitation above, but the separation itself is still correct design).

## `ShouldWritePortPayload` — the one cross-plugin wire format

```cpp
#pragma pack(push, 1)
struct ShouldWritePortPayload
{
    uint8_t shouldWrite;
    uint32_t queueLength;
    uint32_t histCounts[7];
};
#pragma pack(pop)
```
Defined identically (byte-for-byte, deliberately duplicated rather than shared via a cross-plugin
header, since each plugin is an independent CMake project/`.so`) in both
`motion_detect/src/motiondetecttask.h` and `record/src/recordtask.h`. If you change one, change
both — nothing enforces this at compile time.

## Known gaps / things to check before relying on them

- **Root privilege**: `eCaptureProServer` runs as root (vendor default). A 2026-08-10 investigation
  found real `RLIMIT_RTPRIO`/`RLIMIT_MEMLOCK`-class `EPERM` failures blocking non-root operation,
  applied a systemd-level fix for those specifically, and the camera stream still failed to open
  identically — root cause not found, reverted, pending vendor support response. See
  `install_commands_evt.md`'s "eCapture Pro" section.
- **`RecordTask` event relay** (`LastSessionEvent` etc., surfaced in `main.cpp`'s poll loop):
  non-functional, see "The TaskParam sync limitation" above. The code is still there (harmless,
  just silent) — don't spend time debugging it further without a port-based redesign.
- **Multi-camera-per-`eCaptureProServer`-process**: the client and this pipeline can in principle
  handle several cameras behind one `-s` server, but a known collision exists if they do —
  `RecordTask`'s `*_latest_0.*` symlinks are named from the shared `-n`/`Name` value, not per
  camera, so two cameras on the same server sharing one output root would overwrite each other's
  `_latest` symlinks. Not a problem for the current one-camera-per-host leader/follower topology;
  would need a fix (e.g. including the device ID in the shared-root symlink name) if that topology
  ever changes.
- **SDK-internal log lines**: not reconfigurable (confirmed via binary inspection of
  `libeSdkPro.so` — hardcoded `spdlog` pattern, no supported format-override API). Left as-is by
  design; this client's own log lines already match the old Teledyne format exactly.
