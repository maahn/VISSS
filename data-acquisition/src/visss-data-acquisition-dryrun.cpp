//============================================================================

#include "visss-data-acquisition.h"   
//============================================================================

#include "frame_queue.h"   
#include "storage_worker_cv.h"   
//#include "storage_worker_cv.h"   


// using namespace cv;
// using namespace std;

#define OPENCV_WINDOW_NAME	"VISSS Live Image"


const char* params
    = "{ help h           |                   | Print usage }"
      "{ output o         | ./                | Output Path }"
      "{ quality q        | 16                | quality 0-51 }"
      "{ preset p         | veryfast          | preset (ultrafast - placebo) }"
      "{ liveratio l      | 70                | every Xth frame will be displayed in the live window }"
      "{ fps f            | 140               | frames per seconds of output }"
      "{ maxframes m      | -1                | stop after this many frames (for debugging) }"
      "{ writeallframes w |                   | write all frames whether sth is moving or not (for debugging) }"
      "{ @videofile       | <none>            | video file }";

// ====================================


char GetKey()
{
    char key = getchar();
    while ((key == '\r') || (key == '\n'))
    {
        key = getchar();
    }
    return key;
}

void PrintMenu()
{
    std::cout << "**************************************************************************" << std::endl;
    std::cout << "press [Q] or [ESC] to end" << std::endl;
    std::cout << "**************************************************************************" << std::endl;
}

typedef struct tagMY_CONTEXT
{
    cv::VideoCapture       fileHandle;
    std::string             base_name;
    int                     enable_sequence;
    int                     enable_save;
    int                     live_window_frame_ratio;
    double                     fps;
    std::string                  quality;
    std::string                  preset;
    std::chrono::time_point<std::chrono::system_clock> t_reset;
    BOOL              exit;
} MY_CONTEXT, *PMY_CONTEXT;



