record
============
Encodes GPU-resident frames via FFmpeg's hevc_nvenc and muxes them directly to a file, replacing
eSDK Pro's built-in NvencTask. See recordtask.h for why (short version: NvencTask's native
segmented recording rotates the video file but not its accompanying per-frame timestamp file,
which VISSS needs to be frame-exact synced with the video).

Implements the full VISSS folder/naming convention and periodic rollover (see recordtask.h's
class doc comment for the exact layout and rollover mechanics - the encoder session is opened
once and reused across rollovers via avcodec_flush_buffers(), not reopened each time, so
rollovers cost no dropped frames either), plus a per-frame "timestamp_us,frame_id" .txt sidecar
written in the same Process() call that encodes each frame - sync-by-construction with the video.

Not yet implemented: the fuller old metadata column set (histogram bins, camera status, etc. -
deferred until motion detection exists) and the first-frame-of-file .png/.jpg snapshot (acceptable
as a later helper script extracting frame 0 from each finalized .mp4, per project owner).

Configured via task parameters, set by the client after CreatePluginTask() (see
visss-data-acquisition-EVT/src/main.cpp): OutputRoot, Name, DeviceId, Width, Height, Framerate,
BitrateKbps, NewFileIntervalSec.

Runs as a plugin loaded by the eCaptureProServer daemon (same deployment model as motion_detect
and the vendor cuda_brightness example), not compiled into a client app.

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
