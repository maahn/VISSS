#include "recordtask.h"

#include <eSdkPro/errors.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <cuda_runtime.h>

#include <pwd.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

// Loaded by eCaptureProServer as a shared library (dropped into eSdkPro/plugins/), same pattern
// as motion_detect and the vendor cuda_brightness example - ConnectServer(ip) always talks to
// that separate process, even for 127.0.0.1.
class RegisterRecordTask
{
public:
    RegisterRecordTask()
    {
        try
        {
            eSdkPro::Plugin::RegisterTaskPlugin<RecordTask>();
        }
        catch (const std::exception&)
        {}
    }
};
static RegisterRecordTask g_registerRecordTask{};

namespace
{
std::string AvErrToString(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

// See RollSegmentIfNeeded's comment: a boundary-triggered rollover within this many seconds of
// the task's first frame is suppressed, to avoid a near-zero-length second file.
constexpr uint64_t c_minSecondsBeforeRollover = 10;

// eCaptureProServer runs as root (see ../../README.md's "Known gaps": vendor default, no
// User=/capabilities in the systemd unit - root cause of the non-root camera-open failure was
// never identified), so every file
// this task creates - the finalized .mp4/.txt, the .jpg snapshot, and the directories built to
// hold them - would otherwise be root-owned, unreadable/undeletable by the "visss" account that
// owns everything else in the deployment (downstream tooling, sync scripts, VISSSlib). Resolved
// once via getpwnam and cached (uid/gid don't change mid-run) rather than looked up on every
// call. Returns false (and logs, but never aborts recording over it - a permissions/ownership
// nicety isn't worth losing footage for) if the "visss" account doesn't exist on this host or the
// chown itself fails.
bool ChownToVisss(const std::string& path)
{
    static uid_t s_uid = 0;
    static gid_t s_gid = 0;
    static bool s_resolved = false;
    static bool s_available = false;

    if (!s_resolved)
    {
        s_resolved = true;
        // getpwnam is not thread-safe (returns a pointer to static storage), but this task's
        // rollover/snapshot paths that call ChownToVisss all run on the same eCaptureProServer
        // worker thread for this task instance, never concurrently with each other.
        const struct passwd* pw = getpwnam("visss");
        if (pw != nullptr)
        {
            s_uid = pw->pw_uid;
            s_gid = pw->pw_gid;
            s_available = true;
        }
    }
    if (!s_available)
    {
        return false;
    }
    return chown(path.c_str(), s_uid, s_gid) == 0;
}
} // namespace

RecordTask::RecordTask()
{
    SetName(Record::c_taskName);

    m_input = CreateFrameInput(Record::c_inputName, eSdkPro::HWPlatform::Cuda);
    m_shouldWriteInput = CreateDataInput(Record::c_shouldWriteInputName, eSdkPro::HWPlatform::Host);

    m_outputRootParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_outputRootParamName);
    m_outputRootParam.SetValue(".");
    m_outputRootParam.SetToolTip("Base output directory - the VISSS folder convention is built under this.");

    m_nameParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_nameParamName);
    m_nameParam.SetValue("VISSS");
    m_nameParam.SetToolTip("Instrument name used in file naming (matches the old CLI's -n/--name).");

    m_deviceIdParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_deviceIdParamName);
    m_deviceIdParam.SetValue("0");
    m_deviceIdParam.SetToolTip("Camera device id used in file naming.");

    m_widthParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(Record::c_widthParamName);
    m_widthParam.SetValue(0);
    m_widthParam.SetToolTip("Frame width - must match the frames this task will actually receive.");

    m_heightParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(Record::c_heightParamName);
    m_heightParam.SetValue(0);
    m_heightParam.SetToolTip("Frame height - must match the frames this task will actually receive.");

    m_framerateParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(Record::c_framerateParamName);
    m_framerateParam.SetValue(30);
    m_framerateParam.SetToolTip("Encoder output framerate.");

    m_bitrateKbpsParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(Record::c_bitrateKbpsParamName);
    m_bitrateKbpsParam.SetValue(10000);
    m_bitrateKbpsParam.SetToolTip("Target encoder bitrate in Kbps.");

    m_newFileIntervalSecParam =
        CreateParameter<eSdkPro::Plugin::Int32TaskParam>(Record::c_newFileIntervalSecParamName);
    m_newFileIntervalSecParam.SetValue(600);
    m_newFileIntervalSecParam.SetToolTip("Rollover interval in seconds (0 = never roll over).");

    m_presetParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_presetParamName);
    // p1 is the most conservative known-safe value - see the Record::c_presetParamName comment in
    // recordtask.h for the current full p1-p7 measurement (p1-p5 clean, p6/p7 overload, as of this
    // session's hardware). Left at p1 here rather than raised to p5, since changing the default is
    // a user decision, not something to do silently while updating the measurement comment.
    m_presetParam.SetValue("p1");
    m_presetParam.SetToolTip("nvenc preset (p1 fastest/lowest-quality .. p7 slowest/highest-quality).");

    m_minBrightChangeParam = CreateParameter<eSdkPro::Plugin::Int32TaskParam>(Record::c_minBrightChangeParamName);
    m_minBrightChangeParam.SetValue(20);
    m_minBrightChangeParam.SetToolTip(
        "Must match MotionDetectTask's MinBrightChange - selects the metadata header's bin-edge column names.");

    m_temperatureParam = CreateParameter<eSdkPro::Plugin::FloatTaskParam>(Record::c_temperatureParamName);
    m_temperatureParam.SetValue(Record::c_temperatureNotYetReadSentinel);
    m_temperatureParam.SetToolTip(
        "Camera sensor temperature, updated periodically by the client, for the metadata header.");

    m_ptpStatusParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_ptpStatusParamName);
    // Defaults to "Slave", not "unknown": by construction, any frame this task ever sees was
    // captured after main.cpp's mandatory PTP-lock gate already succeeded - see class doc comment.
    m_ptpStatusParam.SetValue("Slave");
    m_ptpStatusParam.SetToolTip("PTP sync status, updated periodically by the client, for the metadata header.");

    m_cameraConfigNameParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_cameraConfigNameParamName);
    m_cameraConfigNameParam.SetValue("none");
    m_cameraConfigNameParam.SetToolTip(
        "Basename of the client's -c camera config file (or \"none\"), for the metadata header.");

    m_lastSessionEventParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_lastSessionEventParamName);
    m_lastSegmentClosedParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_lastSegmentClosedParamName);
    m_lastSegmentStartedParam =
        CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_lastSegmentStartedParamName);
    m_lastSnapshotParam = CreateParameter<eSdkPro::Plugin::StringTaskParam>(Record::c_lastSnapshotParamName);
    for (auto* param : {&m_lastSessionEventParam, &m_lastSegmentClosedParam, &m_lastSegmentStartedParam,
                        &m_lastSnapshotParam})
    {
        param->SetValue("");
        param->SetToolTip("Read-only: lifecycle event, polled by the client - see class doc.");
    }
}