void *ImageCaptureThread( void *context)
{
    MY_CONTEXT *captureContext = (MY_CONTEXT *)context;
    bool was_active = FALSE;


    if (captureContext != NULL)
    {
        int sequence_init = 0;
        unsigned int sequence_count = 0;
        
        int OpenCV_Type = 0;
        OpenCV_Type = CV_8UC1;
        //int codec = cv:: VideoWriter::fourcc('H', '2', '6', '4'); // select desired codec (must be available at runtime)
        int codec = cv:: VideoWriter::fourcc('a', 'v', 'c', '1'); // select desired codec (must be available at runtime)
        bool isColor = FALSE;

        // The synchronized queues, one per video source/storage worker pair
        std::vector<frame_queue> queue(1);

        // Let's create our storage workers -- let's have two, to simulate your scenario
        // and to keep it interesting, have each one write a different format
        std::vector <storage_worker_cv> storage;
        std::vector<std::thread> storage_thread;

        double total_read_time(0.0);
        int32_t frame_count(0);

        uint last_id = 0;
        uint last_timestamp = 0;

        unsigned short id = 0; //TODO: ADD id from ascii file;

        cv::Mat exportImg;

        // While we are still running.
        while(!captureContext->exit)
        {

            // Wait for images to be received
            captureContext->fileHandle >> exportImg;



            if (!exportImg.empty())
            {

                    was_active = TRUE;

                    // id max number is 65535
                    if ((last_id>0) && (id != last_id+1)  && (last_id != 65535) ){
                        std::cout << std::endl << "ERROR | " << get_timestamp() << " | missed frames between " << last_id << " and " << id << std::endl;
                    }


                    last_id = id; 
                    last_timestamp = 9999; //TODO: ADD timestamp from ascii file img->timestamp;
                    if ((captureContext->enable_sequence) || (sequence_init == 1))
                    {

                            high_resolution_clock::time_point t1(high_resolution_clock::now());

                        // Export to OpenCV Mat object using SapBuffer data directly
                        cv::Size imgSize = exportImg.size();

                        //cv::Size imgSize = exportImg.size();
                        //std:: cout << "STATUS | " << get_timestamp() << "| " << imgSize  << std::endl;

                        MatMeta exportImgMeta;


                        if (!sequence_init)
                        {
                        // init

                            //--- INITIALIZE VIDEOWRITER

                            storage.emplace_back(std::ref(queue[0]), 0
                                , captureContext->base_name
                                , codec
                                , captureContext->fps
                                , imgSize
                                , isColor
                                //, captureContext->quality
                                //, captureContext->preset
                                , captureContext->t_reset
                                , captureContext->live_window_frame_ratio
                                );
                            //std:: cout << "STATUS | " << get_timestamp() << "| storage worker started" << std::endl;

                            // And start the worker threads for each storage worker
                            for (auto& s : storage) {
                                storage_thread.emplace_back(&storage_worker_cv::run, &s);
                            }


                            sequence_init = 1;
                            sequence_count = 0;
                            // }
                        }
                        // else if (writer.isOpened())
                        // {
                            // cout << filename << ": FILE OPEN\n";


                        // Now the main capture loop

                        cv::Mat croppedImg = exportImg(cv::Rect(0,frameborder,imgSize.width,imgSize.height-frameborder)).clone();
                        cv::cvtColor(croppedImg, exportImgMeta.MatImage, cv::COLOR_BGR2GRAY);

                        exportImgMeta.timestamp = last_timestamp;
                        exportImgMeta.id = id;

                        // Insert a copy into all queues
                        for (auto& q : queue) {
                            q.push(exportImgMeta);
                        }        

                        high_resolution_clock::time_point t2(high_resolution_clock::now());
                         double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
                        total_read_time += dt_us;

                        // std::cout << "Captured image #" << frame_count << " in "
                        //     << (dt_us / 1000.0) << " ms" << std::endl;
                    

                        fflush(stdout);


                        ++frame_count;

                        sequence_count++;

                        //rest n_timeout after success
                        n_timeouts = 0;

                        if ((maxframes>0) && (frame_count >=maxframes)) {
                            std::cout << "FATAL ERROR | " << get_timestamp() << " | Reached maximum number of frames" << std::endl;
                            global_error = true;
                        }

                        if ( !captureContext->enable_sequence )
                        {
                            std::cout << "STATUS | " << get_timestamp() << " | Complete sequence has " << sequence_count << " frames" << std::endl;
                            sequence_count = 0;
                            sequence_init = 0;

                            // writer.release();

                        }

                    }

                    else
                    {
                        printf("WAITING Frame %llu\r", (unsigned long long)id);
                        fflush(stdout);
                    }
                
            }

            else {
                n_timeouts += 1;
                std::cerr << "ERROR | " << get_timestamp() <<" | Could not get image " 
                << " #" << n_timeouts << std::endl;
            }

            // See if a sequence in progress needs to be stopped here.
            if (global_error || ((!captureContext->enable_sequence) && (sequence_init == 1)))
            {
                std::cout << "STATUS | " << get_timestamp() << " | Complete sequence has " << sequence_count << "frames" << std::endl;
                sequence_count = 0;
                sequence_init = 0;
            }



            if (global_error) {
                captureContext->enable_sequence = 0;
                captureContext->exit = 1;
                std::cerr << "FATAL ERROR | " << get_timestamp() << " | Error detected. Exiting capture loop..." <<std::endl;

            }
            id++;

            // don'be too fast in dry run mode
            int sleeptime = 1000/captureContext->fps;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleeptime));



        } //while

        if (was_active) {

                // We're done reading, cancel all the queues
            for (auto& q : queue) {
                q.cancel();
            }

            // And join all the worker threads, waiting for them to finish
            storage_thread[0].join();
            // for (auto& st2 : storage_thread) {
            //     st2.join();
            // }
            // Report the timings
            total_read_time /= 1000.0;
            double total_storage_time(storage[0].total_time_ms());
            //double total_write_time_a(storage[0].storage[0].total_time_ms());
            // double total_write_time_b(storage[1].total_time_ms());

            std::cout << "STATUS | " << get_timestamp() 
                << " | Completed storage " << frame_count << " images:\n"
                << "  average capture time = " << (total_read_time / frame_count) << " ms\n"
                << "  average storage time = " << (total_storage_time / frame_count) << " ms\n"
                //<< "  average write time A = " << (total_write_time_a / frame_count) << " ms\n"
                ;
        }

    }
    pthread_exit(0);

}


