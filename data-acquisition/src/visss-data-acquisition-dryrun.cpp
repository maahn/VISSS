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
    = "{ help h            |                   | Print usage }"
      "{ output o          | ./                | Output Path }"
      "{ site s            | none              | site string }"
      "{ encoding e        | -c:v@libx264      | ffmpeg encoding options with '@' replacing ' '}"
      "{ liveratio l       | 70                | every Xth frame will be displayed in the live window }"
      "{ fps f             | 140               | frames per seconds of output }"
      "{ newfileinterval i | 300               | write new file very ?s. Set to 0 to deactivate}"
      "{ maxframes m       | -1                | stop after this many frames (for debugging) }"
      "{ writeallframes w  |                   | write all frames whether sth is moving or not (for debugging) }"
      "{ followermode d    | 0                 | do not complain about camera timeouts }"
      "{ nopreview         |                   | no preview window }"
      "{ minBrightChange b | 20                | minimum brightnes change to start recording [20,30] }"
      "{ querygain q       | 0                 | query gain and bightness [0,1]}"
      "{ novideo           |                   | do not store video data }"
      "{ nometadata        |                   | do not store meta data }"
      "{ threads t         | 1                 | number of storage threads }"
      "{ @videofile        | <none>            | video input file }";

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
    std::string            csvFile;
    std::string             base_name;
    int                     enable_sequence;
    int                     enable_save;
    int                     live_window_frame_ratio;
    double                     fps;
    std::string                  quality;
    std::string                  preset;
    bool              exit;
} MY_CONTEXT, *PMY_CONTEXT;