RecordTask::~RecordTask()
{
    CloseSegment(false);
    TeardownEncoder();
}

bool RecordTask::Init()
{
    if (!InitEncoder())
    {
        return false;
    }
    m_firstFrame = true;
    const std::string sessionMsg = "hevc_nvenc session opened, " + std::to_string(m_width) + "x" +
                                    std::to_string(m_height) + " @ " + std::to_string(m_framerateParam.GetValue()) +
                                    "fps, " + std::to_string(m_bitrateKbpsParam.GetValue()) + "Kbps.";
    LogMessage(eSdkPro::LogLevel::Info, sessionMsg);
    m_lastSessionEventParam.SetValue(sessionMsg);
    return true;
}

void RecordTask::Deinit()
{
    CloseSegment(false);
    TeardownEncoder();
}

void RecordTask::Stop()
{
    CloseSegment(false);
}

bool RecordTask::InitEncoder()
{
    m_width = static_cast<uint32_t>(m_widthParam.GetValue());
    m_height = static_cast<uint32_t>(m_heightParam.GetValue());
    if (m_width == 0 || m_height == 0)
    {
        LogMessage(eSdkPro::LogLevel::Error,
                   "RecordTask: Width/Height task parameters must be set (by the client, before "
                   "pipeline.Start()) to a nonzero value.");
        return false;
    }

    if ((m_width % 2 != 0) || (m_height % 2 != 0))
    {
        // NV12 subsamples chroma 2x2, so odd dimensions have no well defined
        // chroma plane size. Recording anyway (with the rounded-up memset in
        // Process()) beats refusing to record, but it is worth saying out loud.
        LogMessage(eSdkPro::LogLevel::Warning,
                   "RecordTask: frame size " + std::to_string(m_width) + "x" + std::to_string(m_height) +
                       " is not even; NV12 chroma is subsampled 2x2, expect the last row/column to be "
                       "approximated.");
    }

    const int framerate = m_framerateParam.GetValue();
    const int bitrateKbps = m_bitrateKbpsParam.GetValue();

    int ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret < 0)
    {
        Abort("av_hwdevice_ctx_create failed: " + AvErrToString(ret));
        return false;
    }

    m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
    if (m_hwFramesCtx == nullptr)
    {
        Abort("av_hwframe_ctx_alloc failed");
        return false;
    }

    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(m_hwFramesCtx->data);
    framesCtx->format = AV_PIX_FMT_CUDA;
    framesCtx->sw_format = AV_PIX_FMT_NV12;
    framesCtx->width = static_cast<int>(m_width);
    framesCtx->height = static_cast<int>(m_height);
    // Generous pool: at high frame rates (485fps observed) several frames are legitimately
    // in-flight inside the encoder's own pipeline at once (held via internal refcounted
    // references, not released until each frame is fully encoded) - a too-small pool starves
    // Process() waiting for a free slot, which is indistinguishable from "too slow" upstream and
    // shows up as dropped frames before the encoder itself. GPU memory cost of a few extra NV12
    // buffers is negligible.
    framesCtx->initial_pool_size = 32;

    ret = av_hwframe_ctx_init(m_hwFramesCtx);
    if (ret < 0)
    {
        Abort("av_hwframe_ctx_init failed: " + AvErrToString(ret));
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("hevc_nvenc");
    if (codec == nullptr)
    {
        Abort("hevc_nvenc encoder not found");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (m_codecCtx == nullptr)
    {
        Abort("avcodec_alloc_context3 failed");
        return false;
    }

    m_codecCtx->width = static_cast<int>(m_width);
    m_codecCtx->height = static_cast<int>(m_height);
    m_codecCtx->time_base = AVRational{1, framerate};
    m_codecCtx->framerate = AVRational{framerate, 1};
    m_codecCtx->pix_fmt = AV_PIX_FMT_CUDA;
    m_codecCtx->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
    m_codecCtx->gop_size = framerate;
    m_codecCtx->max_b_frames = 0; // no B-frame reordering/lookahead latency
    m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);

    // Preset trades GPU cycles for compression quality at a fixed bitrate - see the Preset task
    // param comment in recordtask.h for measured data points (p1 ~50% NVENC/0 dropped frames,
    // p4 ~90%/dropped frames returned). rc-lookahead and max_b_frames are separate, bigger quality
    // levers deliberately left alone here so a regression is attributable to one change, not
    // three. Best-effort (LogMessage, not Abort) since exact AVOption availability can vary by
    // nvenc/driver version and this is a tuning knob, not a correctness requirement.
    const std::string preset = m_presetParam.GetValue();
    if (av_opt_set(m_codecCtx->priv_data, "preset", preset.c_str(), 0) < 0)
    {
        LogMessage(eSdkPro::LogLevel::Warning, "Failed to set nvenc preset=" + preset + ", using encoder default.");
    }
    if (av_opt_set(m_codecCtx->priv_data, "rc-lookahead", "0", 0) < 0)
    {
        LogMessage(eSdkPro::LogLevel::Warning, "Failed to set nvenc rc-lookahead=0, using encoder default.");
    }
    // "rc" defaults to -1 ("let the preset decide"), and evidently preset=p1's default isn't CBR -
    // measured output came out at ~41Mbps against a 15Mbps bit_rate target. Force CBR explicitly
    // so bit_rate/-b is actually honored rather than silently ignored.
    if (av_opt_set(m_codecCtx->priv_data, "rc", "cbr", 0) < 0)
    {
        LogMessage(eSdkPro::LogLevel::Warning, "Failed to set nvenc rc=cbr, bitrate target may not be honored.");
    }
    // Defaults to false: without it, a "forced keyframe" (Process() sets pict_type=AV_PICTURE_
    // TYPE_I on the first frame of each new segment) may still not be a true IDR - HEVC allows
    // non-IDR sync frames that can reference content before them, which produced black frames at
    // the start of every segment after the first rollover (segment 1, before any flush, was
    // unaffected). forced-idr=1 makes the forced keyframe a real reference-chain reset.
    if (av_opt_set(m_codecCtx->priv_data, "forced-idr", "1", 0) < 0)
    {
        LogMessage(eSdkPro::LogLevel::Warning, "Failed to set nvenc forced-idr=1, segment rollovers may corrupt.");
    }

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0)
    {
        Abort("avcodec_open2 failed: " + AvErrToString(ret));
        return false;
    }

    return true;
}

