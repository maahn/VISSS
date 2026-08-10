# eSDK Pro Reference (New SDK Capability Map)

Source: https://docs.emergentvisiontec.com/software/esdk-pro-programmers-guide-overview (fetched 2026-08-07), plus local example projects `plugin_local` and `record_nvenc`.

This doc summarizes what eSDK Pro can do and how its API is shaped, as a reference for mapping the old pipeline's processing steps onto it. It does not cover the classic (non-Pro) eSDK, which is a different, lower-level API — the two are not meant to be mixed in one project.

## 1. What eSDK Pro is

A high-level, client–server C++ SDK for building applications that capture and process images from Emergent Vision cameras across one or more servers. It sits on top of two acceleration frameworks:

- **FlexProc** — processing framework; runs tasks on CPU, GPU, FPGA, or cloud via a plugin model.
- **FlexTrans** — data movement framework; moves frames between GPUs/servers with GPUDirect/zero-copy (no host memory copies).

Where the old SDK likely required manual per-camera buffer/thread management, eSDK Pro abstracts that away: you build a **pipeline** (a graph of **tasks**) and call `Start()`/`Stop()`; concurrency, buffering, and camera synchronization are handled internally.

## 2. Core architecture

```
System (1 per process)
 └─ Server(s) (local or remote)
     └─ Camera(s)
 └─ Pipeline (1 global pipeline per System)
     └─ Task(s), connected via Output → Input ports
         CameraTask, FrameGenTask, PluginTask, NvencTask, RawSavingTask, ImageDisplayTask
```

- **System** — global entry point; owns servers and the one pipeline. `System::Create()` / `Destroy()`.
- **Server** — one machine (local in-process, or remote via `ConnectServer(ip)`). Owns cameras. `DiscoverCameras()`, `AddCamera()`.
- **Camera** — one physical camera. `Open()`/`Close()`, `StartStreaming()`/`StopStreaming()`, `GetParameter<T>(name)`.
- **Pipeline** — directed graph of tasks; data flows via typed ports. Cannot be modified while running. `Start()`, `Stop()`, `Reset()`.
- **Task** — unit of work, runs on its own thread, connected to other tasks via `Output`/`Input` ports. Built-in task types cover camera capture, raw saving, NVENC encode, test-frame generation, preview display; custom processing goes in a **PluginTask**.
- **TaskWorker** (Plugin API) — backend implementation of a task; this is where you'd port old processing logic. Lifecycle: `Init()` → `Process()` (called repeatedly per input) → `Stop()` → `Deinit()`.

Local server = in-process, lowest latency, plugin compiled directly into the app (see `plugin_local` example). Remote server = separate machine/process (`eCaptureProServer`), plugin must ship as a `.dll`/`.so` loaded by that server.

## 3. Class reference

### System (`eSdkPro/system.h`)
`Create()` / `Destroy()`, `ConnectServer(ip[, port])`, `CreateLocalServer()`, `RemoveServer()`, `GetServers()`, `GetCameras()`, `GetPipeline()`, `SetPreviewQuality()`, `SetLogLevel()` (used in how-to guide, header not explicitly listed but confirmed via example usage in docs).

### Server (`eSdkPro/server.h`)
`DiscoverCameras()` → `CameraDiscoveryInfo[]`, `AddCamera(info)`, `AddMulticastSlaveCamera(info, master)`, `RemoveCamera()`, `GetCameras()`, `GetIp()`, `GetPort()`.

### Camera (`eSdkPro/camera.h`)
`GetDiscoveryInfo()` (serial, model, ip, subnet, interface name/ip, MAC), `Open(CameraOpenConfig)`, `Close()`, `StartStreaming()`/`StopStreaming()`, `StartImagePreview(callback)`/`StopImagePreview()`, `GetParameter<T>(name)`.

`CameraOpenConfig`: CPU core pin, GPU device id (-1 = no GPU), `m_gpuDirectEnabled`, stream buffer count (auto or manual), multicast destination ip/port.