void *ImageCaptureThread( void *context)
{
    MY_CONTEXT *captureContext = (MY_CONTEXT *)context;
    bool was_active = false;
    printf("THREAD1 ImageCaptureThread\n");



    if (captureContext != NULL)
    {
        int sequence_init = 0;
        unsigned int sequence_count = 0;
        
        int OpenCV_Type = 0;
        OpenCV_Type = CV_8UC1;
        //int codec = cv:: VideoWriter::fourcc('H', '2', '6', '4'); // select desired codec (must be available at runtime)
        int codec = cv:: VideoWriter::fourcc('a', 'v', 'c', '1'); // select desired codec (must be available at runtime)
        bool isColor = false;

        // The synchronized queues, one per video source/storage worker pair
        std::vector<frame_queue> queue(1);

        // Let's create our storage workers -- let's have two, to simulate your scenario
        // and to keep it interesting, have each one write a different format
        std::vector <storage_worker_cv> storage;
        std::vector<std::thread> storage_thread;

        double total_read_time(0.0);
        int32_t frame_count(0);

        uint ascii_id = 0;
        unsigned long ascii_timestamp = 0;

        unsigned short id = 0; //TODO: ADD id from ascii file;

        std::ifstream csvHandle(captureContext->csvFile);


        cv::Mat exportImg;

        // While we are still running.
        while(!captureContext->exit)
        {

            // Wait for images to be received
            captureContext->fileHandle >> exportImg;
            // get time for recordtime timestamp
            high_resolution_clock::time_point tr(high_resolution_clock::now());


            if (!exportImg.empty())
            {

                    was_active = true;


                    // skip comment
                    std::string csvLine;
                    std::getline(csvHandle, csvLine);   
                     while (csvLine.rfind("#", 0) == 0) {
                        std::getline(csvHandle, csvLine); 
                     }
                    // copy data form ascii file
                    std::vector<std::string> csvLines;
                    boost::split(csvLines, csvLine, boost::is_any_of(","));
                    ascii_id = atoi(trim(csvLines[2]).c_str());

                    ascii_timestamp = atol(trim(csvLines[0]).c_str());

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
                                // , captureContext->t_reset
                                , captureContext->live_window_frame_ratio
                                );
                            //std:: cout << "STATUS | " << get_timestamp() << "| storage worker started" << std::endl;

                            // And start the worker threads for each storage worker
                            for (auto& s : storage) {
                                    printf("THREAD1 storage_thread.emplace_back\n");
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

                        exportImgMeta.timestamp = ascii_timestamp;
                        exportImgMeta.recordtime = tr.time_since_epoch().count()/1000;
                        exportImgMeta.id = ascii_id;

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
                std::cerr << "STATUS | " << get_timestamp() <<" | Arrived at the end of the file" << std::endl;
                captureContext->exit = 1;
                captureContext->enable_sequence = 0;
                global_error = true;
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

            if (storeVideo) {
                // don'be too fast in dry run mode
                int sleeptime = 1000/captureContext->fps;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleeptime));
            }


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
    int res = 0;
    int writeallframes1;
    int queryGain1;
    int followermode1;
    int minBrightnessChange;
    FILE *fp = NULL;
    FILE *fp2 = NULL;
    // char uniqueName[FILENAME_MAX];
    // uint32_t macLow = 0; // Low 32-bits of the mac address (for file naming).

    //============================================================================
    // Greetings
    std::cout << "VISSS data acquisition (" << __DATE__ << ")" << std::endl;
    std::cout << "**************************************************************************" << std::endl;

    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_VERBOSE);


    cv :: CommandLineParser parser(argc, argv, params);

    if (parser.has("help") )
    {
        parser.printMessage();
        return 0;
    }

    videoFileIn = parser.get<cv::String>(0);
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: Video file "<< videoFileIn << std::endl;
    videoFileInRaw = videoFileIn.substr(0, videoFileIn.find_last_of("."));
    size_t sep = videoFileInRaw.find_last_of("\\/");
    if (sep != std::string::npos)
        videoFileInRaw = videoFileInRaw.substr(sep + 1, videoFileInRaw.size() - sep - 1);

    cv::String output = parser.get<cv::String>("output");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: Output path "<< output << std::endl;


    encoding = parser.get<cv::String>("encoding");
    // std::cout << "DEBUG | " << get_timestamp() << " | PARSER: FFMPEG encoding "<< encoding << std::endl;
    replace(encoding.begin(), encoding.end(), '@', ' ');
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: FFMPEG encoding "<< encoding << std::endl;

    site = parser.get<cv::String>("site");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: site "<< site << std::endl;

    nStorageThreads = parser.get<int>("threads");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: threads "<< nStorageThreads << std::endl;

    context.live_window_frame_ratio = parser.get<int>("liveratio");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: liveratio "<< context.live_window_frame_ratio << std::endl;

    new_file_interval = parser.get<int>("newfileinterval");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: newfileinterval "<< new_file_interval << std::endl;

    context.fps = parser.get<int>("fps");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: fps "<< context.fps << std::endl;
    // if there is more than one thread, reduce frame rate of the output accordingly
    context.fps = context.fps / nStorageThreads;

    maxframes = parser.get<int>("maxframes");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: maxframes "<< maxframes << std::endl;

    writeallframes1 = parser.get<int>("writeallframes");
    if (writeallframes1 == 0) {
        writeallframes = false;
    } else if (writeallframes1 == 1) {
        writeallframes = true;
    } else {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| writeallframes must be 0 or 1 " << writeallframes1<< std::endl;
        global_error = true;
    }
    followermode1 = parser.get<int>("followermode");
    if (followermode1 == 0) {
        max_n_timeouts1 = max_n_timeouts;
    } else if (followermode1 == 1) {
        max_n_timeouts1 = max_n_timeouts * 10; //tolerate more timeouts as a follower
    } else {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| followermode must be 0 or 1 " << followermode1<< std::endl;
        global_error = true;
    }

    minBrightnessChange = parser.get<int>("minBrightChange");
    if (minBrightnessChange == 20) {
        range[0] = 20;
        range[1] =   30;
        range[2] =   40;
        range[3] =   60;
        range[4] =   80;
        range[5] =   100;
        range[6] =   120;
        range[7] =   256 ; //the upper boundary is exclusive;
    } else if (minBrightnessChange == 30) {
        range[0] = 30;
        range[1] = 40;
        range[2] =   60;
        range[3] =   80;
        range[4] =   100;
        range[5] =   120;
        range[6] =   140;
        range[7] =   256 ; //the upper boundary is exclusive;
    } else {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| minBrightChange must be 20 or 30 " << minBrightnessChange<< std::endl;
        global_error = true;
    }

    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: writeallframes "<< writeallframes << " " << writeallframes1 << std::endl;
    showPreview = !parser.has("nopreview");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: showPreview "<< showPreview << std::endl;
    storeVideo = !parser.has("novideo");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: storeVideo "<< storeVideo << std::endl;
    storeMeta = !parser.has("nometadata");
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: storeMeta "<< storeMeta << std::endl;

    queryGain1 = parser.get<int>("querygain");
    if (queryGain1 == 0) {
        queryGain = false;
    } else if (queryGain1 == 1) {
        queryGain = true;
    } else {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| queryGain must be 0 or 1 " << queryGain1<< std::endl;
        global_error = true;
    }
    std::cout << "DEBUG | " << get_timestamp() << " | PARSER: queryGain "<< queryGain << std::endl;


    gethostname(hostname, HOST_NAME_MAX);

    configFile = "DRYRUN";
    name = "DRYRUN";
    strcpy(DeviceID, "DUMMY");

    DeviceIDMeta = videoFileInRaw;

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

    context.csvFile= videoFileIn.substr(0,videoFileIn.find_last_of('.'))+".txt";


                t_reset = std::chrono::system_clock::now();


                        // Create a thread to receive images from the API and save them
                        context.base_name = output;
                        context.exit = false;
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
                                done = true;
                           }
                        }

                        context.enable_sequence = 0; // End sequence if active.

                        context.exit = true;



}