void RecordTask::TeardownEncoder()
{
    if (m_codecCtx != nullptr)
    {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_hwFramesCtx != nullptr)
    {
        av_buffer_unref(&m_hwFramesCtx);
    }
    if (m_hwDeviceCtx != nullptr)
    {
        av_buffer_unref(&m_hwDeviceCtx);
    }
}

bool RecordTask::RollSegmentIfNeeded(uint64_t timestampUs)
{
    const uint64_t timestampS = timestampUs / 1000000ULL;
    const int newFileIntervalSec = m_newFileIntervalSecParam.GetValue();

    if (m_firstFrame)
    {
        m_startTimestampS = timestampS;
    }

    // Matches the old pipeline's exact housekeeping trigger (PROCESSING_SPEC_teeldyne.md §3.13):
    // fires once per interval boundary (unixtime % newFileIntervalSec == 0, e.g. :00/:10/:20 for
    // 600s - project owner's explicit ask, 2026-08-11, for predictable file-start times, though
    // this modulo condition already matched that before being asked - see below for what was
    // actually missing). Debounced against the modulo condition staying true for every frame
    // sharing that boundary second (at 485fps that's hundreds of frames) by comparing against the
    // frame-timestamp domain directly rather than wall clock - a wall-clock debounce (previously a
    // hardcoded ">10s since last rollover" against time(nullptr)) is vulnerable to processing-
    // latency jitter: when newFileIntervalSec is small (e.g. 10, for testing), that jitter can
    // push the next legitimate boundary inside the debounce window and silently skip an entire
    // rollover (reproduced: -i 10 skipped every other rollover on real hardware).
    //
    // Also suppressed within c_minSecondsBeforeRollover seconds of the task's first frame
    // (m_startTimestampS, set above): the very first segment is opened unconditionally below
    // regardless of alignment (there has to be SOME file to record the earliest frames into), so
    // if startup happened to land just before a boundary, the very next frame crossing that
    // boundary would otherwise immediately roll over again into a second, near-zero-length file -
    // this suppresses only that spurious extra-early rollover, not the boundary alignment itself;
    // every rollover after the first still lands exactly on a unixtime%newFileIntervalSec==0
    // boundary.
    const bool doHousekeeping = (newFileIntervalSec > 0) &&
                                 (timestampS % static_cast<uint64_t>(newFileIntervalSec) == 0) &&
                                 (timestampS != m_lastRolloverTimestampS) &&
                                 (timestampS >= m_startTimestampS + c_minSecondsBeforeRollover);

    if (!doHousekeeping && !m_firstFrame)
    {
        return true;
    }

    CloseSegment(/*reuseEncoder=*/true);
    if (!OpenSegment(timestampUs))
    {
        return false;
    }

    m_lastRolloverTimestampS = timestampS;
    m_firstFrame = false;
    return true;
}

