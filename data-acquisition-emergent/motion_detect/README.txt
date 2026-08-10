motion_detect
============
Single consolidated plugin for all per-frame processing between the camera and the encoder,
mirroring the old VISSS pipeline's storage_worker_cv::run() stage as one task rather than
several - motion detection, rotation, status-bar overlay, and the recording decision all share
per-frame state (histogram bins, moving-pixel flag, etc.) that only ever needs to live within
this one stage, so there's no reason to pay inter-task port overhead moving it between separate
plugins. See ../pipeline-to-esdkpro-mapping.md §3 for the target decomposition.

Currently implemented: extends each frame with a border region at the top (64px, matching the old
pipeline's hardcoded frameborder; black, or gray if nothing moved) and draws a status-bar text
overlay into it (site | timestamp | name | frame id [| N.R.]), via a GPU-resident copy + border
fill + custom CUDA bitmap-font kernel. Optional 90-degree counterclockwise rotation of the content
region (the `Rotate` task param, matching the old CLI's -r/--rotateimage) - applied after motion
detection but before the border/overlay is composited, so enabling it swaps the output frame's
width/height versus the raw camera frame.

Motion detection (§3.18: fused CUDA absdiff + 7-bin cumulative histogram + adaptive threshold,
see kernel.cu's motionDiffKernel) and recording-decision filtering (§3.21: only moving/first/
periodic-heartbeat frames are pushed downstream to RecordTask) are implemented - see the
`MinBrightChange`/`WriteAllFrames`/`Framerate` task params and motiondetecttask.h's class doc for
details and the one deliberate architectural deviation from the old pipeline (no `newFile`-forced
write, since RecordTask owns rollover independently).

Live preview (§3.25) is also implemented: a decimated (`LiveRatio`, default every 70th frame)
copy of the fully composited frame, shown at ~50% of the original size, is pushed on a separate
`PreviewFrame` port to an eSDK Pro `ImageDisplayTask`, which relays it to a callback in the
*client* process - shown in a real cv::imshow window there, matching the old pipeline exactly. See
visss-data-acquisition-EVT/README.txt's -l/--nopreview flags and main.cpp's OnPreviewFrame.

Runs as a plugin loaded by the eCaptureProServer daemon (same deployment model as the vendor
cuda_brightness/cpu_brightness examples), not compiled into a client app - visss-data-acquisition-EVT
always connects via System::ConnectServer(ip), which talks to that separate server process even
for 127.0.0.1, so the plugin must ship as a .so the server loads, not be registered inside the
client's own process.

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
