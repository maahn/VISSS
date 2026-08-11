# data-acquisition-emergent

VISSS's C++ capture pipeline, ported from the Teledyne DALSA GigE-V SDK (`../data-acquisition/`,
reference-only — do not edit) onto Emergent Vision cameras via eSDK Pro. Old Teledyne-camera
deployments continue indefinitely — this is not a full migration/cutover, both SDKs need
long-term support side by side. The Python launcher (`launch_visss_data_acquisition.py`, in this
directory, symlinked from `../data-acquisition/`) already supports both.

Start here, then follow the links below for depth — each component's own `README.txt` is written
to be extensive enough to work on that component without any other context.

## What's in this directory

| Path | What it is |
|---|---|
| `visss-data-acquisition-EVT/` | The client binary you actually run. See its `README.txt`. |
| `motion_detect/` | Server-side plugin `.so`: motion detection, overlay, rotation. See its `README.txt`. |
| `record/` | Server-side plugin `.so`: NVENC encode, file rollover, metadata. See its `README.txt`. |
| `launch_visss_data_acquisition.py` | Python GUI launcher, shared with the old Teledyne binary. |
| `build_and_install.sh` | Builds all three C++ projects, installs the plugins, restarts the server. |
| `install_commands_evt.md` | OS/environment install runbook (apt packages, NVIDIA/Mellanox stack, eCapture Pro installer). |
| `PROCESSING_SPEC_teeldyne.md` | SDK-agnostic spec of exactly what the *old* Teledyne pipeline does — kept because dozens of comments in this codebase cite it by section number (`§3.19` etc.) as the source of truth for "matches the old behavior." Don't delete without fixing those citations. |
| `bitrate_per_pixel.py` | Standalone helper, unrelated to the main pipeline. |

## Architecture

```
visss-data-acquisition-EVT (client, unprivileged, one process manages every camera it's told about)
        |  System::ConnectServer(ip) -- always the "remote server" model, even for 127.0.0.1
        v
eCaptureProServer (root daemon, loads plugins as .so files from /opt/EVT/eCapturePro/eSdkPro/plugins/)
        |
        +-- CameraTask -----> MotionDetectTask -----> RecordTask
            (built into      (motion_detect/,          (record/,
             the SDK)         one instance/camera)      one instance/camera,
                                                         replaces the SDK's own NvencTask)
```

`MotionDetectTask`/`RecordTask` are **not** linked into the client — they're separate `.so`
plugins loaded by `eCaptureProServer`, which is why they have no console (their `LogMessage()`
calls go nowhere visible — `StandardOutput=null` in the systemd unit) and why anything they need
to surface has to go through a task parameter or port that the client polls/receives, not a
`printf`.

## Build / install / run

```bash
cd data-acquisition-emergent
./build_and_install.sh              # builds all 3 projects, installs plugins, restarts the server (sudo)
./build_and_install.sh --no-install  # build-only, no sudo needed
./build_and_install.sh --clean       # wipe build/ dirs first

cd visss-data-acquisition-EVT
./build/visss-data-acquisition-EVT -s 127.0.0.1 $PWD -i 20 -b 15000 -e p1
```

`EMERGENT_DIR=/opt/EVT`/`ECAPTURE_PRO_DIR=/opt/EVT/eCapturePro` are needed to build — normally set
by `/etc/profile.d/evt.sh` in a login shell, but `build_and_install.sh` also defaults them itself
since non-login shells (CI, agent tool shells) don't source `/etc/profile.d/`.

Full OS/package install runbook: `install_commands_evt.md`.

## eSDK Pro primer (the parts that matter for this codebase)

A client–server C++ SDK: you build a **pipeline** (a graph of **tasks** connected by typed
**ports**) and call `Start()`/`Stop()`; concurrency and buffering between tasks are handled
internally. Custom processing is a `TaskWorker` subclass (`eSdkPro::Plugin` namespace), with a
fixed lifecycle: constructor (create ports/params) → `Init()` → `Process()` (called once per
available input, pure virtual — this is where per-frame logic lives) → `Stop()`/`Deinit()`.

