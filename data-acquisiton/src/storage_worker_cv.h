

// ============================================================================
//https://stackoverflow.com/questions/37140643/how-to-save-two-cameras-data-but-not-influence-their-picture-acquire-speed/37146523

// ============================================================================



class storage_worker_cv
{
public:
    storage_worker_cv(frame_queue& queue
        , int32_t id
        , std::string const& path
        , int32_t fourcc
        , double fps
        , cv::Size frame_size
        , bool is_color
//        , double qualityƒ√
//        , double preset
        , std::chrono::time_point<std::chrono::system_clock> t_reset
        , int live_window_frame_ratio);
    void run();

    double total_time_ms() const { return total_time_ / 1000.0; }

private:
    frame_queue& queue_;

    int32_t id_;
    std::ofstream fMeta_;

    std::string path_;
    std::string filename_;
    std::string filename_latest_ ;
    int32_t fourcc_;
    double fps_;
    cv::Size frame_size_;
    bool is_color_;
//    double quality_;
//    double preset_;
    double total_time_;
    std::chrono::time_point<std::chrono::system_clock> t_reset_;
    int live_window_frame_ratio_;
    unsigned long t_reset_uint_;
    bool firstImage;


    cv::VideoWriter writer_;

    void add_meta_data();
    void close_files();
    void open_files();
    void create_filename();

};
// ----------------------------------------------------------------------------
storage_worker_cv::storage_worker_cv(frame_queue& queue
    , int32_t id
    , std::string const& path
    , int32_t fourcc
    , double fps
    , cv::Size frame_size
    , bool is_color
//    , double quality
//    , double preset
    , std::chrono::time_point<std::chrono::system_clock> t_reset
    , int live_window_frame_ratio)    :
      queue_(queue)
    , id_(id)
    , path_(path)
    , fourcc_(fourcc)
    , fps_(fps)
    , frame_size_(frame_size)
    , is_color_(is_color)
    , total_time_(0.0)
//    , quality_(quality)
//    , preset_(preset)
    , t_reset_(t_reset)
    , live_window_frame_ratio_(live_window_frame_ratio)
{
}
// // ----------------------------------------------------------------------------


void storage_worker_cv::add_meta_data() 
{


    std::chrono::milliseconds ms = duration_cast<std::chrono::milliseconds>(t_reset_.time_since_epoch());
    std::chrono::seconds s = duration_cast<std::chrono::seconds>(ms);
    std::time_t t = s.count();
    std::size_t fractional_seconds = ms.count() % 1000;
    char *ctime_no_newline;
    ctime_no_newline = strtok(ctime(&t), "\n");
    fMeta_ << "# VISSS file format version: 0.2"<< "\n";
    fMeta_ << "# VISSS git tag: " << GIT_TAG
          <<  "\n";
    fMeta_ << "# VISSS git branch: " << GIT_BRANCH
          <<  "\n";

    fMeta_ << "# Camera start time: " << ctime_no_newline << ' '
            << fractional_seconds  << "\n";
    fMeta_ << "# us since epoche: " 
          << t_reset_uint_ << "\n";
    
    fMeta_ << "# Camera serial number: "
          << DeviceID << "\n";
     
    fMeta_ << "# Camera configuration: "
          <<  configFileRaw<< "\n";

    fMeta_ << "# Hostname: "
          <<  hostname<< "\n";
     
    fMeta_ << "# Capture time, Record time, Frame id, x, x \n";
    return;

}