### Camera parameters (`eSdkPro/cameraparam.h`)
Base `CameraParam` (`GetName()`, `GetType()`), typed subclasses: `BoolCameraParam`, `CommandCameraParam` (`Execute()` — for trigger-style actions like `TriggerSoftware`), `EnumCameraParam` (`GetValue/SetValue(string)`, `GetRange()`), `FloatCameraParam`, `Int32CameraParam`, `UInt32CameraParam` (all with `GetValue/SetValue/GetMin/GetMax`, integer types also `GetInc()`), `StringCameraParam`, `RegisterCameraParam`.

Parameters are looked up by string name (e.g. `"Exposure"`, `"Width"`, `"Height"`, `"FrameRate"`, `"PixelFormat"`, `"TriggerMode"`, `"TriggerSource"`, `"TriggerActivation"`, `"AcquisitionMode"`, `"AcquisitionFrameCount"`, `"PtpMode"`, `"PtpStatus"`, `"DeviceUserName"`) — these are GenICam-style feature names, model-specific, not enumerated in the API docs itself (need the camera's feature reference, or discovery via eCapture Pro's GUI/parameter browser).

### Pipeline (`eSdkPro/pipeline.h`)
Task factory + graph builder:
- `CreateCameraTask(camera)`
- `CreatePluginTask(server, pluginName)`
- `CreateRawSavingTask(server, baseRecordingPath)`
- `CreateNvencTask(server, NvencTask::InitParams)`
- `CreateFrameGenTask(server, FrameGenTask::InitParams)` — synthetic test frames, useful for testing the port without hardware
- `CreateImageDisplayTask(server, previewCallback)`
- `DeleteTask()`, `ConnectTasks(output, input)`, `DisconnectTasks()`, `Reset()`, `Start()`, `Stop()`, `SetPtpSyncMode(bool)`
- `ConnectTasksFlexTrans(...)` — GPU-to-GPU or NIC-to-NIC zero-copy connection (Linux only, mentioned in how-to guide, not on the formal class page — worth confirming signature against the SDK headers directly)

### Ports (`eSdkPro/port.h`)
`Port` base (`GetName()`, `GetType()`, `GetPlatform()` — HWPlatform, e.g. Host vs GPU). `Input`/`Output` are typed markers with no extra public methods at the application level (the interesting methods are on the Plugin-side `FrameInput`/`FrameOutput`, see §4).

### Tasks (`eSdkPro/task.h`)
Base `Task`: `SetLabel/GetLabel`, `SetCpuCore/GetCpuCore`, `SetGpuDeviceId/GetGpuDeviceId`, `GetParameter<T>(name)` (task parameters, distinct from camera parameters).

- `CameraTask` — `GetOutput()`. Wraps a `Camera` for pipeline use.
- `FrameGenTask` — synthetic frame source. `InitParams`: width, height, pixel format, framerate, hex color, fading step. `PixelFormat` enum: `BayerRG8`, `Mono8`, `RGB8`, `BGR8`.
- `PluginTask` — `GetInput(name)`, `GetOutput(name)`. Named ports (a plugin can expose multiple inputs/outputs, unlike the single-port built-in tasks).
- `NvencTask` — `InitParams`: gpu id, base recording path, width/height/pixelFormat/framerate, `CodecType` (`H264_AVC`/`H265_HEVC`), bitrate Kbps. `GetInput()`, `GetOutput()` (encoded stream), `GetLastRecordingPath()`.
- `RawSavingTask` — `GetInput()`, `SetMultiRecordingPaths()` (up to 4 paths, round-robin, for bandwidth), `SetDirectIO(bool)` (default on, needs 512-byte-aligned frame size), `GetLastRecordingPath()`.
- `ImageDisplayTask` — `GetInput()`, downsampled preview frames.

### Task parameters (`eSdkPro/taskparam.h`)
Same shape as camera parameters but for tasks: `TaskParam` base + `BoolTaskParam`, `EnumTaskParam` (index-based: `GetOptions()`, `GetIdx/SetIdx`), `FloatTaskParam`, `Int32TaskParam`, `StringTaskParam`.

