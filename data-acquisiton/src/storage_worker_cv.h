
// ============================================================================
//https://stackoverflow.com/questions/37140643/how-to-save-two-cameras-data-but-not-influence-their-picture-acquire-speed/37146523

// ============================================================================

class storage_worker_cv
{
public:
    storage_worker_cv(frame_queue& queue
        , int32_t id
        , std::string const& file_name
        , int32_t fourcc
        , double fps
        , cv::Size frame_size
        , bool is_color
        , double quality
        , double preset);
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
    double quality_;
    double preset_;
    double total_time_;
};
// ----------------------------------------------------------------------------
storage_worker_cv::storage_worker_cv(frame_queue& queue
    , int32_t id
    , std::string const& file_name
    , int32_t fourcc
    , double fps
    , cv::Size frame_size
    , bool is_color
    , double quality
    , double preset)    :
      queue_(queue)
    , id_(id)
    , file_name_(file_name)
    , fourcc_(fourcc)
    , fps_(fps)
    , frame_size_(frame_size)
    , is_color_(is_color)
    , total_time_(0.0)
    , quality_(quality)
    , preset_(preset)
{
}
// // ----------------------------------------------------------------------------
void storage_worker_cv::run()


// TEST cv::CAP_FFMPEG!!
//     res["h264"] = VideoWriter::fourcc('H','2','6','4');
//     res["h265"] = VideoWriter::fourcc('H','E','V','C');
//     res["mpeg2"] = VideoWriter::fourcc('M','P','E','G');
//     res["mpeg4"] = VideoWriter::fourcc('M','P','4','2');
//     res["mjpeg"] = VideoWriter::fourcc('M','J','P','G');
//     res["vp8"] = VideoWriter::fourcc('V','P','8','0');

// DEBUG FLAG IN MAKEFILE!! -O3 -O2