bool RecordTask::OpenSegment(uint64_t timestampUs)
{
    char hostnameBuf[256] = {0};
    gethostname(hostnameBuf, sizeof(hostnameBuf) - 1);
    const std::string hostname(hostnameBuf);
    const std::string name = m_nameParam.GetValue();
    const std::string deviceId = m_deviceIdParam.GetValue();
    const std::string outputRoot = m_outputRootParam.GetValue();

    // Matches the old pipeline's exact rounding (round to nearest 0.1s, truncate to whole
    // seconds) and UTC file-naming convention (PROCESSING_SPEC_teeldyne.md §3.22) - distinct from
    // the status-bar overlay's local time.
    const std::time_t seconds = static_cast<std::time_t>((timestampUs + 100000ULL) / 1000000ULL);
    std::tm tmBuf{};
    gmtime_r(&seconds, &tmBuf);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%Y%m%d-%H%M%S", &tmBuf);
    char dateDir[32];
    strftime(dateDir, sizeof(dateDir), "%Y/%m/%d", &tmBuf);

    // threadId hardcoded to "_0": see the class doc comment.
    const std::string baseName = hostname + "_" + name + "_" + deviceId + "_" + timeStr + "_0";
    const std::string instrumentDir = outputRoot + "/" + hostname + "_" + name + "_" + deviceId;
    const std::string finalDir = instrumentDir + "/data/" + dateDir + "/";
    const std::string stagingDir = outputRoot + "/tmp/";

    std::error_code ec;
    std::filesystem::create_directories(stagingDir, ec);
    if (ec)
    {
        Abort("Failed to create directory '" + stagingDir + "': " + ec.message());
        return false;
    }
    std::filesystem::create_directories(finalDir, ec);
    if (ec)
    {
        Abort("Failed to create directory '" + finalDir + "': " + ec.message());
        return false;
    }
    // Idempotent (safe to re-chown an already-visss-owned directory every segment) - see
    // ChownToVisss's comment for why this is needed at all. Not fatal if it fails.
    ChownToVisss(stagingDir);
    ChownToVisss(instrumentDir);
    ChownToVisss(finalDir);

    m_stagingMp4Path = stagingDir + baseName + ".mp4";
    m_stagingTxtPath = stagingDir + baseName + ".txt";
    m_finalMp4Path = finalDir + baseName + ".mp4";
    m_finalTxtPath = finalDir + baseName + ".txt";
    m_finalJpgPath = finalDir + baseName + ".jpg";

    int ret = avformat_alloc_output_context2(&m_formatCtx, nullptr, nullptr, m_stagingMp4Path.c_str());
    if (ret < 0 || m_formatCtx == nullptr)
    {
        Abort("avformat_alloc_output_context2 failed: " + AvErrToString(ret));
        return false;
    }

    m_stream = avformat_new_stream(m_formatCtx, nullptr);
    if (m_stream == nullptr)
    {
        Abort("avformat_new_stream failed");
        return false;
    }
    m_stream->time_base = m_codecCtx->time_base;

    ret = avcodec_parameters_from_context(m_stream->codecpar, m_codecCtx);
    if (ret < 0)
    {
        Abort("avcodec_parameters_from_context failed: " + AvErrToString(ret));
        return false;
    }

    if ((m_formatCtx->oformat->flags & AVFMT_NOFILE) == 0)
    {
        ret = avio_open(&m_formatCtx->pb, m_stagingMp4Path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            Abort("avio_open failed for '" + m_stagingMp4Path + "': " + AvErrToString(ret));
            return false;
        }
    }

    ret = avformat_write_header(m_formatCtx, nullptr);
    if (ret < 0)
    {
        Abort("avformat_write_header failed: " + AvErrToString(ret));
        return false;
    }

    m_timestampFile.open(m_stagingTxtPath, std::ios::out | std::ios::trunc);
    if (!m_timestampFile.is_open())
    {
        Abort("Failed to open timestamp file '" + m_stagingTxtPath + "'");
        return false;
    }

    // §3.24 metadata header, version "e.1" - see the class doc comment for which fields are
    // genuine eSDK Pro readings vs. the old pipeline's own "not available" sentinels.
    m_timestampFile << "# VISSS file format version: e.1\n";
    m_timestampFile << "# VISSS git tag: " << VISSS_GIT_TAG << "\n";
    m_timestampFile << "# VISSS git branch: " << VISSS_GIT_BRANCH << "\n";
    m_timestampFile << "# Camera reset time: " << timeStr << "\n";
    m_timestampFile << "# us since epoche: " << timestampUs << "\n";
    m_timestampFile << "# Camera serial number: " << deviceId << "\n";
    m_timestampFile << "# Camera configuration: " << m_cameraConfigNameParam.GetValue() << "\n";
    m_timestampFile << "# Hostname: " << hostname << "\n";
    m_timestampFile << "# Camera Temperature: ";
    {
        const float temperature = m_temperatureParam.GetValue();
        if (temperature <= Record::c_temperatureNotYetReadSentinel)
        {
            m_timestampFile << "nan";
        }
        else
        {
            m_timestampFile << std::fixed << std::setprecision(6) << temperature;
            m_timestampFile.unsetf(std::ios::fixed);
        }
    }
    m_timestampFile << "\n";
    m_timestampFile << "# PTP Status: " << m_ptpStatusParam.GetValue() << "\n";
    if (m_minBrightChangeParam.GetValue() == 30)
    {
        m_timestampFile << "# Capture time, Record time, Frame id, Queue Length, 30, 40, 60, 80, 100, 120, 140\n";
    }
    else
    {
        m_timestampFile << "# Capture time, Record time, Frame id, Queue Length, 20, 30, 40, 60, 80, 100, 120\n";
    }

    m_frameIndex = 0;
    // Reset per segment: a segment in which no frame was ever written would
    // otherwise inherit the previous segment's last capture time in its footer.
    m_lastWrittenTimestampUs = 0;
    m_snapshotPending = true;
    LogMessage(eSdkPro::LogLevel::Info, "Started " + m_stagingMp4Path);
    m_lastSegmentStartedParam.SetValue("Started " + m_stagingMp4Path);
    return true;
}