### Frame (`eSdkPro/frame.h`)
Three constructors: empty, allocate-new (`width, height, format, platform`), wrap-existing-pointer (`width, height, format, stride, dataPtr, dataSize`). Accessors: `GetWidth/GetHeight/GetPixelFormat/GetStride/GetDataPtr/GetDataSize`, `GetFrameId/SetFrameId` (incrementing frame counter — gaps indicate dropped frames), `GetTimestampNs/SetTimestampNs` (GVSP timestamp converted to ns, or PTP-master-epoch-based if PTP is on).

### Error handling (`eSdkPro/errors.h`)
`ErrorCode` enum: `Success`, `General`, `NotFound`, `Invalid` (coarse-grained compared to what a lower-level SDK might expose). `ESdkProException` (`what()`, `GetErrorCode()`) — thrown, not returned; wrap SDK calls in try/catch (see both local examples).

## 4. Plugin API — where old processing logic gets ported to

Namespace `eSdkPro::Plugin`. This is the extension point for custom processing (equivalent to "the code you actually port," as opposed to SDK plumbing).

### TaskWorker (`eSdkPro/plugin/taskworker.h`)
Subclass this. Lifecycle called by the pipeline:
1. Constructor — call `SetName()`, create ports (`CreateFrameInput/Output`, `CreateDataInput/Output`) and parameters (`CreateParameter<T>()`).
2. `Init()` (virtual, returns bool) — allocate resources. Called once when pipeline starts.
3. `Start()` (virtual) — called right before processing begins.
4. `Process()` (**pure virtual**, returns bool) — called once per available input, on a dedicated thread. This is the per-frame processing hook — the direct analog of whatever "process one frame" function exists in the old code.
5. `Stop()` (virtual) — called after processing ends.
6. `Deinit()` (virtual) — release resources; guard against double-release if also called from destructor.

Also: `GetParameter<T>()`, `Abort(msg)` (aborts task + stops pipeline), `LogMessage(level, msg)` (warns against excessive logging — congests the server).

Register with `RegisterTaskPlugin<YourTaskWorker>()` (called once, e.g. in `main()`), then instantiate via `Pipeline::CreatePluginTask(server, name)`.

### Plugin task parameters (`eSdkPro/plugin/taskparam.h`)
Same 5 types as application-side task parameters, created inside the TaskWorker constructor via `CreateParameter<T>(name)`, optionally with `SetToolTip()`. The app can then read/set these at runtime via `Task::GetParameter<T>()`.

### Plugin Input (`eSdkPro/plugin/port.h`)
- `FrameInput`: `GetFrame()` (frame valid only until `Process()` returns) or `GetFrameAsync()` + matching `ReleaseAsyncFrame()` (frame stays valid past `Process()` — needed if you hand the frame to another thread/queue).
- `DataInput`: same sync/async pattern (`GetData()` / `GetDataAsync()` + `ReleaseAsyncData()`) for generic (non-frame) data — e.g. passing an anomaly-detection flag downstream, as shown in the PCB inspection example.

### Plugin Output (`eSdkPro/plugin/port.h`)
- `FrameOutput`: simple `PushFrame(frame)`, or a reusable-buffer pattern for performance: `RegisterOutputFrame()` / `DeregisterOutputFrame()` / `QueueOutputFrame()` / `GetOutputFrame()` (blocks until available).
- `DataOutput`: mirror API for generic data (`PushData`, `Register/DeregisterOutputData`, `QueueOutputData`, `GetOutputData`).

Generic (non-frame) `Data`/`DataInput`/`DataOutput` ports were added in eCapture Pro 1.5.0 — worth checking the installed SDK version supports this if the old pipeline needs to pass non-image data (metadata, detection results, etc.) between stages.

## 5. How-to patterns confirmed from docs (usable as porting templates)