int main(int argc, char** argv)
{
cv::String videoFileIn;
std::string videoFileInRaw;
MY_CONTEXT context ;
    pthread_t  tid;
    char c;

    //============================================================================
    // Greetings
    std::cout << "VISSS data acquisition (" << __DATE__ << ")" << std::endl;
    std::cout << "**************************************************************************" << std::endl;

    cv :: CommandLineParser parser(argc, argv, params);

    if (parser.has("help"))
    {
        parser.printMessage();
        return 0;
    }
    cv::String output = parser.get<cv::String>("output");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: Output path "<< output << std::endl;

    videoFileIn = parser.get<cv::String>(0);
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: Video file "<< videoFileIn << std::endl;
    videoFileInRaw = videoFileIn.substr(0, videoFileIn.find_last_of("."));
    size_t sep = videoFileInRaw.find_last_of("\\/");
    if (sep != std::string::npos)
        videoFileInRaw = videoFileInRaw.substr(sep + 1, videoFileInRaw.size() - sep - 1);


    context.quality = parser.get<cv::String>("quality");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: FFMPEG Quality "<< context.quality << std::endl;
    context.preset = parser.get<cv::String>("preset");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: FFMPEG preset "<< context.preset << std::endl;

    context.live_window_frame_ratio = parser.get<int>("liveratio");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: liveratio "<< context.live_window_frame_ratio << std::endl;

    context.fps = parser.get<double>("fps");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: fps "<< context.fps << std::endl;

    maxframes = parser.get<int>("maxframes");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: maxframes "<< maxframes << std::endl;

    if (parser.has("writeallframes"))
    {
        writeallframes = true;
    }
    else {
        writeallframes = false;
    }

    std::set<std::string> presets = {
        "ultrafast",
        "superfast",
        "veryfast",
        "faster",
        "fast",
        "medium",
        "slow",
        "slower",
        "placebo",
    };

    if (presets.find(context.preset) == presets.end()){
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| Do not know preset " << context.preset<< std::endl;
        global_error = true;
    }


    char OPENCV_FFMPEG_PRESET[100];
    strcpy(OPENCV_FFMPEG_PRESET,"OPENCV_FFMPEG_PRESET=");
    strcat(OPENCV_FFMPEG_PRESET,context.preset.c_str());
    char OPENCV_FFMPEG_CRF[100];
    strcpy(OPENCV_FFMPEG_CRF,"OPENCV_FFMPEG_CRF=");
    strcat(OPENCV_FFMPEG_CRF,context.quality.c_str());
    char OPENCV_FFMPEG_THREADCOUNT[100];
    strcpy(OPENCV_FFMPEG_THREADCOUNT,"OPENCV_FFMPEG_THREADCOUNT=24"); // don't ask me why overloading the CPUs works better...
    //strcat(OPENCV_FFMPEG_THREADCOUNT,context.quality.c_str());


    if(putenv(OPENCV_FFMPEG_PRESET)!=0)
    {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| putenv failed: " << OPENCV_FFMPEG_PRESET<< std::endl;
        exit(1);
    }
    if(putenv(OPENCV_FFMPEG_CRF)!=0)
    {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| putenv failed: " << OPENCV_FFMPEG_CRF<< std::endl;
        global_error = true;
    }

    //if(putenv(OPENCV_FFMPEG_THREADCOUNT)!=0)
    //{
    //    std::cerr << "FATAL ERROR | " << get_timestamp() << "| putenv failed: " << OPENCV_FFMPEG_THREADCOUNT<< std::endl;
    //    global_error = true;
    //}

    gethostname(hostname, HOST_NAME_MAX);

    configFile = "DRYRUN";
    configFileRaw = "DRYRUN";
    strcpy(DeviceID, "DUMMY");

std::cout << "**************************************************************************" << std::endl;


    try {
        context.fileHandle  = cv::VideoCapture(videoFileIn);
    }
    catch (const char*) {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| Cannot open  " << videoFileIn<< std::endl;
        return 1;
    }
    if( !context.fileHandle.isOpened() ) {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| Cannot open2  " << videoFileIn<< std::endl;
        return 1;
    }

                context.t_reset = std::chrono::system_clock::now();
                        // Create a thread to receive images from the API and save them
                        context.base_name = output;
                        context.exit = FALSE;
                        pthread_create(&tid, NULL, ImageCaptureThread, &context);

                        // Call the main command loop or the example.
                        PrintMenu();



                        context.enable_sequence = 1;

                       struct sigaction sigIntHandler;
                       sigIntHandler.sa_handler = signal_handler;
                       sigemptyset(&sigIntHandler.sa_mask);
                       sigIntHandler.sa_flags = 0;
                       sigaction(SIGINT, &sigIntHandler, NULL);
                       sigaction(SIGTERM, &sigIntHandler, NULL);

                       struct sigaction sigIntHandler_null;
                       sigIntHandler_null.sa_handler = signal_handler_null;
                       sigemptyset(&sigIntHandler_null.sa_mask);
                       sigIntHandler_null.sa_flags = 0;
                       sigaction(SIGALRM, &sigIntHandler_null, NULL);


                        while(!(global_error) && (!done))
                        {
                            alarm(1);
                            c = GetKey();

                            if ((c == 0x1b) || (c == 'q') || (c == 'Q'))
                            {
                                done = TRUE;
                           }
                        }

                        context.enable_sequence = 0; // End sequence if active.

                        context.exit = TRUE;



}




