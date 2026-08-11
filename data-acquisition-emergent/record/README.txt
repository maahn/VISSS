record
============
Encodes GPU-resident frames via FFmpeg's hevc_nvenc and muxes them directly to a file, replacing
eSDK Pro's built-in NvencTask entirely. Why: NvencTask's native segmented-recording rotates the
video file but not its accompanying per-frame timestamp/metadata sidecars — those are constructed
once per whole session (confirmed via symbols in libeSdkPro.so: NvencTaskWorker has one
constructor call, not one per segment), and it owns its own output path/subdirectory/timestamp
naming convention (baseRecordingPath + auto-created subdirectories, GetLastRecordingPath()) with
no knowledge of VISSSlib's staging->final rename, *_latest symlink-via-atomic-rename convention,
or per-file metadata .txt sidecar. VISSS needs frame-exact video/timestamp sync and its own exact
naming convention, so this plugin encodes and writes both in the same Process() call —
sync-by-construction — via libavcodec's hevc_nvenc + libavformat mux, not cv::VideoWriter.

Runs as a plugin loaded by the eCaptureProServer daemon (same deployment model as motion_detect
and the vendor cuda_brightness example), not compiled into the client app. One instance per
camera. See ../README.md for the eSDK Pro plugin lifecycle/port contract in general.

Design points (all verified against the real installed SDK/ffmpeg, not assumed)
============
- Encoder session opened once in Init(), kept alive for the task's whole lifetime, not reopened
  per rollover. avcodec_open2() (NVENC session creation) is genuinely slow (100ms+); at 485fps
  that's enough time to drop 50-100 frames if done lazily on the first Process() call. This is why
  Width/Height are task params set by the client before the pipeline starts, not lazily inferred
  from the first frame.