void storage_worker_cv::open_files() 
{
    create_filename();

    std::cout << std::flush;
    writer_.open(filename_+".mov", cv::CAP_FFMPEG, fourcc_, fps_, frame_size_, is_color_);
    //writer_.open("appsrc ! videoconvert  ! timeoverlay ! queue ! x264enc speed-preset=veryfast mb-tree=TRUE me=dia analyse=i8x8 rc-lookahead=20 subme=1 ! queue ! qtmux !  filesink location=video-h264_lookahead20.mov",
    //writer_.open("appsrc ! videoconvert  ! timeoverlay ! queue ! x264enc speed-preset=superfast rc-lookahead=80 subme=2 ! queue ! qtmux !  filesink location=video-h264_lookahead80a_subme2.mov",
    //                            cv::CAP_GSTREAMER, 0, fps_, frame_size_, is_color_);
// 

   std::cout << "STATUS | " << get_timestamp() << " | Opened "<< filename_<< std::endl;


    // Open the text file.
    fMeta_.open(filename_+".txt");
    add_meta_data();
    std::cout << "STATUS | " << get_timestamp() << " | Opened "<< filename_+".txt"<< std::endl;


    return;
}

void storage_worker_cv::close_files() {


    fMeta_.close();
    writer_.release();
    create_symlink(filename_+".mov",  filename_latest_+".mov");


    std::cout << std::endl << "STATUS | " << get_timestamp() << " | All files closed. "<<std::endl;

    return;

}


