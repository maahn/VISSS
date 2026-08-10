# VISSS Pipeline → eSDK Pro Mapping & Gap List

Cross-references `PROCESSING_SPEC.md` (old Teledyne/GigE-V pipeline, SDK-agnostic spec) against
`new-sdk-reference.md` (eSDK Pro capability map) to determine, stage by stage, what maps
cleanly, what needs custom code, and what's genuinely unresolved before implementation starts.

## 1. Architectural findings that change the shape of the port

These aren't per-stage mappings — they're structural differences between how the old pipeline
and eSDK Pro are built, and they simplify (or complicate) large chunks of the spec at once.

**Finding A — the producer/consumer thread split (spec §4) is mostly free.**
The old pipeline hand-builds a capture thread + N storage-worker threads + bounded
`frame_queue`s with mutex/condvar, specifically so the real-time capture thread never blocks on
encode/disk I/O. In eSDK Pro, `CameraTask` and each `PluginTask` already run on their own
dedicated thread (`Task` docs: "runs in its own thread"), connected via ports that carry frames
between threads. A `CameraTask → PluginTask` pipeline **is** a producer/consumer split with
built-in queuing — you don't need to build `frame_queue.h`'s equivalent from scratch. What's
*not* confirmed: queue depth, and backpressure behavior on overflow (block vs. drop) — see gap
list §3.

**Finding B — §3.15's round-robin thread-splitting should not be ported at all.**
The spec already confirms this with the project owner: it was a CPU-bottleneck workaround, and
GPU-resident processing removes the reason for it. This isn't an eSDK Pro-specific finding, but
it means the target architecture is **one sequential motion-detection stream**, not N — which
also sidesteps needing N independent `PluginTask`s or manual queue-index arithmetic.

