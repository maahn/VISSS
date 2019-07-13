
// ============================================================================
//https://stackoverflow.com/questions/37140643/how-to-save-two-cameras-data-but-not-influence-their-picture-acquire-speed/37146523

// ============================================================================

// FFmpeg
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
        , std::string const& path
        , int32_t fourcc
        , double fps
        , cv::Size frame_size
        , bool is_color
        , double quality
        , double preset
        , std::chrono::time_point<std::chrono::system_clock> t_reset);
    int run();

    double total_time_ms() const { return total_time_ / 1000.0; }

private:
    frame_queue& queue_;

    int32_t id_;
    std::ofstream fMeta_;

    std::string path_;
    std::string filename_;
    int32_t fourcc_;
    double fps_;
    cv::Size frame_size_;
    bool is_color_;
    double quality_;
    double preset_;
    double total_time_;
    std::chrono::time_point<std::chrono::system_clock> t_reset_;
    unsigned long t_reset_uint_;
    char hostname_[HOST_NAME_MAX];

    cv::VideoWriter writer_;

    void add_meta_data();
    void close_files();
    void open_files();
    void create_filename();

};
// ----------------------------------------------------------------------------
storage_worker_ffmpeg::storage_worker_ffmpeg(frame_queue& queue
    , int32_t id
    , std::string const& path
    , int32_t fourcc
    , double fps
    , cv::Size frame_size
    , bool is_color
    , double quality
    , double preset
    , std::chrono::time_point<std::chrono::system_clock> t_reset)    :
      queue_(queue)
    , id_(id)
    , path_(path)
    , fourcc_(fourcc)
    , fps_(fps)
    , frame_size_(frame_size)
    , is_color_(is_color)
    , total_time_(0.0)
    , quality_(quality)
    , preset_(preset)
    , t_reset_(t_reset)
{
}
// // ----------------------------------------------------------------------------


void storage_worker_ffmpeg::add_meta_data() 
{


    std::chrono::milliseconds ms = duration_cast<std::chrono::milliseconds>(t_reset_.time_since_epoch());
    std::chrono::seconds s = duration_cast<std::chrono::seconds>(ms);
    std::time_t t = s.count();
    std::size_t fractional_seconds = ms.count() % 1000;
    char *ctime_no_newline;
    ctime_no_newline = strtok(ctime(&t), "\n");
    fMeta_ << "# Camera start time: " << ctime_no_newline << ' '
            << fractional_seconds  << "\n";
    fMeta_ << "# us since epoche: " 
          << t_reset_uint_ << "\n";
    
    fMeta_ << "# Camera serial number: "
          << DeviceID << "\n";
     
    fMeta_ << "# Camera configuration: "
          <<  "CONFIG FILE"<< "\n";

    fMeta_ << "# Hostname: "
          <<  hostname_<< "\n";
     
    fMeta_ << "# Capture time, Record time, Frame id \n";
    return;

}


void storage_worker_ffmpeg::open_files() 
{

    create_filename();

    // writer_.open(filename_, cv::CAP_FFMPEG, fourcc_, fps_, frame_size_, is_color_);
    // writer_.set(cv::VIDEOWRITER_PROP_QUALITY, quality_);
    // writer_.set(cv::VIDEOWRITER_PROP_PRESET, preset_);
    // double foo = writer_.get(cv::VIDEOWRITER_PROP_QUALITY);
    // printf("GET %f \n",foo);
    // double foo2 = writer_.get(cv::VIDEOWRITER_PROP_PRESET);
    // printf("GET %f \n",foo2);

    // Open the text file.
    fMeta_.open(filename_+".txt");
    add_meta_data();

    return;
}

void storage_worker_ffmpeg::close_files() {
    fMeta_.close();
    // writer_.release();
    return;

}


void storage_worker_ffmpeg::create_filename() {

    time_t t = time(0);   // get time now
    struct tm * now = localtime( & t );

    char timestamp1 [80];
    strftime (timestamp1,80,"%Y/%m/%d",now);
    char timestamp2 [80];
    strftime (timestamp2,80,"%Y%m%d-%H%M%S",now);

    std::string full_path = path_ + "/" + hostname_ + "_" + DeviceID + "/" + timestamp1 + "/" ;
    mkdir_p(full_path.c_str());

    filename_ = full_path + hostname_ + "_" + DeviceID  + "_" + timestamp2 +".avi";
    printf("filename_ %s \n",filename_.c_str());


    return;
}

