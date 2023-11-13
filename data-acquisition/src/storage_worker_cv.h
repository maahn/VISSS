

// ============================================================================
//https://stackoverflow.com/questions/37140643/how-to-save-two-cameras-data-but-not-influence-their-picture-acquire-speed/37146523

// ============================================================================



class storage_worker_cv
{
public:
    storage_worker_cv(frame_queue& queue
        , int id
        , std::string path
        , int32_t fourcc
        , double fps
        , cv::Size frame_size
        , bool is_color
//        , double qualityƒ√
//        , double preset
        // , std::chrono::time_point<std::chrono::system_clock> t_reset
        , int live_window_frame_ratio);
    void run();

    double total_time_ms() const { return total_time_ / 1000.0; }

private:
    frame_queue& queue_;

    int id_;
    std::ofstream fMeta_;

    std::string path_;
    std::string filename_;
    std::string filename_latest_ ;
    std::string filename_final_ ;
    int32_t fourcc_;
    int fps_;
    cv::Size frame_size_;
    bool is_color_;
//    double quality_;
//    double preset_;
    double total_time_;
    // std::chrono::time_point<std::chrono::system_clock> t_reset;
    int live_window_frame_ratio_;
    unsigned long t_record;
    bool firstImage;
    bool fileUsed;
    bool statusFrame;

    // cv::VideoWriter writer_;
    FILE *pipeout;