void RecordTask::CloseSegment(bool reuseEncoder)
{
    if (m_formatCtx == nullptr)
    {
        return; // nothing open (e.g. Deinit() after Stop() already closed it)
    }

    // Drain any frames still buffered inside the encoder's own pipeline so they land in this
    // (still current) file, not the next one.
    avcodec_send_frame(m_codecCtx, nullptr);
    DrainPackets();
    if (reuseEncoder)
    {
        // hevc_nvenc declares AV_CODEC_CAP_ENCODER_FLUSH - resets internal state so the same
        // (already-open, expensive-to-recreate) NVENC session can encode the next segment as a
        // fresh independent stream.
        avcodec_flush_buffers(m_codecCtx);
    }

    av_write_trailer(m_formatCtx);
    if ((m_formatCtx->oformat->flags & AVFMT_NOFILE) == 0)
    {
        avio_closep(&m_formatCtx->pb);
    }
    avformat_free_context(m_formatCtx);
    m_formatCtx = nullptr;
    m_stream = nullptr;

    if (m_timestampFile.is_open())
    {
        // Whole seconds, not microseconds: the old Teledyne pipeline writes
        // this one field in seconds (storage_worker_cv.h passes timestamp_s
        // while every other timestamp in the file is microseconds), and this
        // format follows that pipeline rather than diverging from it - one
        // downstream parser has to cope with both files.
        m_timestampFile << "# Last capture time: " << (m_lastWrittenTimestampUs / 1000000ULL) << "\n";
    }
    m_timestampFile.close();

    std::error_code ec;
    std::filesystem::rename(m_stagingMp4Path, m_finalMp4Path, ec);
    if (ec)
    {
        LogMessage(eSdkPro::LogLevel::Error,
                   "Failed to move '" + m_stagingMp4Path + "' to '" + m_finalMp4Path + "': " + ec.message());
    }
    else
    {
        // Moving out of tmp/ is exactly when this matters: the file just landed in its
        // permanent, downstream-consumed location - see ChownToVisss's comment for why it would
        // otherwise stay root-owned.
        ChownToVisss(m_finalMp4Path);
    }
    std::filesystem::rename(m_stagingTxtPath, m_finalTxtPath, ec);
    if (ec)
    {
        LogMessage(eSdkPro::LogLevel::Error,
                   "Failed to move '" + m_stagingTxtPath + "' to '" + m_finalTxtPath + "': " + ec.message());
    }
    else
    {
        ChownToVisss(m_finalTxtPath);
    }

    const std::string outputRoot = m_outputRootParam.GetValue();
    const std::string name = m_nameParam.GetValue();
    const std::string deviceId = m_deviceIdParam.GetValue();
    // DeviceId included, not just Name: Name is shared across every camera the client manages
    // (one -n value for the whole process, see main.cpp), so two cameras behind one server would
    // otherwise both write the *same* "_latest_0.*" path and race each other (confirmed by
    // testing: a real 2-camera deployment produced only one set of _latest files, whichever
    // camera's RecordTask wrote last "won"). DeviceId (the camera serial) is what actually keeps
    // this unique per camera - matches the Python launcher's wiper lastImage path, which must
    // stay in sync with this naming (see launch_visss_data_acquisition.py's EmergentInstrument).
    CreateSymlink(m_finalMp4Path, outputRoot + "/" + name + "_" + deviceId + "_latest_0.mp4");
    CreateSymlink(m_finalTxtPath, outputRoot + "/" + name + "_" + deviceId + "_latest_0.txt");

    LogMessage(eSdkPro::LogLevel::Info, "Written " + m_finalMp4Path);
    m_lastSegmentClosedParam.SetValue("Written " + m_finalMp4Path);
}