**The one lesson that actually cost debugging time**: every buffer registered on an output port
via `RegisterOutputFrame()`/`RegisterOutputData()` **must be pushed every `Process()` cycle** to
stay recyclable — `GetOutputFrame()`/`GetOutputData()` blocks forever on the next call if you skip
a push. This is why `motion_detect/`'s `ShouldWrite` port exists at all: it would be simpler to
just not push a frame that's being filtered out, but doing that once stalled the whole pipeline
(visible as a runaway "Missed Save" counter, `eCaptureProServer` stuck in `deactivating` for
minutes with leaked threads). Never fetch an output port's buffer on a cycle where you don't
intend to push it.

**`TaskParam` values only sync client→server, never the reverse** — a value set from *inside* a
plugin's own code (`SetValue()` called from within `Process()` or similar) never propagates back
to the client's later `GetParameter<T>().GetValue()` calls, confirmed by testing twice
independently (see `visss-data-acquisition-EVT/README.txt`'s `--maxframes` entry and
`record/README.txt`'s event-relay note). The only proven-live server→client channel in this
codebase is a `DataOutput`/`DataInput` port pair or a `FrameOutput`→`ImageDisplayTask` client
callback (both push-based), not a parameter read-back.

**Generic (non-typed) parameter access doesn't exist** — `Camera::GetParameter<T>(name)`/
`Task::GetParameter<T>(name)` need `T` known at compile time, and there's no documented
"get the type, then branch" path. The camera-config loader in `visss-data-acquisition-EVT/`
works around this by probing every known `CameraParamType` in turn until one's `GetParameter<T>()`
doesn't throw — see that component's README for the exact design.

`eSDK Pro`'s own internal log format (spdlog, statically linked into `libeSdkPro.so`) is not
reconfigurable — confirmed via binary inspection, not just "no header found for it." Only
verbosity is (`SetLogLevel()`/`ECAPTURE_PRO_LOGLEVEL`). Don't spend time trying to make SDK-internal
log lines match this codebase's own format; they're a different, fixed format by design.

## Known gaps (as of 2026-08-10)

- `eCaptureProServer` runs as root (vendor default, no `User=`/capabilities in the systemd unit).
  An investigation into running it as an unprivileged user found real `RLIMIT_RTPRIO`/
  `RLIMIT_MEMLOCK`-class `EPERM` failures, fixed those specifically via a systemd drop-in, and the
  camera stream still failed to open identically afterward — root cause not found, all changes
  reverted, a support ticket is pending with Emergent Vision.
- `--resetDHCP` equivalent — unclear if eSDK Pro exposes this at all; never investigated.
- Digital I/O line-pulse config purpose (old config's `LineSelector`/`outputLineSource` etc.) —
  open question, never resolved with the project owner.
- Wiper `clean()` (Python launcher) is all-or-nothing across every camera behind one shared
  process — cleaning any one camera's lens briefly stops recording for all of them, since there's
  only one Start/Stop button per process. A real trade-off of the multi-camera-per-process design
  below, not a bug; worth revisiting (the wipe command itself doesn't need the process stopped)
  if it turns out to matter in practice.

**Multi-camera-per-server is a real, supported deployment shape, not a deferred edge case** — a
real deployment YAML defines two cameras behind one host (a combined leader+follower box), and the
client already auto-discovers/manages every camera on the servers it's given in one process. The
Python launcher (`EmergentInstrument`) creates **one instance per deployment**, not one per
camera — instantiating one per camera entry was a real, confirmed bug (two GUI columns racing to
open the same physical cameras, only one ever getting its `-c` config applied), fixed 2026-08-11.
`RecordTask`'s `_latest_0.*` symlinks include `DeviceId` in the name for the same reason (were
`{Name}_latest_0.*`, shared across every camera behind one process and provably colliding — now
`{Name}_{DeviceId}_latest_0.*`, see `record/README.txt`).
