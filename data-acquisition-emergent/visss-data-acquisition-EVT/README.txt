visss-data-acquisition-EVT
============
VISSS data acquisition for Emergent Vision cameras via eSDK Pro. Started as a copy of the
eSDK Pro `record_nvenc` example; see ../pipeline-to-esdkpro-mapping.md for the port plan.
Connects to all cameras on a given server using GPUDirect. Pipeline per camera:
CameraTask -> MotionDetectTask (../motion_detect/) -> RecordTask (../record/), the latter
replacing eSDK Pro's built-in NvencTask - see ../record/README.txt for why.

Build
============
For building this example, open a terminal and move to this example folder, then execute the following commands:

Windows (Powershell):
- `cmake -S . -B build`
- `cmake --build build --config Release`

Linux:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build`

Command Line Interface:
============
-s <server IP> <server recording path>
    Identifies a server to use. The recording path is relative to the eSDKPro server process so it must be an absolute path.
    eSDK Pro adds the current timestamp to the output directory to differentate between multiple recordings which use the same root path.
-b <bitrate Kbps>
    The target bitrate in Kbps. Default 10000.
-i <num sec>
    New-file interval: RecordTask closes the current file and starts a new one every this-many
    seconds (0 = never), no dropped frames, no pipeline restart. Default 600. See
    ../record/recordtask.h for the exact file naming/rollover mechanics.
-n <name>
    Instrument name, used in file naming (matches the old CLI's -n/--name). Default VISSS.
-e <preset>
    nvenc preset (p1 fastest/lowest-quality .. p7 slowest/highest-quality at a fixed bitrate).
    Default p1. Still being tuned against real hardware - see record/recordtask.h's Preset param
    comment for measured data points (p1: ~50% NVENC utilization, 0 dropped frames at 485fps;
    p4: ~90% utilization, dropped frames returned).
-r
    Rotate the recorded frame 90 degrees counterclockwise (matches the old CLI's
    -r/--rotateimage, PROCESSING_SPEC_teeldyne.md §3.19). Off by default. Fixed angle only, not
    configurable. Swaps width/height of the recorded video versus the raw camera frame.
-m <20|30>
    Motion-detection histogram bin-edge table selector (matches the old CLI's
    -b/--minBrightChange, §3.18). Must be 20 or 30. Default 20.
-w
    Disable recording-decision filtering (matches the old CLI's -w/--writeallframes, §3.21):
    every frame is written regardless of motion. Off by default (filtering enabled).
-l <ratio>
    Live preview decimation (matches the old CLI's -l/--liveratio, §3.25): refresh the preview
    window every this-many frames. Default 70.
--nopreview
    Disable live preview generation entirely (matches the old CLI's --nopreview). Preview is a
    real GUI window per camera (cv::imshow, requires an X session - DISPLAY must be set), matching
    the old pipeline's own preview exactly. Needs libopencv-dev at build time.
-q <count>
    Buffers pre-registered per MotionDetectTask output port - the new pipeline's equivalent of the
    old bounded frame_queue between capture and storage threads, giving Process() slack to absorb
    a momentary downstream slowdown instead of blocking immediately. Tested clean (0 dropped/
    missed) at 485fps with values as low as 1 up through p5 (see record/recordtask.h's Preset
    comment) - doesn't rescue a preset that's sustainedly too slow (p6/p7), only transient
    slowdowns. Default 8, a modest safety margin.
-c <serial> <path>
    Optional per-camera config file (repeatable, one per camera), applied to the matching camera
    right after it's opened, before PTP sync/recording starts. A camera whose serial has no
    matching -c keeps its power-on-default parameters. Plain text, one "FeatureName value" pair
    per line (e.g. "FrameRate 485"), using the camera's actual GenICam feature names - same
    format/spirit as the old Teledyne pipeline's config file. No YAML/JSON dependency - values are
    applied by probing each known camera-parameter type (UInt32/Int32/Float/Bool/Enum/String/
    Register/Command) until one matches, so new parameter names can be added to the file without
    any code changes. If ANY line fails (unknown feature, bad value), every line's result is still
    logged, but the whole program aborts startup rather than running with a silently partial/wrong
    camera configuration - fix the file and rerun.
--site <name>
    Site name shown in the status-bar overlay text (matches the old CLI's -s/--site; a long flag
    here since -s is already this file's server flag). Default "none".
--maxframes <count>
    Debug: stop the run once ~<count> frames have elapsed (matches the old CLI's -m/--maxframes;
    a long flag here since -m is already this file's MinBrightChange flag). This is an
    elapsed-time estimate (frames / camera FrameRate), not an exact live frame count - eSDK Pro
    TaskParam values set from inside a plugin's Process()/other methods never propagate back to
    the client (confirmed by testing; the same root cause as RecordTask's segment-lifecycle
    events never appearing in the console - see ../record/recordtask.h). Frames arrive at a
    steady, PTP-disciplined rate, so the estimate is accurate to a fraction of a second in
    practice. Default: unlimited (the deployment default).
-h, --help
    Print a quick-reference flag summary and exit. Checked before any startup work, so this is
    fast/side-effect-free even without cameras or a server reachable.

PTP sync is mandatory, not a flag: the program waits for every camera to reach PTP Slave status
(requires a grandmaster reachable on the camera network, e.g. the Mellanox NIC) and aborts
startup if any camera fails to lock within 30s. See setPtp() in src/main.cpp.

Eg: To record on a local server in the current directory, stopping after 1000 frames:

Windows (Powershell):
- `.\build\Release\visss-data-acquisition-EVT.exe -s 127.0.0.1 (Get-Location).Path --maxframes 1000`

Linux:
- `./build/visss-data-acquisition-EVT -s 127.0.0.01 $PWD --maxframes 1000`

Notes
============
- On Windows you may not have permissions to create/modify files in the installed example directory. If so, move this example to another location before building.
- On Windows you must add the eSdkPro bin directory to the current terminal session before running the example, for finding the necessary dependencies, e.g: `$env:PATH = "$env:PATH;$env:ECAPTURE_PRO_DIR\eSdkPro\bin"`