- Rollover swaps the muxer only, not the encoder. hevc_nvenc declares
  AV_CODEC_CAP_ENCODER_FLUSH (confirmed by writing a tiny standalone test program against the
  installed libavcodec). So a rollover: drains pending packets into the closing file via
  avcodec_flush_buffers(), then opens a fresh muxer for the next file. No NVENC session reopen, no
  dropped frames at rollover boundaries. See recordtask.h's class doc for the exact file
  naming/rollover mechanics and RollSegmentIfNeeded()'s frame-timestamp-domain tracking (a
  wall-clock debounce used to silently skip every other rollover at small -i values; fixed by
  tracking the last rollover in the frame-timestamp domain instead of time(nullptr)). Rollovers
  land exactly on unixtime % NewFileIntervalSec == 0 (e.g. :00/:10/:20 for -i 600), for predictable
  file-start times - except within c_minSecondsBeforeRollover (10s) of the task's first frame,
  where a boundary crossing is deliberately suppressed: the first segment opens unconditionally on
  the first frame regardless of alignment, so if startup happened to land just before a boundary,
  the very next frame would otherwise immediately roll over again into a second, near-zero-length
  file - project owner's explicit ask (2026-08-11). motion_detect's MotionDetectTask mirrors this
  exact suppression (its own independently-derived M:/H: reset boundary, kept in sync with this
  task's real rollover - see motion_detect/README.txt), and so does the client's own
  PrintNewFileNotice prediction (main.cpp) - all three must stay in sync if this ever changes.
- forced-idr=1 AVOption is required, not just pict_type=AV_PICTURE_TYPE_I on the first frame of a
  new segment. Without it, every segment after the first rollover opened to a black/corrupt frame
  in players — HEVC allows non-IDR "keyframes" that still reference content before them, so the
  flushed-and-reused encoder needs to be told explicitly to emit a true IDR at segment boundaries.
- rc=cbr must be set explicitly. -rc defaults to "let the preset decide," and at preset=p1 that
  wasn't CBR — measured output was ~2.7x over the requested bitrate until fixed.
- mono8->nv12 conversion needs zero CUDA kernel code: Y plane is a straight cudaMemcpy2D, U/V
  plane is cudaMemset2D'd to a constant 128 (neutral chroma). hevc_nvenc doesn't accept mono8/gray
  directly (verified against the encoder's actual supported-format list).
- First-frame .jpg snapshot per segment via libavcodec's mjpeg encoder, written straight to its
  final path the moment a new segment's first frame arrives (matches the old pipeline's own
  cv::imwrite(filename_final_ + ".jpg", ...) exactly). Needs AV_PIX_FMT_YUVJ420P, not GRAY8
  (mjpeg only supports yuvj420p/422p/444p, verified) — same Y=data/UV=128 trick as the video path,
  done once per rollover on the CPU (a small host round-trip here is fine, it's off the real-time
  per-frame path unlike the main video encode).
- NVENC preset: p1-p5 measured clean (0 dropped frames) at 485fps; p6/p7 overload (dropped frames
  return, and QueueDepth doesn't rescue a sustainedly-too-slow preset, only transient slowdowns —
  see visss-data-acquisition-EVT/README.txt's -q flag). p1 kept as the default since raising it is
  a deployment decision, not a code default.
- File naming matches the old pipeline's convention (PROCESSING_SPEC_teeldyne.md §3.22) with two
  deliberate differences: .mp4 not .mkv (matches what's actually produced), and thread-id
  hardcoded to _0 (round-robin thread-splitting isn't ported at all — GPU-resident processing
  removed the reason for it, confirmed with the project owner during the port).
- *_latest_0.{mp4,txt,jpg} symlinks are named {OutputRoot}/{Name}_{DeviceId}_latest_0.* -
  DeviceId included, not just Name, because several cameras can share one client process (see
  visss-data-acquisition-EVT/README.txt). Without DeviceId, two cameras behind one server (a
  real, confirmed deployment shape, not a hypothetical edge case) would both write the *same*
  _latest path and race each other - found exactly this way on real hardware (only one _latest
  set existed after a real 2-camera run), fixed 2026-08-11. Name itself was ALSO a shared -n CLI
  value at first (same bug class: every camera's own recording showed up under whichever name -n
  happened to be, e.g. a follower camera's files literally named "..._leader_...") until
  main.cpp gained --name <serial> <name> the same day - Name here is now that camera's own
  per-camera override when one is given, only falling back to the shared -n otherwise. The Python
  launcher's EmergentInstrument constructs its wiper lastImage path to match this exactly (this
  camera's own YAML "name" + serial, since --name is now always emitted per camera) - if you ever
  change this naming, update that too.
- Every file this task creates (the finalized .mp4/.txt, the .jpg snapshot, and the directories
  built to hold them) is chowned to the "visss" user (ChownToVisss, recordtask.cpp) right after
  it's created/moved into its final location - eCaptureProServer runs as root (see ../README.md's
  "Known gaps"), so without this everything recorded would be root-owned and unreadable/
  undeletable by the account everything else in the deployment (sync scripts, VISSSlib, the
  Python launcher's own log/status files) runs as. Best-effort: logged but never fatal if the
  "visss" account doesn't exist on a given host or the chown itself fails.

Per-frame flow / ShouldWritePortPayload
============
Reads a ShouldWrite payload from the DataInput port paired with the main frame InFrame port, sent
by motion_detect's MotionDetectTask every frame (not just written frames — see motion_detect's own
README for why every-frame delivery is required by the port-buffer contract). The payload struct
is defined identically (byte-for-byte, #pragma pack(1)) in both recordtask.h and
motiondetecttask.h — duplicated rather than shared via a cross-plugin header since each plugin is
an independent CMake project/.so; if you change one, change both:

    struct ShouldWritePortPayload
    {
        uint8_t shouldWrite;    // recording decision: 0 = skip, 1 = write
        uint32_t queueLength;   // motion_detect's measured backpressure
        uint32_t histCounts[7]; // motion-detection histogram bin counts
    };

Metadata .txt format
============
Version `e.1` (bumped from `0.6` on 2026-08-10). Header includes: git tag/branch (compile-time, via
CMake `git describe`), real camera temperature/PTP status (relayed from the client's periodic
GenICam reads via the Temperature/PtpStatus task params — this plugin has no direct camera
access of its own), real camera-config filename (CameraConfigName param, or "none" if that
camera had no matching -c flag). Does NOT include the old Teledyne-only transfer-diagnostic lines
(transferQueueCurrentBlockCount/transferMaxBlockSize, always -99 here, no eSDK Pro equivalent
found). See recordtask.h's class doc comment for the exact header/CSV-column layout — treat any
further change to this format as compatibility-breaking for downstream VISSSlib and bump the
version again.

Task params (set by the client after CreatePluginTask(), see
visss-data-acquisition-EVT/src/main.cpp): OutputRoot, Name, DeviceId, Width, Height, Framerate,
BitrateKbps, NewFileIntervalSec, Preset, MinBrightChange, Temperature, PtpStatus,
CameraConfigName, plus four separate event params (LastSessionEvent, LastSegmentClosed,
LastSegmentStarted, LastSnapshot — deliberately separate rather than one, so events firing within
one Process() call don't coalesce before the client's next poll).

Known non-functional path: the client's main.cpp polls those four event params every loop tick
and prints any new value — this has never actually printed anything, even for a segment provably
opened moments earlier. Root cause: TaskParam values set from inside a plugin's own Process() (or
any other plugin-side method) never propagate back to a client GetParameter<T>().GetValue() call
— confirmed by testing, see ../README.md's eSDK Pro primer section. The code is otherwise harmless
(just silent); don't spend time debugging it further without a port-based redesign (a
DataOutput/DataInput pair, like ShouldWrite above, is the only channel confirmed to actually work
for server->client data in this codebase).

Build
============
Linux:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build`

Needs libavcodec-dev/libavformat-dev/libavutil-dev (matching the version already installed as an
eCapturePro runtime dependency):
- `sudo apt install libavcodec-dev libavformat-dev libavutil-dev`

Install
============
- `sudo cp build/librecord.so /opt/EVT/eCapturePro/eSdkPro/plugins/`
- `sudo systemctl restart evt_eCaptureProServer.service`