- **Discover/connect cameras**: `server.DiscoverCameras()` → `server.AddCamera(info)` → read params like `DeviceUserName`.
- **Set a camera parameter**: `cam.GetParameter<UInt32CameraParam>("Exposure").SetValue(...)`.
- **Software trigger**: set `TriggerMode=On`, `TriggerSource=Software`, then `Execute()` a `CommandCameraParam("TriggerSoftware")`.
- **Hardware trigger**: `TriggerMode=On`, `TriggerSource=GPI_n`, `TriggerActivation=Rising_Edge`, `AcquisitionMode`, `AcquisitionFrameCount`. Note: `FrameRate` param is ignored when `AcquisitionFrameCount=1`.
- **PTP multi-camera sync**: per-camera `PtpMode=TwoStep`, plus `pipeline.SetPtpSyncMode(true)` — pipeline refuses to start if any camera fails to sync.
- **Insert custom processing**: `CreatePluginTask(server, "Name")` between a `CameraTask` and a sink task (`RawSavingTask` in the example), wired with `ConnectTasks()`.
- **GPU-to-GPU / NIC-to-NIC zero-copy**: `ConnectTasksFlexTrans()` (Linux-only, needs GPUDirect or Rivermax respectively) — relevant only if the old pipeline does cross-device/cross-server transfer.
- **Save raw / NVENC-compress**: `CreateRawSavingTask` / `CreateNvencTask`, fed from camera params (width/height/framerate/pixelFormat) — matches what `record_nvenc` example already does end to end.
- **Start/stop/reset lifecycle**: pipeline can only be edited while stopped; `Stop()` drains in-flight frames before returning.
- **Error handling**: `ESdkProException` around `pipeline.Start()`; on catch, `Stop()` (safe if already stopped) then `Reset()` before rebuilding.
- **Logging**: `system.SetLogLevel(LogLevel::Debug|Info|Error)`.

## 6. Example projects (eSDK Pro examples, installed alongside the SDK)

Default install location: `C:\Program Files\EVT\eCapturePro\eSdkPro\examples` (Windows) / `/opt/EVT/eCapturePro/eSdkPro/examples` (Linux).

| Example | Purpose | Have locally? |
|---|---|---|
| `server_connect` | Connect + list cameras (recommended first run) | No |
| `record_raw` | Save raw frames to disk | No |
| `record_nvenc` | NVENC-compressed recording | **Yes** (`record_nvenc/`, 264 lines) |
| `plugin_local` | Custom plugin task, in-process | **Yes** (`plugin_local/`, 94 lines) |
| `plugin_remote` | Custom plugin task, remote server (compiled as .dll/.so) | No |
| `cuda_brightness` | GPU frame processing plugin (brightness) | **Yes** (`cuda_brightness/`, ~300 lines across 4 files) |
| `cpu_brightness` | CPU equivalent of the above | No |
| `multicast` | Stream one camera to multiple servers | No |

Total example code across all 8 projects is presumably closer to the ~3000 lines you mentioned — the three connected here account for ~650 lines combined. If the remaining ones are relevant to your old pipeline's processing steps (`cpu_brightness` and `cuda_draw_rect` as further plugin-authoring templates, `record_raw`/`server_connect` as minimal-boilerplate references), it'd help to pull those into a connected folder too.

## 7a. Real processing-plugin pattern (from `cuda_brightness`)

This is the template that matters most for porting actual image-processing logic — it's a full `TaskWorker` doing GPU-side per-pixel work, not the pass-through stub in `plugin_local`.

**Structure** (`CudaBrightnessTask : eSdkPro::Plugin::TaskWorker`):