{

    char hostname[HOST_NAME_MAX];

    // 6.91 ms
    cv::VideoWriter writer(file_name_, cv::CAP_FFMPEG, fourcc_, fps_, frame_size_, is_color_);
    std::ofstream fMeta;
    writer.set(cv::VIDEOWRITER_PROP_QUALITY, quality_);
    writer.set(cv::VIDEOWRITER_PROP_PRESET, preset_);
    double foo = writer.get(cv::VIDEOWRITER_PROP_QUALITY);
    printf("GET %f \n",foo);
    double foo2 = writer.get(cv::VIDEOWRITER_PROP_PRESET);
    printf("GET %f \n",foo2);

    // 8.05 ms
    // cv::VideoWriter writer("appsrc ! videoconvert  ! timeoverlay ! x264enc speed-preset=ultrafast  ! mp4mux ! filesink location=video-h264.mp4",
    
    // 10 ms:
    // cv::VideoWriter writer("appsrc ! videoconvert  ! timeoverlay ! vaapih264enc speed-preset=ultrafast ! mp4mux ! filesink location=video-h264.mp4",
    
    // cv::VideoWriter writer("appsrc ! videoconvert ! timeoverlay  ! x264enc speed-preset=ultraast ! mp4mux ! filesink location=video-h2642.mp4",
                                // cv::CAP_GSTREAMER, 0, fps_, frame_size_, is_color_);

    // Open the file.
    fMeta.open(file_name_+".txt");


      std::chrono::milliseconds ms = duration_cast<std::chrono::milliseconds>(t_reset.time_since_epoch());
      std::chrono::seconds s = duration_cast<std::chrono::seconds>(ms);
      std::time_t t = s.count();
      std::size_t fractional_seconds = ms.count() % 1000;
    char *ctime_no_newline;
    ctime_no_newline = strtok(ctime(&t), "\n");
      fMeta << "# Camera start time: " << ctime_no_newline << ' '
            << fractional_seconds  << "\n";
    unsigned long t_reset_uint = t_reset.time_since_epoch().count()/1000;
    fMeta << "# us since epoche: " 
          << t_reset_uint << "\n";
    
    fMeta << "# Camera serial number: "
          << DeviceID << "\n";
     
    fMeta << "# Camera configuration: "
          <<  "CONFIG FILE"<< "\n";

    gethostname(hostname, HOST_NAME_MAX);
    fMeta << "# Hostname: "
          <<  hostname<< "\n";
     
    fMeta << "# Capture time, Record time, Frame id \n";
    try {
        int32_t frame_count(0);
        for (;;) {
            MatMeta image(queue_.pop());
            if (!image.MatImage.empty()) {
                high_resolution_clock::time_point t1(high_resolution_clock::now());
                if (frame_count % 100 == 0)
                {
                    fMeta  <<image.timestamp +t_reset_uint << ", " << t1.time_since_epoch().count()/1000
                          << ", " << image.id << "\n";
                }
                ++frame_count;
                writer.write(image.MatImage);

                high_resolution_clock::time_point t2(high_resolution_clock::now());
                double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
                total_time_ += dt_us;

                //to do : move to thread? https://stackoverflow.com/questions/21126950/asynchronously-writing-to-a-file-in-c-unix
                

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

// void storage_worker_cv::run()
// {

// // https://stackoverflow.com/questions/40454019/opencv-to-ffplay-from-named-pipe-fifo
//     char const * myFIFO = "/tmp/myfifo";
//     int status;
//     if ((status = mkfifo(myFIFO, 0666)) < 0) { 
//         printf("Fifo mkfifo error: %s\n", strerror(errno)); 
//         exit(EXIT_FAILURE);
//     } else {
//         std::cout << "Made a named pipe at: " << myFIFO << std::endl;
//     }

//     // ffplay raw video
//     printf("Run command:\n\ncat /tmp/myfifo | ffplay -f rawvideo -pixel_format bgr24 -video_size %.0fx%.0f -framerate %4.2f -i pipe:\n"
//         ,1280.
//         ,1024.
//         ,200.
//         );  
//     printf("Run command:\n\ncat /tmp/myfifo | cvlc --demux=rawvideo --rawvid-fps=%4.2f --rawvid-width=%.0f --rawvid-height=%.0f  --rawvid-chroma=RV24 - --sout \"#transcode{vcodec=h264,vb=200,fps=30,width=320,height=240}:std{access=http{mime=video/x-flv},mux=ffmpeg{mux=flv},dst=:8081/stream.flv}\""
//         ,200.
//         ,1280.
//         ,1024.
//         );  

//     int fd;
//     if ((fd = open(myFIFO,O_WRONLY)) < 0) {
//         printf("Fifo open error: %s\n", strerror(errno));
//         exit(EXIT_FAILURE);
//     }   


//     try {
//         int32_t frame_count(0);
//         for (;;) {
//             cv::Mat image(queue_.pop());
            
//             if (!image.empty()) {
//                 high_resolution_clock::time_point t1(high_resolution_clock::now());

//                 ++frame_count;

//                 // // image.convertTo(image_rgb, cv::CV_8UC3);
//                 // cv::Mat image_rgb;
//                 // cv::cvtColor(image, image_rgb, cv::COLOR_GRAY2RGB);
//                 // method: named pipe as matrix writes data to the named pipe, but image has glitch
//                 size_t bytes = image.total() * image.elemSize();

//                 if (write(fd, image.data, bytes) < 0) {
//                     printf("Error in write: %s \n", strerror(errno)); 
//                 }            

//                 high_resolution_clock::time_point t2(high_resolution_clock::now());
//                 double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
//                 total_time_ += dt_us;

//                 // std::cout << "Worker " << id_ << " stored image #" << frame_count
//                 //     << " in " << (dt_us / 1000.0) << " ms" << std::endl;
//             }
//         }
//     } catch (frame_queue::cancelled& /*e*/) {
//         // Nothing more to process, we're done
//         close(fd);
//         std::cout << "Queue " << id_ << " cancelled, worker finished." << std::endl;
//     }
// }