// TEST cv::CAP_FFMPEG!!
//     res["h264"] = VideoWriter::fourcc('H','2','6','4');
//     res["h265"] = VideoWriter::fourcc('H','E','V','C');
//     res["mpeg2"] = VideoWriter::fourcc('M','P','E','G');
//     res["mpeg4"] = VideoWriter::fourcc('M','P','4','2');
//     res["mjpeg"] = VideoWriter::fourcc('M','J','P','G');
//     res["vp8"] = VideoWriter::fourcc('V','P','8','0');

// DEBUG FLAG IN MAKEFILE!! -O3 -O2

    // 8.05 ms
    // cv::VideoWriter writer("appsrc ! videoconvert  ! timeoverlay ! x264enc speed-preset=ultrafast  ! mp4mux ! filesink location=video-h264.mp4",
    
    // 10 ms:
    // cv::VideoWriter writer("appsrc ! videoconvert  ! timeoverlay ! vaapih264enc speed-preset=ultrafast ! mp4mux ! filesink location=video-h264.mp4",
    
    // cv::VideoWriter writer("appsrc ! videoconvert ! timeoverlay  ! x264enc speed-preset=ultraast ! mp4mux ! filesink location=video-h2642.mp4",
                                // cv::CAP_GSTREAMER, 0, fps_, frame_size_, is_color_);

int storage_worker_ffmpeg::run() 