- **Constructor** — `SetName()`, `CreateFrameInput(name, HWPlatform::Cuda)`, `CreateFrameOutput(name, HWPlatform::Cuda)` (note the platform is `Cuda`, not `Host` — frames stay in GPU memory throughout), `CreateParameter<FloatTaskParam>()` for a user-adjustable "Brightness Factor" with `SetToolTip()`.
- **`Init()`** — pre-registers a placeholder (empty) output `Frame` via `RegisterOutputFrame()` + `QueueOutputFrame()`. Real dimensions get set lazily on first real frame.
- **`Process()`**:
  1. `m_input.GetFrame()` (sync, frame only valid until `Process()` returns — no need for the async variant here since everything happens within this call).
  2. Validate `GetPixelFormat()` against a switch of supported GVSP formats; unsupported formats get logged (`LogMessage(LogLevel::Error, ...)`) and `Process()` returns `false` rather than throwing.
  3. Get the previously registered output frame via `GetOutputFrame()`; if its dimensions/format don't match the new input, `DeregisterOutputFrame()` the old one, allocate a new `Frame(width, height, format, HWPlatform::Cuda)`, and `RegisterOutputFrame()` it. This buffer-reuse pattern avoids reallocating a GPU buffer every frame unless the frame size actually changes.
  4. Read the current parameter value (`m_brightnessParam.GetValue()`).
  5. Call out to a separate `.cu`/`.cuh` file's plain C++ function (`launchBrightenKernel(inputFrame, outputFrame, factor)`) — the CUDA kernel launch itself is kept out of the TaskWorker class, in its own translation unit compiled by `nvcc`.
  6. Propagate `SetFrameId()` from input to output (preserves frame correlation/ordering downstream) and `PushFrame()` the result.
  7. Both `ESdkProException` and generic `std::exception` are caught around the whole body and turned into `Abort(e.what())` + `return false` — this is the per-task error-handling convention, distinct from the app-level try/catch around `pipeline.Start()`.

**Kernel-side pattern** (`kernel.cu`): a `__global__` 2D kernel with one CUDA thread per pixel (`dim3 threadsPerBlock(32,32)`), templated on a per-pixel processing function so the same launch code handles mono/RGB/packed 10-/12-bit formats via different template instantiations. Frame data is accessed via `frame.GetDataPtr()` — already a GPU pointer since the port platform is `Cuda`. After the kernel launch, `cudaDeviceSynchronize()` + `cudaGetLastError()` convert CUDA errors into an `ESdkProException`.

**Registration**: a static file-scope object (`RegisterTaskFactory`) whose constructor calls `RegisterTaskPlugin<CudaBrightnessTask>()` — this runs automatically when the plugin shared library is loaded (relevant for `plugin_remote`-style deployment where the plugin isn't compiled directly into the app).

**Build**: separate `CMakeLists.txt` in `src/` adds both the `.cpp` and `.cu` files as sources for the target — confirms CUDA sources compile as part of the same target as regular C++ files (via CMake's CUDA language support), not a separate static lib.

This example resolves the "no template for a real processing plugin" gap noted below — if the old pipeline does per-pixel or per-frame GPU work, this is the shape to follow: `Init()` for lazy buffer setup, format-switch validation, buffer-reuse via register/deregister, external kernel launch function, frame-id propagation, and the try/catch → `Abort()` convention.

## 7. Open questions / gaps to resolve during the mapping phase

- **Camera parameter names are not enumerated in the API docs** — they're GenICam feature names specific to your camera model. Need the camera's feature reference (or eCapture Pro's live parameter browser) to know what's available for exposure/gain/ROI/binning/etc.
- **`ConnectTasksFlexTrans()`** appears only in the how-to guide, not the formal Pipeline class reference — confirm exact signature against the installed SDK headers before relying on it.
- **`System::SetLogLevel()` / `LogLevel` enum** — used in the how-to guide but not documented on the System class page; confirm available levels.
- `plugin_local` and `record_nvenc` are single-camera-loop, fixed 3-second or CLI-duration runs with no dynamic reconfiguration and no error recovery beyond top-level try/catch. `cuda_brightness` (now reviewed, see §7a) fills the "real processing plugin" gap — it's a solid template for per-pixel GPU work specifically.
- Still no local example demonstrates: multi-camera pipelines beyond "one task per camera," dynamic parameter changes at runtime while the pipeline is running, the `Data`/`DataInput`/`DataOutput` generic (non-frame) ports, `ImageDisplayTask`/preview usage, or CPU-side (non-CUDA) processing (`cpu_brightness` would show whether `HWPlatform::Host` processing follows the same buffer-reuse pattern or something simpler).