void storage_worker_cv::create_filename() {

    time_t t = time(0);   // get time now
    struct tm * now = localtime( & t );
    int res = 0;
    char timestamp1 [80];
    strftime (timestamp1,80,"%Y/%m/%d",now);
    char timestamp2 [80];
    strftime (timestamp2,80,"%Y%m%d-%H%M%S",now);


    std::string full_path = path_ + "/" + hostname + "_" + configFileRaw + "_" + DeviceID + "/data/" + timestamp1 + "/" ;

    filename_ = full_path + hostname + "_" + configFileRaw + "_" + DeviceID  + "_" + timestamp2;
    filename_latest_ = path_ + "/" + hostname + "_" + configFileRaw + "_" + DeviceID + "_latest" ;

    res = mkdir_p(full_path.c_str());
    if (res != 0) {
        std::cerr << "FATAL ERROR | " << get_timestamp() << " | Cannot create path "<< full_path.c_str() <<std::endl;
        global_error = true;
    }

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

void storage_worker_cv::run() 

{
    nice(-25);


    long int timestamp = 0;
    long int frame_count_new_file = 0;
    firstImage = TRUE;
    t_reset_uint_ = t_reset_.time_since_epoch().count()/1000;
    int fps_int = cvCeil(fps_);
    cv::Mat imgSmall;
    cv::Mat imgWithMeta;
    cv::Mat imgOld;
    cv::Mat imgDiff;
    cv::Mat img1Chan;
    int threshold;
    //int [sizeof(thresholds)]= { 0 };
    cv::Mat nPixel;

    
    cv::namedWindow( "VISSS Live Image", cv::WINDOW_AUTOSIZE | cv :: WINDOW_KEEPRATIO  );


    try {
        int32_t frame_count(0);
        for (;;) {
            MatMeta image(queue_.pop());
            if (!image.MatImage.empty()) {
                high_resolution_clock::time_point t1(high_resolution_clock::now());
                

                cv::extractChannel(image.MatImage, img1Chan, 0);

                if (firstImage)  { 
                    imgOld = img1Chan * 0;
                }   

                cv::absdiff(img1Chan, imgOld, imgDiff);


                // get histogramm
                float range[] = {2,4,6,8,10,20,30,40,60,80,100,256  }; //the upper boundary is exclusive
                int histSize = 7;
                const float* histRange = { range };
                bool uniform = false;
                bool accumulate = false;
                cv::calcHist( &imgDiff, 1, 0, cv::Mat(), nPixel, 1,&histSize, &histRange, uniform, accumulate );

                // Mat to array
                std::vector<float> nPixelA;
                nPixelA.assign((float*)nPixel.data, (float*)nPixel.data + nPixel.total()*nPixel.channels());

                //std::cout <<  "M1 = " << std::endl << " "  << nPixelA[0]<< " "<< nPixelA[1]<< " "<< nPixelA[2]<< " "<< nPixelA[3]<< " "<< nPixelA[4]<< " "<< nPixelA[5] << " "<< nPixelA[6]<< std::endl << std::endl;
                    //cumsum
                for (int ii = histSize-1; ii --> 0; )
                {
                     nPixelA[ii] = nPixelA[ii] + nPixelA[ii+1];
                }

                //std::cout <<  "M2 = " << std::endl << " "  << nPixelA[0]<< " "<< nPixelA[1]<< " "<< nPixelA[2]<< " "<< nPixelA[3]<< " "<< nPixelA[4]<< " "<< nPixelA[5] << " "<< nPixelA[6]<< std::endl << std::endl;




                //for (int tt : thresholds) {
                //    nPixel[tt] = cv::sum(imgDiff > thresholds[tt])[0];
                //} https://webcache.googleusercontent.com/search?q=cache:iUCC_CSnaLwJ:https://answers.opencv.org/question/60753/counting-black-white-pixels-with-a-threshold/+&cd=1&hl=de&ct=clnk&gl=de

                cv::copyMakeBorder(image.MatImage, imgWithMeta, frameborder, 0, 0, 0, cv::BORDER_CONSTANT, 0 );
                std::string textImg = get_timestamp() + " | " + configFileRaw + " | Q:" + std::to_string(queue_.size()) + " | M: ";
                for (int jj = histSize; jj --> 0; )
                {
                     if (nPixelA[jj]>0) 
                     {
                        textImg = textImg + std::to_string((int)range[jj]);
                        break;
                     }
                }

                cv::putText(imgWithMeta, 
                        textImg,
                        cv::Point(20,50), // Coordinates
                        cv::FONT_HERSHEY_PLAIN, // Font
                        2, // Scale. 2.0 = 2x bigger
                        cv::Scalar(255), // BGR Color
                        2, // Line Thickness (Optional)
                        cv::LINE_AA); // Anti-alias (Optional)


                fMeta_  <<image.timestamp +t_reset_uint_ << ", " << t1.time_since_epoch().count()/1000
                          << ", " << image.id << ", " << 0 << ", " << 0 << "\n";

                ++frame_count;
                timestamp = static_cast<long int> (time(NULL));


                if (firstImage || ((timestamp % new_file_interval == 0) && (frame_count-frame_count_new_file > 300)))
                {

                    close_files();
                    open_files();

                    cv::imwrite(filename_+".jpg", imgWithMeta );
                    create_symlink(filename_+".jpg",  filename_latest_+".jpg");

                    frame_count_new_file = frame_count;

                    std::cout ;
                    std::cout << "STATUS | " << get_timestamp() << " | Written "<< filename_+".jpg"<< std::endl;

                }

                writer_.write(imgWithMeta);

                if (frame_count % fps_int == 0)
                {

                    double min, max;
                    minMaxIdx(imgDiff, &min, &max);

                    std::cout << "STATUS | " << get_timestamp() << 
                    " | Queue:" << queue_.size() << 
                    //" | " << nPixelA <<
                    "  \r"<<std::flush;
                    // "  \r"<<std::endl;


               }


                cv::extractChannel(image.MatImage.clone(), imgOld, 0);

                if (frame_count % live_window_frame_ratio_ == 0)
                {
                    
                    cv::resize(imgWithMeta, imgSmall, cv::Size(), 0.5, 0.5);


                    cv::imshow( "VISSS Live Image", imgSmall );
                    cv::waitKey(1);
                }
 


                high_resolution_clock::time_point t2(high_resolution_clock::now());
                double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
                total_time_ += dt_us;

                
                firstImage = FALSE;



                // std::cout << "Worker " << id_ << " stored imgWithMeta #" << frame_count
                //     << " in " << (dt_us / 1000.0) << " ms" << std::endl;
            }
        }
    } catch (frame_queue::cancelled& /*e*/) {
        // Nothing more to process, we're done
        std::cout << "Storage queue " << id_ << " cancelled, storage worker finished. Closing files." << std::endl;
        close_files();
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