{
    long int timestamp = 0;
    long int frame_count_new_file = 0;
    gethostname(hostname_, HOST_NAME_MAX);
    t_reset_uint_ = t_reset_.time_since_epoch().count()/1000;
    

    open_files();
//////////////////////////////////////////////
    // initialize FFmpeg library
    av_register_all();
//  av_log_set_level(AV_LOG_DEBUG);

        int ret;

    int dst_width = frame_size_.width;
    int dst_height = frame_size_.height;
    AVRational dst_fps = {fps_, 1};

    // allocate color cv::Mat with extra bytes (required by AVFrame::data)
    std::vector<uint8_t> imgbuf(dst_height * dst_width * 3 + 16);
    cv::Mat imageColor(dst_height, dst_width, CV_8UC3, imgbuf.data(), dst_width * 3);

    // open output format context
    AVFormatContext* outctx = nullptr;
    ret = avformat_alloc_output_context2(&outctx, nullptr, nullptr, filename_.c_str());
    if (ret < 0) {
        std::cerr << "fail to avformat_alloc_output_context2(" << filename_ << "): ret=" << ret;
        return 2;
    }

    // open output IO context
    ret = avio_open2(&outctx->pb, filename_.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "fail to avio_open2: ret=" << ret;
        return 2;
    }
    // create new video stream
    AVCodec* vcodec = avcodec_find_encoder(outctx->oformat->video_codec);
    AVStream* vstrm = avformat_new_stream(outctx, vcodec);
    if (!vstrm) {
        std::cerr << "fail to avformat_new_stream";
        return 2;
    }
    avcodec_get_context_defaults3(vstrm->codec, vcodec);
    vstrm->codec->width = dst_width;
    vstrm->codec->height = dst_height;
    vstrm->codec->pix_fmt = vcodec->pix_fmts[0];
    vstrm->codec->time_base = vstrm->time_base = av_inv_q(dst_fps);
    vstrm->r_frame_rate = vstrm->avg_frame_rate = dst_fps;
    if (outctx->oformat->flags & AVFMT_GLOBALHEADER)
        vstrm->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // open video encoder
    ret = avcodec_open2(vstrm->codec, vcodec, nullptr);
    if (ret < 0) {
        std::cerr << "fail to avcodec_open2: ret=" << ret;
        return 2;
    }

    std::cout
        << "outfile: " << filename_ << "\n"
        << "format:  " << outctx->oformat->name << "\n"
        << "vcodec:  " << vcodec->name << "\n"
        << "size:    " << dst_width << 'x' << dst_height << "\n"
        << "fps:     " << av_q2d(dst_fps) << "\n"
        << "pixfmt:  " << av_get_pix_fmt_name(vstrm->codec->pix_fmt) << "\n"
        << std::flush;

    // initialize sample scaler
    SwsContext* swsctx = sws_getCachedContext(
        nullptr, dst_width, dst_height, AV_PIX_FMT_BGR24,
        dst_width, dst_height, vstrm->codec->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!swsctx) {
        std::cerr << "fail to sws_getCachedContext";
        return 2;
    }

    // allocate frame buffer for encoding
    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> framebuf(avpicture_get_size(vstrm->codec->pix_fmt, dst_width, dst_height));
    avpicture_fill(reinterpret_cast<AVPicture*>(frame), framebuf.data(), vstrm->codec->pix_fmt, dst_width, dst_height);
    frame->width = dst_width;
    frame->height = dst_height;
    frame->format = static_cast<int>(vstrm->codec->pix_fmt);

//////////////////////////////////////////////

    // encoding loop
    avformat_write_header(outctx, nullptr);
    int64_t frame_pts = 0;
    unsigned nb_frames = 0;
    bool end_of_stream = false;
    int got_pkt = 0;

    int32_t frame_count(0);
    do {
        high_resolution_clock::time_point t1(high_resolution_clock::now());
        try {
            MatMeta image(queue_.pop());
            if (!image.MatImage.empty()) {
                


                if (frame_count % 100 == 0)
                {
                    fMeta_  <<image.timestamp +t_reset_uint_ << ", " << t1.time_since_epoch().count()/1000
                          << ", " << image.id << "\n";
                }
                ++frame_count;
                timestamp = static_cast<long int> (time(NULL));
        //         if ((timestamp % 300 == 0) && (frame_count-frame_count_new_file > 300))
        //         {
        // close_files();
        // open_files();
        // frame_count_new_file = frame_count;


        //         }
                // writer_.write(image.MatImage);
                cv::cvtColor(image.MatImage,imageColor,cv::COLOR_GRAY2RGB);
                const int stride[] = { static_cast<int>(imageColor.step[0]) };
                sws_scale(swsctx, &imageColor.data, stride, 0, imageColor.rows, frame->data, frame->linesize);
                frame->pts = frame_pts++;
            } else {
                end_of_stream = true;
            }
        } 
        catch (frame_queue::cancelled& /*e*/) {
            // Nothing more to process, we're done
            std::cout << "Worker finished..." << std::endl;
            end_of_stream = true;
        }

        AVPacket pkt;
        pkt.data = nullptr;
        pkt.size = 0;
        av_init_packet(&pkt);
        ret = avcodec_encode_video2(vstrm->codec, &pkt, end_of_stream ? nullptr : frame, &got_pkt);
        if (ret < 0) {
            std::cerr << "fail to avcodec_encode_video2: ret=" << ret << "\n";
            break;
        }

        if (got_pkt) {
            // rescale packet timestamp
            pkt.duration = 1;
            av_packet_rescale_ts(&pkt, vstrm->codec->time_base, vstrm->time_base);
            // write packet
            av_write_frame(outctx, &pkt);
            std::cout << nb_frames << '\r' << std::flush;  // dump progress
            ++nb_frames;
        }

        av_free_packet(&pkt);

        high_resolution_clock::time_point t2(high_resolution_clock::now());
        double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
        total_time_ += dt_us;

        //to do : move to thread? https://stackoverflow.com/questions/21126950/asynchronously-writing-to-a-file-in-c-unix
        

        // std::cout << "Worker " << id_ << " stored image.MatImage #" << frame_count
        //     << " in " << (dt_us / 1000.0) << " ms" << std::endl;
    }  while (!end_of_stream || got_pkt);
    close_files();
    std::cout << "Queue " << id_ << " cancelled, worker finished." << std::endl;
    av_write_trailer(outctx);
    std::cout << nb_frames << " frames encoded" << std::endl;

    av_frame_free(&frame);
    avcodec_close(vstrm->codec);
    avio_close(outctx->pb);
    avformat_free_context(outctx);

    return 0;
}

// void storage_worker_ffmpeg::run()
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


