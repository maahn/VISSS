
// ============================================================================
//https://stackoverflow.com/questions/37140643/how-to-save-two-cameras-data-but-not-influence-their-picture-acquire-speed/37146523
// https://gist.github.com/yohhoy/52b31522dbb751e5296e
// ============================================================================

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

class storage_worker_ffmpeg
{
public:
    storage_worker_ffmpeg(frame_queue& queue
        , int32_t id
        , std::string const& file_name
        , int32_t fourcc
        , double fps
        , cv::Size frame_size
        , bool is_color = true);

    void run();

    double total_time_ms() const { return total_time_ / 1000.0; }

private:
    frame_queue& queue_;

    int32_t id_;

    std::string file_name_;
    int32_t fourcc_;
    double fps_;
    cv::Size frame_size_;
    bool is_color_;

    double total_time_;
};
// ----------------------------------------------------------------------------
storage_worker_ffmpeg::storage_worker_ffmpeg(frame_queue& queue
    , int32_t id
    , std::string const& file_name
    , int32_t fourcc
    , double fps
    , cv::Size frame_size
    , bool is_color)
    : queue_(queue)
    , id_(id)
    , file_name_(file_name)
    , fourcc_(fourcc)
    , fps_(fps)
    , frame_size_(frame_size)
    , is_color_(is_color)
    , total_time_(0.0)
{
}
// // ----------------------------------------------------------------------------
void storage_worker_ffmpeg::run()


// TEST cv::CAP_FFMPEG!!
//     res["h264"] = VideoWriter::fourcc('H','2','6','4');
//     res["h265"] = VideoWriter::fourcc('H','E','V','C');
//     res["mpeg2"] = VideoWriter::fourcc('M','P','E','G');
//     res["mpeg4"] = VideoWriter::fourcc('M','P','4','2');
//     res["mjpeg"] = VideoWriter::fourcc('M','J','P','G');
//     res["vp8"] = VideoWriter::fourcc('V','P','8','0');

// DEBUG FLAG IN MAKEFILE!! -O3 -O2

{


    // initialize FFmpeg library
    av_register_all();

























    
    // 6.91 ms
    cv::VideoWriter writer(file_name_, cv::CAP_FFMPEG, fourcc_, fps_, frame_size_, is_color_);
    std::ofstream fMeta;
    // writer.set(cv::VIDEOWRITER_PROP_QUALITY, 5);

    // 8.05 ms
    // cv::VideoWriter writer("appsrc ! videoconvert  ! timeoverlay ! x264enc speed-preset=ultrafast  ! mp4mux ! filesink location=video-h264.mp4",
    
    // 10 ms:
    // cv::VideoWriter writer("appsrc ! videoconvert  ! timeoverlay ! vaapih264enc speed-preset=ultrafast ! mp4mux ! filesink location=video-h264.mp4",
    
    // cv::VideoWriter writer("appsrc ! videoconvert ! timeoverlay  ! x264enc speed-preset=ultraast ! mp4mux ! filesink location=video-h2642.mp4",
                                // cv::CAP_GSTREAMER, 0, fps_, frame_size_, is_color_);

    // Open the file.
    fMeta.open(file_name_+".txt");


      std::chrono::milliseconds ms = duration_cast<std::chrono::milliseconds>(t_reset_2.time_since_epoch());

      std::chrono::seconds s = duration_cast<std::chrono::seconds>(ms);
      std::time_t t = s.count();
      std::size_t fractional_seconds = ms.count() % 1000;
    char *ctime_no_newline;
    ctime_no_newline = strtok(ctime(&t), "\n");
      fMeta << "# Camera start time: " << ctime_no_newline << ' '
            << fractional_seconds  << "\n";
    fMeta << "# ns since epoche: " 
          << t_reset_2.time_since_epoch().count() << "\n";
    
    fMeta << "# Camera serial number: "
          << DeviceID << "\n";
     
    fMeta << "# Computer time, Camera time, Frame id \n";
    try {
        int32_t frame_count(0);
        for (;;) {
            MatMeta image(queue_.pop());
            if (!image.MatImage.empty()) {
                high_resolution_clock::time_point t1(high_resolution_clock::now());

                ++frame_count;
                writer.write(image.MatImage);

                high_resolution_clock::time_point t2(high_resolution_clock::now());
                double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
                total_time_ += dt_us;

                //to do : move to thread? https://stackoverflow.com/questions/21126950/asynchronously-writing-to-a-file-in-c-unix
                fMeta  <<image.timestamp 
                      << ", " << image.id << "\n";

                // std::cout << "Worker " << id_ << " stored image.MatImage #" << frame_count
                //     << " in " << (dt_us / 1000.0) << " ms" << std::endl;
            }
        }
    } catch (frame_queue::cancelled& /*e*/) {
        // Nothing more to process, we're done
        std::cout << "Queue " << id_ << " cancelled, worker finished." << std::endl;
        fMeta.close();
    }
}
