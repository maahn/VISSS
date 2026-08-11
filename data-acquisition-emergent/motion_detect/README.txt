motion_detect
============
Single consolidated plugin for all per-frame processing between the camera and the encoder,
mirroring the old VISSS pipeline's storage_worker_cv::run() stage as one task rather than
several - motion detection, rotation, status-bar overlay, and the recording decision all share
per-frame state (histogram bins, moving-pixel flag, etc.) that only ever needs to live within
this one stage, so there's no reason to pay inter-task port overhead moving it between separate
plugins.

Currently implemented: extends each frame with a border region at the top (64px, matching the old
pipeline's hardcoded frameborder; black, or gray (100) if nothing moved this frame - a purely
visual "nothing happened" flag) and draws a status-bar text overlay into it
(site | timestamp | name | Q:<queue length> | H:<edge>[ N.R.] | M:<move%>), via a GPU-resident
copy + border fill + custom CUDA bitmap-font kernel (font8x14.h, generated with a throwaway
Python/PIL script, not hand-transcribed, to avoid glyph-correctness risk; rendered at 2x size via
kernel.cuh's c_fontScale - change that constant, not the font data, to resize further). Text field
notes:
- `name` is the client's `-n` value (StringTaskParam, set by main.cpp - was silently stuck at this
  task's own "VISSS" default until 2026-08-11, -n only reached RecordTask before that).
- `H:` is the highest-triggered histogram bin's edge value (3-digit zero-padded, e.g. "030"),
  matching the old pipeline's exact scan-from-highest-bin logic (storage_worker_cv.h) - empty (just
  " N.R.") if nothing crossed any bin's adaptive threshold this frame.
- `M:` is the percentage of frames written (not just "moving" - matches the old pipeline's own
  frame_count_moving semantic, which counts on shouldWrite not on movingPixel alone) since the
  current output file opened. Per-file reset without any cross-task signaling: this task
  independently derives the same rollover-boundary crossing RecordTask uses from the same frame
  timestamps (see the `NewFileIntervalSec` task param and Process()'s comment) - no port carries
  data from RecordTask back to this task, so this is the only way to get per-file semantics here.
Optional 90-degree counterclockwise rotation of the content region (the `Rotate` task param,
matching the old CLI's -r/--rotateimage) - applied after motion detection but before the
border/overlay is composited, so enabling it swaps the output frame's width/height versus the raw
camera frame.

Motion detection (fused CUDA absdiff + 7-bin cumulative histogram + adaptive threshold, see
kernel.cu's motionDiffKernel - PROCESSING_SPEC_teeldyne.md §3.18 for the old algorithm this is a
faithful port of, including its integer-division quirks) and recording-decision filtering (only
moving/first/periodic-~10s-heartbeat frames get shouldWrite=1, §3.21) are implemented - see the
`MinBrightChange`/`WriteAllFrames`/`Framerate` task params and motiondetecttask.h's class doc for
details and the one deliberate architectural deviation from the old pipeline (no `newFile`-forced
write, since RecordTask owns rollover independently).

Live preview (§3.25) is also implemented: a decimated (`LiveRatio`, default every 70th frame)
copy of the fully composited frame, downscaled on GPU, downloaded to host, and pushed on a
separate `PreviewFrame` port to an eSDK Pro `ImageDisplayTask`, which relays it to a callback in
the *client* process - shown in a real cv::imshow window there, matching the old pipeline exactly
(not a JPEG file - see visss-data-acquisition-EVT/README.txt's -l/--nopreview flags and main.cpp's
OnPreviewFrame). Only fires on previewed frames, so the extra downscale/download work is rare.

Runs as a plugin loaded by the eCaptureProServer daemon (same deployment model as the vendor
cuda_brightness/cpu_brightness examples), not compiled into a client app - visss-data-acquisition-EVT
always connects via System::ConnectServer(ip), which talks to that separate server process even
for 127.0.0.1, so the plugin must ship as a .so the server loads, not be registered inside the
client's own process.

ShouldWrite port and the every-frame-push contract
============
The recording decision (plus the 7 histogram counts and a measured queue-length figure) is packed
into a ShouldWritePortPayload struct and pushed on the `ShouldWrite` DataOutput port *every
frame*, in lockstep with the main OutFrame frame output - not just on frames that get written.
This is deliberate: eSDK Pro's FrameOutput requires every registered buffer to be pushed every
Process() cycle to stay recyclable - skipping PushFrame() on a filtered-out frame starves the
buffer and stalls the whole pipeline (found and fixed during the port; the symptom was a runaway
"Missed Save" counter and eCaptureProServer stuck in `deactivating` for minutes with leaked
threads). See ../README.md's eSDK Pro primer for the general version of this lesson. RecordTask
reads both InFrame and ShouldWrite each Process() call and decides there whether to actually
encode/write - see ../record/README.txt for the exact wire format (defined identically, byte-for-
byte, in both motiondetecttask.h and recordtask.h; if you change one, change both).

Task params: Site, Name, Rotate, MinBrightChange, WriteAllFrames, Framerate, LiveRatio,
NoPreview, QueueDepth (buffers pre-registered per output port - this pipeline's equivalent of the
old bounded frame_queue between capture and storage threads; unlike that queue's hardcoded 3000-
frame depth, this is a small pre-allocated pool, tested clean at 485fps with values as low as 1
through p5 preset - see record/README.txt's preset notes; doesn't rescue a sustainedly-too-slow
preset, only absorbs transient slowdowns).

Build
============
Linux:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build`

Install
============
Copy the built library into eCaptureProServer's plugin directory and restart the service so it
picks it up:
- `sudo cp build/libmotion_detect.so /opt/EVT/eCapturePro/eSdkPro/plugins/`
- `sudo systemctl restart evt_eCaptureProServer.service`

If a previous libstatus_bar.so is still present from before this plugin was renamed/consolidated,
remove it first so eCaptureProServer doesn't load a stale duplicate:
- `sudo rm -f /opt/EVT/eCapturePro/eSdkPro/plugins/libstatus_bar.so`

Verify it loaded by checking the service log, or that
`pipeline.CreatePluginTask(server, "MotionDetect")` in a client app no longer throws
"Plugin MotionDetect not found".