void RecordTask::CreateSymlink(const std::string& target, const std::string& link)
{
    // Atomic replace: symlink a .tmp path, then rename over the real link - matches the old
    // pipeline's create_symlink() exactly (visss-data-acquisition.h).
    const std::string tmp = link + ".tmp";
    if (symlink(target.c_str(), tmp.c_str()) != 0)
    {
        LogMessage(eSdkPro::LogLevel::Error,
                   "Failed to create symlink '" + tmp + "' -> '" + target + "': " + strerror(errno));
        return;
    }
    if (rename(tmp.c_str(), link.c_str()) != 0)
    {
        LogMessage(eSdkPro::LogLevel::Error, "Failed to rename '" + tmp + "' to '" + link + "': " + strerror(errno));
    }
}

bool RecordTask::SaveSnapshot(const eSdkPro::Frame& inputFrame, const std::string& jpgPath)
{
    // One-off, low-frequency operation (once per segment, e.g. every 600s) - the CPU round-trip
    // here has no bearing on the real-time GPU pipeline, unlike the per-frame encode path.
    std::vector<uint8_t> hostBuf(static_cast<size_t>(m_width) * m_height);
    cudaError_t cudaErr = cudaMemcpy2D(hostBuf.data(), m_width, inputFrame.GetDataPtr(), inputFrame.GetStride(),
                                        m_width, m_height, cudaMemcpyDeviceToHost);
    if (cudaErr != cudaSuccess)
    {
        LogMessage(eSdkPro::LogLevel::Error,
                   "Snapshot: cudaMemcpy2D (device->host) failed: " + std::string(cudaGetErrorString(cudaErr)));
        return false;
    }

    const AVCodec* jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (jpegCodec == nullptr)
    {
        LogMessage(eSdkPro::LogLevel::Error, "Snapshot: mjpeg encoder not found.");
        return false;
    }

    AVCodecContext* jpegCtx = avcodec_alloc_context3(jpegCodec);
    if (jpegCtx == nullptr)
    {
        LogMessage(eSdkPro::LogLevel::Error, "Snapshot: avcodec_alloc_context3 failed.");
        return false;
    }
    jpegCtx->width = static_cast<int>(m_width);
    jpegCtx->height = static_cast<int>(m_height);
    // ffmpeg's mjpeg encoder only supports yuvj420p/yuvj422p/yuvj444p (verified against the
    // installed build - no gray8) - same trick as the video path's mono8->nv12 conversion: Y
    // plane is the real mono data, U/V planes are a constant 128 (neutral chroma, "no color").
    jpegCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    jpegCtx->time_base = AVRational{1, 1};

    int ret = avcodec_open2(jpegCtx, jpegCodec, nullptr);
    if (ret < 0)
    {
        LogMessage(eSdkPro::LogLevel::Error, "Snapshot: avcodec_open2 (mjpeg) failed: " + AvErrToString(ret));
        avcodec_free_context(&jpegCtx);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    frame->width = jpegCtx->width;
    frame->height = jpegCtx->height;
    frame->format = AV_PIX_FMT_YUVJ420P;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
    {
        LogMessage(eSdkPro::LogLevel::Error, "Snapshot: av_frame_get_buffer failed: " + AvErrToString(ret));
        av_frame_free(&frame);
        avcodec_free_context(&jpegCtx);
        return false;
    }
    for (uint32_t row = 0; row < m_height; row++)
    {
        memcpy(frame->data[0] + (static_cast<size_t>(row) * static_cast<size_t>(frame->linesize[0])),
               hostBuf.data() + (static_cast<size_t>(row) * m_width), m_width);
    }
    const uint32_t chromaWidth = (m_width + 1) / 2;
    const uint32_t chromaHeight = (m_height + 1) / 2;
    for (uint32_t row = 0; row < chromaHeight; row++)
    {
        memset(frame->data[1] + (static_cast<size_t>(row) * static_cast<size_t>(frame->linesize[1])), 128,
               chromaWidth);
        memset(frame->data[2] + (static_cast<size_t>(row) * static_cast<size_t>(frame->linesize[2])), 128,
               chromaWidth);
    }
    frame->pts = 0;

    bool ok = true;
    std::ofstream jpgFile(jpgPath, std::ios::binary);
    if (!jpgFile.is_open())
    {
        LogMessage(eSdkPro::LogLevel::Error, "Snapshot: failed to open '" + jpgPath + "' for writing.");
        ok = false;
    }
    else
    {
        // mjpeg's encoded packet IS a complete, standalone JPEG file (unlike h264/hevc elementary
        // streams) - written directly, no muxer/container needed.
        ret = avcodec_send_frame(jpegCtx, frame);
        if (ret >= 0)
        {
            ret = avcodec_send_frame(jpegCtx, nullptr); // flush - single-image encode
        }

        AVPacket* pkt = av_packet_alloc();
        while (ret >= 0)
        {
            ret = avcodec_receive_packet(jpegCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break;
            }
            if (ret < 0)
            {
                LogMessage(eSdkPro::LogLevel::Error,
                           "Snapshot: avcodec_receive_packet failed: " + AvErrToString(ret));
                ok = false;
                break;
            }
            jpgFile.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        jpgFile.close();
    }

    av_frame_free(&frame);
    avcodec_free_context(&jpegCtx);
    if (ok)
    {
        LogMessage(eSdkPro::LogLevel::Info, "Written " + jpgPath);
        m_lastSnapshotParam.SetValue("Written " + jpgPath);
    }
    return ok;
}

bool RecordTask::SendFrameToEncoder(AVFrame* frame)
{
    // AVERROR(EAGAIN) is not an error here, it's the normal "encoder's internal queue is full"
    // signal: drain whatever packets are ready to make room, then retry submitting the same
    // frame. Treating it as fatal (as an earlier version of this code did) would abort recording
    // the first time the encoder's queue briefly filled up during normal operation.
    while (true)
    {
        int ret = avcodec_send_frame(m_codecCtx, frame);
        if (ret == 0)
        {
            return true;
        }
        if (ret == AVERROR(EAGAIN))
        {
            if (!DrainPackets())
            {
                return false;
            }
            continue;
        }

        Abort("avcodec_send_frame failed: " + AvErrToString(ret));
        return false;
    }
}

bool RecordTask::DrainPackets()
{
    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr)
    {
        Abort("av_packet_alloc failed");
        return false;
    }

    while (true)
    {
        int ret = avcodec_receive_packet(m_codecCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        if (ret < 0)
        {
            av_packet_free(&pkt);
            Abort("avcodec_receive_packet failed: " + AvErrToString(ret));
            return false;
        }

        av_packet_rescale_ts(pkt, m_codecCtx->time_base, m_stream->time_base);
        pkt->stream_index = m_stream->index;

        ret = av_interleaved_write_frame(m_formatCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
        {
            av_packet_free(&pkt);
            Abort("av_interleaved_write_frame failed: " + AvErrToString(ret));
            return false;
        }
    }

    av_packet_free(&pkt);
    return true;
}

bool RecordTask::Process()
{
    try
    {
        eSdkPro::Frame inputFrame = m_input.GetFrame();

        if (inputFrame.GetPixelFormat() != GVSP_PIX_MONO8)
        {
            LogMessage(eSdkPro::LogLevel::Error, "RecordTask only supports GVSP_PIX_MONO8 frames currently.");
            return false;
        }

        if (inputFrame.GetWidth() != m_width || inputFrame.GetHeight() != m_height)
        {
            Abort("RecordTask: frame dimensions (" + std::to_string(inputFrame.GetWidth()) + "x" +
                  std::to_string(inputFrame.GetHeight()) + ") don't match configured Width/Height (" +
                  std::to_string(m_width) + "x" + std::to_string(m_height) + ").");
            return false;
        }

        // Read the paired recording decision (§3.21) and histogram counts (§3.18) from
        // MotionDetectTask - see recordtask.h's c_shouldWriteInputName comment for why this is a
        // companion data port rather than MotionDetectTask simply not pushing filtered frames.
        eSdkPro::Data shouldWriteData = m_shouldWriteInput.GetData();
        const auto* decision = reinterpret_cast<const ShouldWritePortPayload*>(shouldWriteData.GetDataPtr());
        const bool shouldWrite = (decision != nullptr) && (decision->shouldWrite != 0);
        const uint32_t queueLength = (decision != nullptr) ? decision->queueLength : 0;
        uint32_t histCounts[7] = {0, 0, 0, 0, 0, 0, 0};
        if (decision != nullptr)
        {
            std::memcpy(histCounts, decision->histCounts, sizeof(histCounts));
        }

        const uint64_t timestampUs = inputFrame.GetTimestampNs() / 1000ULL;
        if (!RollSegmentIfNeeded(timestampUs))
        {
            return false;
        }

        if (!shouldWrite)
        {
            // Filtered out (§3.21): rollover state above is already updated from this frame's
            // timestamp regardless, so segment boundaries stay accurate even when the frame that
            // happens to land on one is itself filtered. Nothing else to do - no encode, no
            // snapshot, no metadata row (m_snapshotPending, if set, carries over to the next
            // frame that does get written).
            return true;
        }

        if (m_snapshotPending)
        {
            // Best-effort: a failed snapshot shouldn't take down recording, so no Abort() here -
            // but only symlink it if it actually got written, or the link dangles.
            if (SaveSnapshot(inputFrame, m_finalJpgPath))
            {
                // Written directly to its final path (no tmp/ staging for jpg, unlike mp4/txt) -
                // see ChownToVisss's comment for why this is needed at all.
                ChownToVisss(m_finalJpgPath);
                // DeviceId included - see the .mp4/.txt CreateSymlink calls' comment for why.
                CreateSymlink(m_finalJpgPath, m_outputRootParam.GetValue() + "/" + m_nameParam.GetValue() + "_" +
                                                   m_deviceIdParam.GetValue() + "_latest_0.jpg");
            }
            m_snapshotPending = false;
        }

        // Get a CUDA frame from ffmpeg's own hwframe pool (not our own buffer) - avoids the
        // fragility of manually wrapping a foreign device pointer into an AVHWFramesContext;
        // costs one extra GPU-to-GPU copy below, which is negligible.
        AVFrame* hwFrame = av_frame_alloc();
        if (hwFrame == nullptr)
        {
            Abort("av_frame_alloc failed");
            return false;
        }

        int ret = av_hwframe_get_buffer(m_hwFramesCtx, hwFrame, 0);
        if (ret < 0)
        {
            av_frame_free(&hwFrame);
            Abort("av_hwframe_get_buffer failed: " + AvErrToString(ret));
            return false;
        }

        // mono8 -> nv12: Y plane is the mono data as-is (stride-aware device-to-device copy), UV
        // plane is a constant 128 (neutral chroma / "no color") - no CUDA kernel needed, matches
        // the old pipeline's gray-only assumption (PROCESSING_SPEC_teeldyne.md §3.14).
        cudaError_t cudaErr = cudaMemcpy2D(hwFrame->data[0], static_cast<size_t>(hwFrame->linesize[0]),
                                            inputFrame.GetDataPtr(), inputFrame.GetStride(), m_width, m_height,
                                            cudaMemcpyDeviceToDevice);
        if (cudaErr != cudaSuccess)
        {
            av_frame_free(&hwFrame);
            Abort("cudaMemcpy2D (Y plane) failed: " + std::string(cudaGetErrorString(cudaErr)));
            return false;
        }

        // Rounded up, not truncated: with an odd m_height the last chroma row
        // was left uninitialized, showing up as coloured noise along the
        // bottom edge of otherwise grey footage.
        cudaErr = cudaMemset2D(hwFrame->data[1], static_cast<size_t>(hwFrame->linesize[1]), 128, m_width,
                                (m_height + 1) / 2);
        if (cudaErr != cudaSuccess)
        {
            av_frame_free(&hwFrame);
            Abort("cudaMemset2D (UV plane) failed: " + std::string(cudaGetErrorString(cudaErr)));
            return false;
        }

        hwFrame->pts = m_frameIndex++;
        if (hwFrame->pts == 0)
        {
            // Force a keyframe on the first frame of every new segment. avcodec_flush_buffers()
            // resets the encoder's internal state for reuse, but NVENC's own GOP/keyframe-cadence
            // tracking doesn't know a brand new independent file just started - without this, the
            // new segment's first frame can come out as a P-frame referencing content from the
            // *previous* segment, which doesn't exist in the new file's timeline. Symptom: every
            // segment after the first rollover opens to a black frame in players (first segment,
            // opened before any flush, is unaffected).
            hwFrame->pict_type = AV_PICTURE_TYPE_I;
        }

        bool ok = SendFrameToEncoder(hwFrame);
        av_frame_free(&hwFrame);
        if (!ok)
        {
            return false;
        }

        // One row per frame actually submitted to the encoder - see class doc comment for why
        // this guarantees sync with the video even across dropped frames. "Record time" is when
        // this row is actually written (processing-latency indicator vs. "Capture time").
        // "Queue Length" is MotionDetectTask's queueLength estimate, carried through the
        // ShouldWrite payload unchanged - see its QueueDepth comment for what it measures and why
        // RecordTask can't compute this metric itself (the overlay baking it into pixels in
        // MotionDetectTask happens before RecordTask ever sees the frame).
        const auto recordTimeUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        m_timestampFile << timestampUs << ", " << recordTimeUs << ", " << inputFrame.GetFrameId() << ", "
                         << queueLength;
        for (uint32_t histCount : histCounts)
        {
            m_timestampFile << ", " << histCount;
        }
        m_timestampFile << "\n";
        m_lastWrittenTimestampUs = timestampUs;

        return DrainPackets();
    }
    catch (const eSdkPro::ESdkProException& e)
    {
        Abort(e.what());
        return false;
    }
    catch (const std::exception& e)
    {
        Abort(e.what());
        return false;
    }
}