**Finding C — PTP handling is very likely a straight simplification, not a straight port.**
§3.5's 30-second manual poll loop against a Teledyne-specific `"ptpStatus"` string exists because
the old SDK gives you nothing better than "read this string yourself." eSDK Pro has
`EnumCameraParam("PtpStatus")` (seen in `record_nvenc`'s pattern) *and* `Pipeline::SetPtpSyncMode
(true)`, which the docs describe as: pipeline "will not start if any camera cannot be
synchronized." That's the SDK doing §3.5's job internally. Open question (gap list §1): does
`Start()` block/retry until synced, or fail immediately if not yet synced at call time? If the
latter, you still need a thin retry loop around `Start()`, but it's a few lines, not a manual
per-second status poll.

**Finding D — the frame-ID rollover stage (§3.11) may not exist in the new pipeline.**
The old 65535 wraparound is called out in the spec as device-specific ("for m1280 camera").
`Frame::GetFrameId()` in eSDK Pro is documented only as "unique incrementing id... may be used
as a frame counter... gaps indicate dropped frames," with no mention of a wrap boundary. If
`FrameId_t` is a wide (e.g. 64-bit) counter, §3.11 is dead code in the port. **Needs
confirmation from the actual SDK headers** (`FrameId_t` typedef) before deciding — see gap list.

**Finding E — timestamp mode (§3.10) likely collapses to PTP-only.**
`Frame::GetTimestampNs()` is documented as already producing epoch-based nanoseconds when PTP is
active ("timestamp will be based off of the epoch of the master clock"), and camera-tick-scaled
nanoseconds otherwise. That means eSDK Pro is already doing the unit conversion the old code does
by hand (µs vs. ns, tick-frequency scaling). The spec itself says the no-PTP fallback path "may
not need porting at all" if the new hardware's PTP is reliable — combined with Finding C, the
recommendation is: **build for PTP-only, drop the `timestampControlReset`/jump-detection
machinery (§3.10 no-PTP branch, §3.17) unless a concrete reason to keep it surfaces.**

**Finding F — the biggest real architecture decision is §3.22/§3.23 (file naming vs. NvencTask).**
eSDK Pro's built-in `NvencTask` does exactly what §3.23 asks for (GPU-resident H.264/H.265
encode, no CPU roundtrip) but owns its own output path/subdirectory/timestamp naming convention
(`baseRecordingPath` + auto-created subdirectories, `GetLastRecordingPath()`). It does **not**
know about VISSSlib's specific staging→final rename, `_latest` symlink-via-atomic-rename
convention, or per-file metadata `.txt` sidecar. Two real options, not resolved by either
reference doc:
  1. Use `NvencTask` as-is and adapt VISSSlib's ingestion to eSDK Pro's own naming/staging
     scheme (changes a downstream, out-of-repo consumer).
  2. Bypass `NvencTask` and drive NVENC/Video Codec SDK directly from a custom `PluginTask`,
     keeping full control of naming/staging/symlinks/metadata pairing exactly as today.
  This is a decision to make explicitly, not something to default silently — flagged in gap
  list §2.

**Finding G — §3.25 (live preview) has a ready-made SDK replacement, optional.**
`Camera::StartImagePreview(callback)` / `System::SetPreviewQuality()` / `Pipeline::
CreateImageDisplayTask()` do roughly what the manual `cv::imshow` + 0.4x downscale does today.
Not required, but worth adopting instead of reimplementing decimation/downscale by hand.

## 2. Stage-by-stage mapping

Spec tags carried over: **[SDK]** / **[LOGIC]** / **[MIXED]**.

| Spec § | Stage | eSDK Pro mapping |
|---|---|---|
| 3.1 | Camera discovery & connect **[SDK]** | `Server::DiscoverCameras()` → `Server::AddCamera(info)` → `Camera::Open(CameraOpenConfig)`. Direct replacement; `CameraOpenConfig` also subsumes 3.6's buffer setup and GPUDirect enablement in one struct. |
| 3.2 | Feature config from file **[MIXED, core feature]** | No direct equivalent to GenICam's generic `FromString()`. eSDK Pro's `Camera::GetParameter<T>(name)` is templated on the *caller* knowing the parameter type `T` ahead of time — there's no documented "get an untyped `CameraParam` by name, then branch on `GetType()`" path in the reference material gathered so far. **This blocks a literal port of the config-file loader and needs resolving against the actual SDK headers** — see gap list §4, this is likely the single highest-priority technical unknown given the spec confirms this is a core feature, not disposable plumbing. |
| 3.3 | `--resetDHCP` **[SDK]** | Not covered by the eSDK Pro API pages reviewed (`SetIp`-style network config functions were only seen under the *classic* eSDK's "Network IP Configuration Functions," and the docs explicitly say never mix eSDK Pro and classic eSDK in one project). Needs confirmation whether eSDK Pro exposes DHCP/IP reconfig at all, or whether this becomes a one-off classic-eSDK or vendor-tool utility kept outside the main app. |
| 3.4 | TurboDrive check **[SDK]** | Confirmed drop (project owner, Teledyne-specific). No eSDK Pro equivalent needed. |
| 3.5 | PTP wait **[MIXED]** | `EnumCameraParam("PtpStatus")` + `Pipeline::SetPtpSyncMode(true)`. See Finding C. Camera-side PTP *role* config (`ptpMode Slave` in the old config file, §3.2's example) maps to `EnumCameraParam("PtpMode").SetValue("TwoStep")` per the how-to guide's sync example — confirm exact enum values/semantics match "Slave" role, not just Two-Step vs. One-Step protocol variant (these may be orthogonal settings, not the same axis — worth checking against the camera's actual feature list). |
| 3.6 | Buffer allocation & transfer init **[SDK]** | `CameraOpenConfig`: `m_numStreamBuffers`/`m_autoNumStreamBuffers` (≈ old `NUM_BUF=8`), `m_gpuDeviceId`/`m_gpuDirectEnabled` (this *is* the GPUDirect path the port wants), `m_streamCpuCore`. The old code's manual heartbeat-timeout/packet-pacing/CPU-affinity transport tuning (§3.6 second paragraph) has no visible per-parameter equivalent in the docs reviewed — likely internal to eSDK Pro's transport layer now, not exposed. Not necessarily a gap (may just not need tuning anymore) but unconfirmed. |
| 3.7 | Capture thread priority **[LOGIC — timing constraint]** | `Task::SetCpuCore()`/`SetGpuDeviceId()` cover CPU/GPU affinity. **No documented `SCHED_RR`-equivalent priority control** on `Task` or `CameraTask`. Given the SDK's stated goal ("simplified concurrency... handles multi-threading and synchronization for you"), it's plausible capture-thread prioritization is handled internally — but this is an assumption, not a confirmed fact. See gap list §5. |
| 3.8 | Frame wait + error/timeout handling **[SDK/LOGIC]** | No `GevWaitForNextImage`-style polling call is exposed to application code — frames arrive by invoking `PluginTask`'s `Process()` when available (push model, not pull-with-timeout). This changes the error model: the old code distinguishes "frame delivered with error status" from "timeout" from "success" via a single call's return value; `Frame` in eSDK Pro exposes no visible status/error field, only id/timestamp/dimensions/data. **Open question**: are corrupt/error frames silently filtered out before reaching `Process()` (leaving only gaps in `GetFrameId()` as evidence, matching 3.11/3.12's own detection mechanism), or does `Process()` need its own explicit bad-frame handling? Timeout/stall detection (the `n_timeouts`/`max_n_timeouts`/follower-mode logic) has no polling-timeout equivalent to hook into — would need to be reimplemented as a watchdog (e.g., a timer thread checking time-since-last-`Process()`) inside the custom `TaskWorker`. Buffer release: `FrameInput::GetFrame()` auto-invalidates after `Process()` returns — direct equivalent of unconditional `GevReleaseImage()`, one less thing to get wrong. |
| 3.9 | Startup warm-up skip **[LOGIC]** | No SDK involvement — a simple frame counter inside the custom `TaskWorker::Process()`, skip-but-still-consume the first N frames. Direct port. |
| 3.10 | Timestamp reconciliation **[LOGIC]** | See Finding E. `Frame::GetTimestampNs()` likely replaces the entire PTP-mode branch outright. Recommend building PTP-only and dropping the no-PTP branch unless a concrete need surfaces (gap list §6 tracks the decision). |
| 3.11 | Frame-ID rollover **[LOGIC, device-specific]** | See Finding D — likely dead code if `FrameId_t` doesn't wrap at a small boundary. Confirm counter width before deciding whether to port. |
| 3.12 | Missed-frame diagnostic **[LOGIC, diagnostic only]** | Direct port: compare consecutive `Frame::GetFrameId()`, log gaps. Apply the spec's own suggested fix (seed `last_id` from the first observed frame, not 0) while porting — no reason to carry the old false-positive forward. |
| 3.13 | New-file/housekeeping trigger **[LOGIC]** | The `timestamp_s % new_file_interval` trigger itself is pure app logic, ports as-is. The camera-status refresh it also does (temperature, PTP status, *transfer diagnostics*) is partly gap territory: temperature is presumably some `FloatCameraParam`/`UInt32CameraParam` (GenICam feature name TBD from the camera's own feature list — same open item as camera parameter names generally, see the SDK reference doc's own open questions). Teledyne-specific transport stats (`transferQueueCurrentBlockCount`, `transferMaxBlockSize`) have no visible eSDK Pro equivalent in the docs reviewed — likely drop from the metadata header (§3.24) or find whatever transport diagnostics eSDK Pro does expose, if any. |
| 3.14 | Zero-copy buffer wrap **[SDK format]** | `Frame::GetDataPtr()/GetWidth()/GetHeight()/GetPixelFormat()/GetStride()` replace the manual `cv::Mat` wrap. With GPUDirect the data is already GPU-resident (`HWPlatform::Cuda`), so there's no CPU-side `cv::Mat` to construct at all for the processing path — this stage effectively disappears, replaced by whatever GPU-side representation the motion-detection kernel consumes directly from `Frame::GetDataPtr()` (same pattern as `cuda_brightness`, see new-sdk-reference.md §7a). Mono8 pixel format assumption carries over cleanly — `GVSP_PIX_MONO8` is a supported/handled format in the `cuda_brightness` kernel switch, confirming the new SDK's pixel-format vocabulary includes it. `frameborder=64` padding: allocate the output `Frame` at `(width, height+64, format, HWPlatform::Cuda)` up front, same idea as today, just via `Frame`'s allocating constructor instead of `cv::copyMakeBorder`'s implicit allocation. |
| 3.15 | Round-robin thread distribution **[LOGIC, confirmed not to port]** | See Finding B. Target: one `PluginTask` doing motion detection on the full-rate sequential stream. If encode-throughput parallelism is still wanted later, add it as a separate downstream concern, not by fragmenting the motion-detection input. |
| 3.16 | Gain/exposure query **[LOGIC, metadata-only]** | `cam.GetParameter<FloatCameraParam>("ExposureTime")`/`Gain` — direct port, matches the how-to guide's own exposure example. |
| 3.17 | Frame drop during clock-reset **[LOGIC edge case]** | Conditionally obsolete — only relevant if the no-PTP branch (3.10) survives. Default recommendation: drop along with 3.10's no-PTP path. |
| 3.18 | Motion detection core algorithm **[LOGIC — preserve exactly]** | Pure algorithm, not an eSDK Pro concern at all — this is an OpenCV-CUDA (or custom-kernel) porting question, same category as the `cuda_brightness` kernel. `cv::cuda::absdiff` exists and should be a direct swap for `cv::absdiff`. **`cv::cuda::calcHist`'s support for the old code's non-uniform 7-bin histogram (bin edges `20,30,40,60,80,100,120,256`, not equal-width bins) is unconfirmed** — OpenCV's CUDA histogram functions are typically uniform-bin-only; if so, this needs a custom reduction kernel (structurally similar to `cuda_brightness`'s per-pixel kernel, but accumulating counts instead of writing pixels) rather than a library call. Flagged in the spec's own open-questions (§9.8, alongside `putText`) — recommend treating both as one combined "OpenCV-CUDA has gaps for this pipeline's exact needs" research item. `imgOld`-equivalent (previous-frame state between `Process()` calls) maps directly onto keeping a member `Frame` in the custom `TaskWorker`, same pattern `cuda_brightness` already uses for its reusable output buffer. |
| 3.19 | Rotation **[LOGIC, optional]** | `cv::cuda::rotate` or a small custom kernel; low-risk, same treatment as 3.18. |
| 3.20 | Text overlay & border **[LOGIC/formatting]** | Not an eSDK Pro concern — the spec's own open question (§9.8: `cv::cuda` has no `putText`) stands regardless of camera SDK. `Frame`'s allocating constructor covers the border-padding allocation (see 3.14); the text-rendering method itself (NPP, custom kernel, or CPU roundtrip for just the border strip) is a decision independent of this mapping. |
| 3.21 | Recording decision **[LOGIC]** | Pure app logic (`writeallframes \|\| movingPixel \|\| firstImage \|\| newFile \|\| statusFrame`), no SDK dependency, direct port into whichever `TaskWorker` produces the final frame. |
| 3.22 | File rollover & naming **[LOGIC/output format]** | See Finding F — depends entirely on the `NvencTask`-vs-custom-encode decision. If custom encode is chosen, this ports essentially unchanged (it's just file I/O). If `NvencTask` is adopted, this stage's naming scheme needs to be reconciled with `NvencTask`'s own path/subdirectory conventions and `GetLastRecordingPath()`. |
| 3.23 | Video encoding **[SDK-plumbing-equivalent]** | `Pipeline::CreateNvencTask()` does exactly the GPU-resident-encode job this stage wants (confirmed working end-to-end in the connected `record_nvenc` example). The open question is purely Finding F's naming/staging conflict, not encoding capability itself. |
| 3.24 | Metadata file writing **[LOGIC/output format]** | Fully custom app logic (CSV + header), independent of camera SDK choice — the only eSDK Pro-relevant inputs are `Frame::GetFrameId()`/`GetTimestampNs()` (both direct replacements for the old `id`/`timestamp` fields) and whatever camera-status parameters get resolved from the 3.13 gap (temperature, PTP status — `EnumCameraParam("PtpStatus").GetValue()` covers that one directly). |
| 3.25 | Live preview **[auxiliary]** | See Finding G — `Camera::StartImagePreview()`/`ImageDisplayTask` are ready-made replacements, adoption optional. |
| 3.26 | Shutdown & cleanup **[SDK]** | `Pipeline::Stop()` ("stops all cameras and waits until all frames have been processed before returning") directly satisfies the spec's "flush and finalize the currently-open file for each worker before exiting" requirement — put per-file close logic in the custom `TaskWorker`'s `Stop()`/`Deinit()`. Then `System::Destroy()` for final teardown. Substantially simpler than the old `GevStopTransfer`/`GevAbortTransfer`/`GevFreeTransfer`/`GevCloseCamera`/`GevApiUninitialize` sequence. |

## 3. Proposed pipeline architecture (draft, pending gap resolution)

Based on the mapping above, a first-pass task decomposition:

1. **`CameraTask`** (built-in) — capture, replaces §3.1/§3.6/§3.8's SDK half. `CameraOpenConfig`
   set for GPUDirect (`m_gpuDirectEnabled=true`, `m_gpuDeviceId=<n>`).
2. **Custom `PluginTask` — "MotionDetectTask"** — one instance, full frame rate (Finding B).
   Owns: warm-up skip (§3.9), ID/timestamp bookkeeping (§3.11–3.12, pending Finding D/E),
   housekeeping-trigger logic (§3.13's non-camera-status half), motion detection (§3.18),
   rotation (§3.19), overlay/border (§3.20), recording decision (§3.21). Outputs a `FrameOutput`
   (the bordered/overlaid frame, when recording) plus a `DataOutput` carrying per-frame metadata
   (histogram bins, ids, timestamps, moving flag) for the metadata sink.
3. **Sink stage(s) — depends on Finding F's resolution:**
   - Option 1: `NvencTask` (built-in) fed from MotionDetectTask's `FrameOutput`, plus a small
     custom `PluginTask` (`DataInput`-only) consuming the `DataOutput` to write the `.txt`
     metadata sidecar, with app-level code reconciling `NvencTask`'s file naming to VISSSlib's
     expected scheme.
   - Option 2: one custom `PluginTask` ("EncodeAndWriteTask") owning NVENC/Video Codec SDK
     directly, full control over staging/final/symlink naming and metadata pairing exactly as
     today — more implementation work, no reconciliation needed.
4. **Optional: `ImageDisplayTask`** or `Camera::StartImagePreview()` for §3.25, replacing the
   manual `cv::imshow` decimation.

This decomposition assumes PTP-only timestamping (Finding E) and drops §3.15's thread-splitting
and §3.17's clock-reset-drop edge case per Findings B/E. It should be revisited once the gap list
below is resolved, particularly #4 (generic parameter setter) and Finding F (encode ownership),
since both affect the shape of "MotionDetectTask" and whether a fourth task is needed.

## 4. Consolidated gap list (blocking → informational)

Combines new gaps found in this mapping pass with the spec's own still-open items (§9.8–9.9),
which remain unresolved regardless of SDK choice.

1. **PTP `Start()` blocking behavior** (Finding C) — does `Pipeline::Start()` block/retry until
   camera PTP-lock, or fail immediately if not yet locked? Determines whether any retry loop is
   still needed around `Start()`.
2. **Encode ownership: `NvencTask` vs. custom NVENC integration** (Finding F) — the single
   largest architecture decision in the port; affects task count, file-naming logic ownership,
   and how much of §3.22 survives as-is. Recommend deciding this before writing any pipeline
   code, not discovering it mid-implementation.
3. **Port queue depth & backpressure behavior** (Finding A) — old code drops-on-overflow at 3000
   frames rather than blocking the producer. Unconfirmed whether eSDK Pro's inter-task port
   queuing blocks, drops, or is unbounded. Matters for whether the "capture thread must never
   block" real-time constraint (spec §4) still holds automatically or needs explicit handling.
4. **Generic (runtime-typed) camera parameter setter** — blocks a literal port of §3.2's
   config-file loader, which the spec confirms is a core feature. `GetParameter<T>()` requires
   knowing `T` at the call site; no documented way to fetch/set a parameter when only a runtime
   string name + string value are known (as the old `fscanf`-based loader does). Needs resolving
   against actual SDK headers — possibly via `CameraParamType`/`GetType()` dispatch on some
   less-templated accessor not surfaced in the doc pages reviewed.
5. **Capture-thread scheduling priority** (§3.7) — no documented `SCHED_RR`-equivalent on `Task`.
   Unclear whether eSDK Pro handles this internally (plausible, given its "simplified
   concurrency" positioning) or whether external OS-level tuning is still required.
6. **No-PTP timestamp path: port or drop?** (Finding E) — recommend dropping per the spec's own
   suggestion, contingent on confirming the new camera's PTP reliability in practice. Not a
   technical blocker, but a decision that should be made explicitly rather than by default.
7. **Frame-ID counter width** (Finding D) — determines whether §3.11's rollover-compensation
   logic is needed at all. Check `FrameId_t`'s underlying type in the SDK headers.
8. **Bad-frame / dropped-frame signaling at the `Process()` level** (§3.8) — old code
   distinguishes error-status frames from timeouts from successes explicitly; unclear whether
   eSDK Pro's `Frame` surfaces any error/status info or whether bad frames are filtered before
   `Process()` is ever called, leaving only `GetFrameId()` gaps as evidence.
9. **`--resetDHCP` equivalent** (§3.3) — unclear whether eSDK Pro exposes IP/DHCP reconfiguration
   at all, given it appeared only under the classic eSDK's function list in the docs reviewed.
10. **Camera status/transport diagnostics for metadata header** (§3.13/§3.24) — temperature
    likely available as a named camera parameter (name TBD from the camera's own GenICam feature
    list — same open item already noted in new-sdk-reference.md); Teledyne-specific transport
    stats (`transferQueueCurrentBlockCount`, `transferMaxBlockSize`) have no obvious eSDK Pro
    equivalent and may need to be dropped from the metadata header format (a version-bump-worthy
    change per the spec's own metadata-format rules, §3.24).
11. **PTP role config semantics** (§3.5/§3.2) — confirm `EnumCameraParam("PtpMode")`'s values
    (`TwoStep` seen in the how-to guide) actually correspond to the old config's `ptpMode Slave`
    concept, or whether "Slave" is a separate parameter/axis on the new camera.
12. **`cv::cuda` gaps: non-uniform `calcHist` and `putText`** — carried over from spec §9.8, now
    explicitly paired since both are the same category of problem (OpenCV-CUDA missing
    CPU-OpenCV features this pipeline relies on). Not an eSDK Pro question; a CUDA/OpenCV
    research item to resolve before implementing §3.18/§3.20.
13. **`libpcap` linkage relevance** — carried over from spec §9.9, unresolved either way.
14. **Digital I/O line-pulse purpose** — carried over from spec §9.10; if operationally required,
    the new camera/SDK needs an equivalent digital-output trigger mechanism, not investigated in
    either reference doc since it's outside what the API reference pages document (would need
    the camera's own I/O feature reference).

## 5. What this mapping deliberately does not resolve

Per the spec's own build-system note: CMake is the target build system (following the new SDK's
example convention), not the old Makefile — not re-litigated here. Camera parameter *names*
beyond those already confirmed via the how-to guide (`Exposure`, `Width`, `Height`, `FrameRate`,
`PixelFormat`, `TriggerMode`, `TriggerSource`, `PtpMode`, `PtpStatus`) still need the HR-2000SM's
own GenICam feature reference — this was already flagged in new-sdk-reference.md and isn't
repeated in gap-list form here to avoid duplication.