    void add_meta_data(unsigned long timestamp);
    void close_files(unsigned long timestamp);
    void open_files(unsigned long timestamp, cv::Size imgSize);
    void create_filename(unsigned long timestamp);

};
// ----------------------------------------------------------------------------
storage_worker_cv::storage_worker_cv(frame_queue& queue
    , int id
    , std::string path
    , int32_t fourcc
    , double fps
    , cv::Size frame_size
    , bool is_color
//    , double quality
//    , double preset
    // , std::chrono::time_point<std::chrono::system_clock> t_reset
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
    , live_window_frame_ratio_(live_window_frame_ratio)
{
     PrintThread{} << "INFO-"<< id_ << " | " << get_timestamp() << " | Thread storage_worker_cv created" << std::endl;

}
// // ----------------------------------------------------------------------------


void storage_worker_cv::add_meta_data(unsigned long timestamp) 
{


    // std::chrono::milliseconds ms = duration_cast<std::chrono::milliseconds>(t_reset.time_since_epoch());
    // std::chrono::seconds s = duration_cast<std::chrono::seconds>(ms);
    // std::time_t t = s.count();
    // std::size_t fractional_seconds = ms.count() % 1000;
    // char *ctime_no_newline;
    // ctime_no_newline = strtok(ctime(&t), "\n");

    //0.2 with mean and standard deviation
    //0.3 with number of changing pixels
    //0.4 with last capture time

    std::time_t temp = timestamp/1e6;
    std::tm* t = std::gmtime(&temp);

    char timestampStr [80];
    strftime (timestampStr,80,"%Y%m%d-%H%M%S", t);


    fMeta_ << "# VISSS file format version: 0.4"<< "\n";
    fMeta_ << "# VISSS git tag: " << GIT_TAG
          <<  "\n";
    fMeta_ << "# VISSS git branch: " << GIT_BRANCH
          <<  "\n";
    fMeta_ << "# Camera reset time: " << timestampStr << "\n";
    fMeta_ << "# us since epoche: " 
          << timestamp << "\n";
    fMeta_ << "# Camera serial number: "    
          << DeviceIDMeta << "\n";
    fMeta_ << "# Camera configuration: "
          <<  configFile.substr(configFile.find_last_of("/\\") + 1)<< "\n";
    fMeta_ << "# Hostname: "
          <<  hostname<< "\n";
     
    fMeta_ << "# Capture time, Record time, Frame id, Queue Length";


    for(int ll = 0; ll < histSize; ll++) 
    {
        fMeta_ << ", " << std::to_string((int)range[ll]);
    }
    fMeta_ << "\n";
    return;

}


void storage_worker_cv::open_files(unsigned long timestamp, cv::Size imgSize) 
{
    create_filename(timestamp);

    if (storeVideo) {
        std::string ffmpegCommand = "ffmpeg -loglevel warning -y -f rawvideo ";
        ffmpegCommand += "-vcodec rawvideo -framerate ";
        ffmpegCommand += std::to_string(fps_);
        ffmpegCommand += " -pix_fmt gray -s ";
        ffmpegCommand += std::to_string(imgSize.width) + "x" + std::to_string(imgSize.height);
        ffmpegCommand += " -i - ";
        ffmpegCommand += encoding;
        ffmpegCommand += " -r "+ std::to_string(fps_);
        ffmpegCommand += " " +filename_ + ".mkv";
        
        pipeout = popen(ffmpegCommand.data(), "w");
        // writer_.open(filename_ + ".mkv", cv::CAP_FFMPEG, fourcc_, fps_, frame_size_, is_color_);
        PrintThread{} << "INFO-" << id_ << " | " << get_timestamp() << " | Started "<< ffmpegCommand<< std::endl;
        PrintThread{} << "DEBUG-" << id_ << " | " << get_timestamp() << " | Opened "<< filename_<< std::endl;
    }
    //writer_.open("appsrc ! videoconvert  ! timeoverlay ! queue ! x264enc speed-preset=veryfast mb-tree=true me=dia analyse=i8x8 rc-lookahead=20 subme=1 ! queue ! qtmux !  filesink location=video-h264_lookahead20.mkv",
    //writer_.open("appsrc ! videoconvert  ! timeoverlay ! queue ! x264enc speed-preset=superfast rc-lookahead=80 subme=2 ! queue ! qtmux !  filesink location=video-h264_lookahead80a_subme2.mkv",
    //                            cv::CAP_GSTREAMER, 0, fps_, frame_size_, is_color_);
// 
   fileUsed = false;

    if (storeMeta) {
        // Open the text file.
        fMeta_.open(filename_+".txt");
        add_meta_data(timestamp);
        PrintThread{} << "DEBUG-" << id_ << " | " << get_timestamp() << " | Opened "<< filename_+".txt"<< std::endl;
    }

    return;
}

void storage_worker_cv::close_files(unsigned long timestamp) {

    if (!firstImage) {
        fMeta_ << "# Last capture time: "
          << std::to_string(timestamp) << "\n";
        fMeta_.close();
        std::rename((filename_+".txt").c_str(), (filename_final_+".txt").c_str());
        fflush(pipeout);
        pclose(pipeout);
        if (fileUsed) {
            std::rename((filename_+".mkv").c_str(), (filename_final_+".mkv").c_str());
            PrintThread{} << "INFO-" << id_ << " | " << get_timestamp() << " | Written "<< filename_final_+".mkv"<< std::endl;
            create_symlink(filename_final_+".mkv",  filename_latest_+".mkv");
        } else if (storeVideo) {
            std::remove((filename_+".mkv").c_str());
            PrintThread{} << "INFO-" << id_ << " | " << get_timestamp() << " | Empty file removed: " << filename_<<".mkv" <<std::endl;
        }
    }
    //PrintThread{} << std::endl << "STATUS | " << get_timestamp() << " | All files closed. "<<std::endl;

    return;

}


void storage_worker_cv::create_filename(unsigned long timestamp) {

    std::string full_path;
    // std::time_t now_c = std::chrono::system_clock::to_time_t(t_reset);
    // std::tm now_tm* = *std::localtime(&now_c);
    int res = 0;
    // char timestamp1 [80];
    // strftime (timestamp1,80,"%Y/%m/%d", now_tm);
    // char timestamp2 [80];
    // strftime (timestamp2,80,"%Y%m%d-%H%M%S", now_tm);

    std::time_t temp = timestamp/1e6;
    std::tm* t = std::gmtime(&temp);

    char timestamp1 [80];
    strftime (timestamp1,80,"%Y/%m/%d", t);
    char timestamp2 [80];
    strftime (timestamp2,80,"%Y%m%d-%H%M%S", t);


//    std::string timestamp1 = serializeTimePoint(t_reset, "%Y/%m/%d");
//    std::string timestamp2 =serializeTimePoint(t_reset, "%Y%m%d-%H%M%S");


    if (name != "DRYRUN") {
        full_path = path_ + "/" + hostname + "_" + name + "_" + DeviceID + "/data/" + timestamp1 + "/" ;
        filename_ = path_ + "/tmp/" + hostname + "_" + name + "_" + DeviceID  + "_" + timestamp2 + "_" + std::to_string(id_);
        filename_final_ = full_path + hostname + "_" + name + "_" + DeviceID  + "_" + timestamp2 + "_" + std::to_string(id_);
    } else {
        full_path = path_  + "/" ;
        filename_ = path_ + "/tmp/" + DeviceIDMeta+ "_DRYRUN" + "_" + std::to_string(id_);
        filename_final_ = full_path + DeviceIDMeta+ "_DRYRUN" + "_" + std::to_string(id_);
    }
    filename_latest_ = path_ + "/" + name  + "_latest" + "_" + std::to_string(id_) ;

    res = mkdir_p((path_ + "/tmp/").c_str());
    if (res != 0) {
        std::cerr << "FATAL ERROR"<< id_ << " | " << get_timestamp() << " | Cannot create path "<< (path_ + "/tmp/").c_str() <<std::endl;
        global_error = true;
    }
    res = mkdir_p(full_path.c_str());
    if (res != 0) {
        std::cerr << "FATAL ERROR"<< id_ << " | " << get_timestamp() << " | Cannot create path "<< full_path.c_str() <<std::endl;
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
    

    int result;
    result = nice(-25);

    unsigned long  last_timestamp = 0;
    long int frame_count_new_file = 0;

    firstImage = true;
    std::string message;
    cv::Mat imgSmall;
    cv::Mat imgWithMeta;
    cv::Mat imgOld;
    cv::Mat imgDiff;
    cv::Mat nPixel;
    std::string old_string;
    std::string new_string;
    std::string textImg;
    std::time_t temp;
    std::tm* t;
    char timestampStr [80];
    std::string tempStr;
    int number_of_zeros;

    // boost::container::vector<bool>[histSize] movingPixels;
    bool movingPixels[histSize];
    float movingPixelThreshold;
    bool movingPixel = false;
    int tt = 0;
    cv::Scalar borderColor;
    
    PrintThread{} << "DEBUG-"<< id_ << " | " << get_timestamp() << " | Thread Running!" << std::endl;
    PrintThread{} << "DEBUG-"<< id_ << " | " << get_timestamp() << " | path " << path_ << std::endl;
    PrintThread{} << "DEBUG-"<< id_ << " | " << get_timestamp() << " | fourcc " << fourcc_ << std::endl;
    PrintThread{} << "DEBUG-"<< id_ << " | " << get_timestamp() << " | fps " << fps_ << std::endl;
    PrintThread{} << "DEBUG-"<< id_ << " | " << get_timestamp() << " | frame_size " << frame_size_ << std::endl;
    PrintThread{} << "DEBUG-"<< id_ << " | " << get_timestamp() << " | is_color " << is_color_ << std::endl;
    PrintThread{} << "DEBUG-"<< id_ << " | " << get_timestamp() << " | live_window_frame_ratio " << live_window_frame_ratio_ << std::endl;

    if ((id_ == 0) && showPreview) {
        cv::namedWindow( "VISSS Live Image | "+ name +" | "+std::to_string(id_), cv::WINDOW_AUTOSIZE | cv :: WINDOW_KEEPRATIO  );
    }

    try {
        int32_t frame_count(0);
        int32_t framesSinceLastStatus(0);
        
        for (;;) {

            MatMeta image(queue_.pop());
            if (!image.MatImage.empty()) {
                high_resolution_clock::time_point t1(high_resolution_clock::now());
                

                // t_record = t1.time_since_epoch().count()/1000;

                if (firstImage)  { 
                    imgOld = image.MatImage * 0;
                }   

                cv::absdiff(image.MatImage, imgOld, imgDiff);

                // get cummulative histogramm of differenc ebetween images
                bool uniform = false;
                bool accumulate = false;
                cv::calcHist( &imgDiff, 1, 0, cv::Mat(), nPixel, 1,&histSize, &histRange, uniform, accumulate );

                // Mat to array
                std::vector<float> nPixelA ;
                nPixelA.assign((float*)nPixel.data, (float*)nPixel.data + nPixel.total()*nPixel.channels());


                //PrintThread{} <<  "M1 = " << std::endl << " "  << nPixelA[0]<< " "<< nPixelA[1]<< " "<< nPixelA[2]<< " "<< nPixelA[3]<< " "<< nPixelA[4]<< " "<< nPixelA[5] << " "<< nPixelA[6]<< std::endl << std::endl;
                    //cumsum
                for (int ii = histSize-1; ii --> 0; )
                {
                     nPixelA[ii] = nPixelA[ii] + nPixelA[ii+1];
                }

                movingPixel = false;
                tt = 1;
                for (uint ll = 0; ll < nPixelA.size(); ll++) {
                    movingPixels[ll] = false;
                    movingPixelThreshold = minMovingPixel/tt;
                    if (movingPixelThreshold < 2) {
                        movingPixelThreshold = 2;
                    }
                    if (nPixelA[ll] >= movingPixelThreshold) {
                            movingPixels[ll] = true;
                            movingPixel = true;
                    }
//PrintThread{} << "ll "<< ll << " |tt "<< tt << " |movingPixelThreshold "<< movingPixelThreshold << " |nPixelA "<< nPixelA[ll] << " |movingPixels " << movingPixels[ll] ;


                    tt = tt*2;

                }
//PrintThread{} << std::endl;

                //PrintThread{} <<  "M2 = " << std::endl << " "  << nPixelA[0]<< " "<< nPixelA[1]<< " "<< nPixelA[2]<< " "<< nPixelA[3]<< " "<< nPixelA[4]<< " "<< nPixelA[5] << " "<< nPixelA[6]<< std::endl << std::endl;

                imgOld = image.MatImage.clone();
                //rotate image if required
                if (rotateImage) {
                    cv::rotate(image.MatImage, image.MatImage, cv::ROTATE_90_CLOCKWISE);
                }
                //for (int tt : thresholds) {
                //    nPixel[tt] = cv::sum(imgDiff > thresholds[tt])[0];
                //} https://webcache.googleusercontent.com/search?q=cache:iUCC_CSnaLwJ:https://answers.opencv.org/question/60753/counting-black-white-pixels-with-a-threshold/+&cd=1&hl=de&ct=clnk&gl=de
                if (site == "none") {
                    textImg = "";
                } else {
                    textImg = site + " | ";
                }
                temp = image.timestamp/1e6;
                t = std::gmtime(&temp);
                strftime (timestampStr,80,"%Y/%m/%d %H:%M:%S", t);
                temp = std::round((image.timestamp%1000000) / 1e4);
                tempStr = std::to_string((int)temp);
                number_of_zeros = 2 - tempStr.length();
                if (number_of_zeros > 0) {
                    tempStr = tempStr.insert(0, number_of_zeros, '0');
                    }
                textImg = textImg + timestampStr + '.'+ tempStr +" | " + name + 
                    " | Q:" + std::to_string(queue_.size()) + " | M: ";
                for (int jj = histSize; jj --> 0; )
                {
                     if (movingPixels[jj]) 
                     {
                        old_string = std::to_string((int)range[jj]);
                        new_string = std::string(3 - old_string.length(), '0') + old_string;
                        textImg = textImg + new_string;
                        break;
                     }
                }
                if (movingPixel  || firstImage)
                     {
                        borderColor = ( 0 );
                        
                     }
                 else
                     {
                        borderColor = ( 100 );
                        textImg = textImg + "NOT RECORDING";
                     }

                 textImg = textImg+ " | " + std::to_string(id_);
                 if (queryGain) {
                     textImg = textImg + " | E" + std::to_string((int)image.ExposureTime);
                     textImg = textImg + "G" + std::to_string((int)image.Gain);
                    }
                cv::copyMakeBorder(image.MatImage, imgWithMeta, frameborder, 0, 0, 0, cv::BORDER_CONSTANT, borderColor );
                
                cv::putText(imgWithMeta, 
                        textImg,
                        cv::Point(20,40), // Coordinates
                        cv::FONT_HERSHEY_PLAIN, // Font
                        1.8, // Scale. 2.0 = 2x bigger
                        cv::Scalar(255), // BGR Color
                        1, // Line Thickness (Optional)
                        cv::LINE_AA); // Anti-alias (Optional)


                if (firstImage || image.newFile)
                {


                    close_files(last_timestamp);
                    open_files(image.timestamp, imgWithMeta.size());

                    if (storeVideo) {
                        cv::imwrite(filename_final_+".jpg", imgWithMeta );
                        create_symlink(filename_final_+".jpg",  filename_latest_+".jpg");
                        PrintThread{} ;
                        PrintThread{} << "DEBUG-" << id_ << " | " << get_timestamp() << " | Written "<< filename_final_+".jpg"<< std::endl;
                        }

                    frame_count_new_file = frame_count;

                }

                // store a frame every 10 s in thread 0 to have data about clock and capture_id drifts
                //statusFrame = (frame_count % ((int)fps_ *10) == 0) && (id_ == 0);
                statusFrame = ((image.timestamp/(int)1e6)%10 == 0) && (framesSinceLastStatus > (1.5*fps_))  && (id_ == 0);
                if (statusFrame) {
                    framesSinceLastStatus = 0;
                }
                if (writeallframes || movingPixel  || firstImage || image.newFile || statusFrame )
                     {
                        if (storeVideo) { 
                            // writer_.write(imgWithMeta);
                            size_t sizeInBytes = imgWithMeta.step[0] * imgWithMeta.rows;
                            fwrite(imgWithMeta.data, 1, sizeInBytes, pipeout);
                            fileUsed = true;
                            }
                        if (storeMeta) {
                            message = std::to_string(image.timestamp)
                                + ", " + std::to_string(image.recordtime)
                                + ", " + std::to_string(image.id)
                                + ", " + std::to_string(queue_.size()) ;

                            for(int kk = 0; kk < histSize; kk++) 
                            {
                                message = message + ", " + std::to_string((int)nPixelA[kk]);
                            }
                            message = message + "\n";
                            
                            fMeta_ << message;
                        }
                }
                if (frame_count % (int)fps_ == 0) {

                    message =  "STATUS" + std::to_string(id_) +" | " + get_timestamp() + 
                    " | Queue:" + std::to_string(queue_.size()) +" | ID:" + 
                    std::to_string(image.id) +  " | M: ";
                    for (int jj = histSize; jj --> 0; )
                    {
                         if (movingPixels[jj]) 
                         {
                            message = message + std::to_string((int)range[jj]);
                            break;
                         }
                    }
                    if (queryGain) {
                        message = message + " | E: " + std::to_string((int)image.ExposureTime);
                        message = message + " | G: " + std::to_string((int)image.Gain);
                    }
                    PrintThread{} << message<<std::endl;
                    std::cout << std::flush;
                }
                if ( (id_ == 0) && showPreview && (frame_count % (live_window_frame_ratio_ / nStorageThreads) == 0))
                {
                    
                    cv::resize(imgWithMeta, imgSmall, cv::Size(), 0.5, 0.5);
                    cv::imshow( "VISSS Live Image | "+ name +" | "+std::to_string(id_), imgSmall );
                    cv::waitKey(1);
                    }
 

                firstImage = false;
                ++frame_count;
                ++framesSinceLastStatus;
                last_timestamp = image.timestamp; 

                high_resolution_clock::time_point t2(high_resolution_clock::now());
                double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
                total_time_ += dt_us;

                


                // PrintThread{} << "Worker " << id_ << " stored imgWithMeta #" << frame_count
                //     << " in " << (dt_us / 1000.0) << " ms" << std::endl;
            }



        }
    } catch (frame_queue::cancelled& /*e*/) {
        // Nothing more to process, we're done
        PrintThread{} << "INFO-" << id_ << " | " << get_timestamp() << " | Storage queue " << id_ << " cancelled, storage worker finished. Closing files." << std::endl;
        close_files(last_timestamp);
    }
}



