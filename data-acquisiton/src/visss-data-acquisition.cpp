//============================================================================

#include "visss-data-acquisition.h"   
//============================================================================
#include "frame_queue.h"   
#include "storage_worker_cv.h"   
#include "processing_worker_cv.h"   


// using namespace cv;
// using namespace std;

#define OPENCV_WINDOW_NAME	"VISSS Live Image"

#define MAX_CAMERAS     2


// Set upper limit on chunk data size in case of problems with device implementation
// (Adjust this if needed).
#define MAX_CHUNK_BYTES 256

// Enable/disable transfer tuning (buffering, timeouts, thread affinity).
#define TUNE_STREAMING_THREADS 1

#define NUM_BUF 8

// Enable/disable buffer FULL/EMPTY handling (cycling)
#define USE_SYNCHRONOUS_BUFFER_CYCLING  1





const char* params
    = "{ help h         |                   | Print usage }"
      "{ output o       | ./                | Output Path }"
      "{ camera n       | 0                 | camera number }"
      "{ quality q      | 23                | quality 0-51 }"
      "{ preset p       | medium            | preset (ultrafast - placebo) }"
      "{ liveratio l    | 100               | every Xth frame will be displayed in the live window }"
      "{ fps f          | 140               | frames per seconds of output }"
      "{ @config        | <none>            | camera configuration file }";

// ====================================




void *m_latestBuffer = NULL;

typedef struct tagMY_CONTEXT
{
    GEV_CAMERA_HANDLE camHandle;
    std::string 					base_name;
    int 					enable_sequence;
    int 					enable_save;
    int                     live_window_frame_ratio;
    double                     fps;
    std::string                  quality;
    std::string                  preset;
    std::chrono::time_point<std::chrono::system_clock> t_reset;
    BOOL              exit;
} MY_CONTEXT, *PMY_CONTEXT;



static void ValidateFeatureValues( const GenApi::CNodePtr &ptrFeature )
{
    
   GenApi::CCategoryPtr ptrCategory(ptrFeature);
   if( ptrCategory.IsValid() )
   {
       GenApi::FeatureList_t Features;
       ptrCategory->GetFeatures(Features);
       for( GenApi::FeatureList_t::iterator itFeature=Features.begin(); itFeature!=Features.end(); itFeature++ )
       {    
          ValidateFeatureValues( (*itFeature) );
       }
   }
   else
   {
        // Issue a "Get" on the feature (with validate set to true).
        GenApi::CValuePtr valNode(ptrFeature);  
        if ((GenApi::RW == valNode->GetAccessMode()) || (GenApi::RO == valNode->GetAccessMode()) )
        {
            int status = 0;
            try {
                valNode->ToString(true);
            }
            // Catch all possible exceptions from a node access.
            CATCH_GENAPI_ERROR(status);
            if (status != 0)
            {
                printf("load_features : Validation failed for feature %s\n", static_cast<const char *>(ptrFeature->GetName())); 
            }
        }
    }
}

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


void *ImageCaptureThread( void *context)
{
    MY_CONTEXT *captureContext = (MY_CONTEXT *)context;
    bool was_active = FALSE;

    if (captureContext != NULL)
    {
        int sequence_init = 0;
        unsigned int sequence_count = 0;

        // FILE *seqFP = NULL;
        //size_t len = 0;
        // char filename[FILENAME_MAX] = {0};

        // if ( captureContext->base_name != NULL)
        // {
        //     len = strlen(captureContext->base_name);
        //     strncpy( filename, captureContext->base_name, len);
        // }
        
        int OpenCV_Type = 0;
        OpenCV_Type = CV_8UC1;
        // int codec = CV_FOURCC('H', '2', '6', '4'); // select desired codec (must be available at runtime)
        int codec = cv:: VideoWriter::fourcc('H', '2', '6', '4'); // select desired codec (must be available at runtime)
        bool isColor = FALSE;

        // The synchronized queues, one per video source/processing worker pair
        std::vector<frame_queue> queue(1);

        // Let's create our processing workers -- let's have two, to simulate your scenario
        // and to keep it interesting, have each one write a different format
        std::vector <processing_worker_cv> processing;
        std::vector<std::thread> processing_thread;

        double total_read_time(0.0);
        int32_t frame_count(0);

        uint last_id = 0;
        uint last_timestamp = 0;
        // While we are still running.
        while(!captureContext->exit)
        {
            GEV_BUFFER_OBJECT *img = NULL;
            GEV_STATUS status = 0;

            // Wait for images to be received
            status = GevWaitForNextImage(captureContext->camHandle, &img, 2000);

            if ((img != NULL) && (status == GEVLIB_OK))
            {
                if (img->status == 0)
                {
                    was_active = TRUE;
                    m_latestBuffer = img->address;
                    if ((last_id>=0) && (img->id != last_id+1) ){
                        std::cout<< "ERROR | " << get_timestamp() << " | missed frames between " << last_id << " and " << img->id << std::endl;
                    }


                    // UINT32 TotalBuffers ;
                    // UINT32 NumUsed ;
                    // UINT32 NumFree;
                    // UINT32 NumTrashed;
                    // GevBufferCyclingMode Mode;
                    UINT32 pTotalBuffers  ;
                    UINT32 pNumUsed  ;
                    UINT32 pNumFree  ;
                    UINT32 pNumTrashed;  
                    GevBufferCyclingMode *pMode ;


                    status = GevQueryTransferStatus (captureContext->camHandle,
                    &pTotalBuffers, &pNumUsed,
                    &pNumFree, &pNumTrashed,
                     pMode);

// if (pNumUsed>0) {
//                     printf("%d ",pTotalBuffers);
//                     printf("%d ",pNumUsed);
//                     printf("%d ",pNumFree);
//                     printf("%d ",pNumTrashed);
//                     printf("%d ",pMode);
//                     printf("%d ",img->id);
//                     printf("%d \n",img->timestamp-last_timestamp);
// }
                    last_id = img->id;
                    last_timestamp = img->timestamp;
                    if ((captureContext->enable_sequence) || (sequence_init == 1))
                    {

                            high_resolution_clock::time_point t1(high_resolution_clock::now());

                        // Export to OpenCV Mat object using SapBuffer data directly
                        cv::Mat exportImg(	img->h, img->w, OpenCV_Type, m_latestBuffer );
                        cv::Size imgSize = exportImg.size();
                          // cout << imgSize << 'SIZE\n';
                        MatMeta exportImgMeta;


                        if (!sequence_init)
                        {
                        // init

                            cv::namedWindow( OPENCV_WINDOW_NAME, cv::WINDOW_AUTOSIZE | cv :: WINDOW_KEEPRATIO );

                            //--- INITIALIZE VIDEOWRITER


                            processing.emplace_back(std::ref(queue[0]), 0
                                , captureContext->base_name
                                , codec
                                , captureContext->fps
                                , imgSize
                                , isColor
                                //, captureContext->quality
                                //, captureContext->preset
                                , captureContext->t_reset
                                );
                            std:: cout << "STATUS | " << get_timestamp() << "| Processing worker started" << std::endl;

                            // And start the worker threads for each processing worker
                            for (auto& s : processing) {
                                processing_thread.emplace_back(&processing_worker_cv::run, &s);
                            }


                            sequence_init = 1;
                            sequence_count = 0;
                            // }
                        } else {
                        // else if (writer.isOpened())
                        // {
                            // cout << filename << ": FILE OPEN\n";


                            // Add timestamp to frame
                            if (1) {
                                std::string textJpg = get_timestamp() + ", ID: " + std::to_string(img->id) + ", time: " + std::to_string(img->timestamp);
                                cv::putText(exportImg, 
                                    textJpg,
                                    cv::Point(10,30), // Coordinates
                                    cv::FONT_HERSHEY_PLAIN, // Font
                                    1.0, // Scale. 2.0 = 2x bigger
                                    cv::Scalar(1), // BGR Color
                                    1, // Line Thickness (Optional)
                                    cv::LINE_AA); // Anti-alias (Optional)
                                }

                            exportImgMeta.MatImage = exportImg.clone();
                            exportImgMeta.timestamp = img->timestamp;
                            exportImgMeta.id = img->id;

                            // Insert a copy into all queues
                            for (auto& q : queue) {
                                q.push(exportImgMeta);
                            }        

                            high_resolution_clock::time_point t2(high_resolution_clock::now());
                             double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
                            total_read_time += dt_us;

                            // std::cout << "Captured image #" << frame_count << " in "
                            //     << (dt_us / 1000.0) << " ms" << std::endl;
                        



                            // writer.write(exportImg);

                            if (frame_count % captureContext->live_window_frame_ratio == 0)
                            {
                                cv::imshow( OPENCV_WINDOW_NAME, exportImg );
                                cv::waitKey(1);
                            }
 
                            fflush(stdout);


                            ++frame_count;

                            sequence_count++;

                            //rest n_timeout after success
                            n_timeouts = 0;

                        // See if we  are done.
                        }
                        if ( !captureContext->enable_sequence )
                        {
                            // GEVBUFFILE_Close( seqFP, sequence_count );
                            std::cout << "STATUS | " << get_timestamp() << " | Complete sequence has " << sequence_count << " frames" << std::endl;
                            sequence_count = 0;
                            sequence_init = 0;

                            // writer.release();

                        }

                    }

                    else
                    {
                        //printf("chunk_data = %p  : chunk_size = %d\n", img->chunk_data, img->chunk_size); //???????????
                        printf("WAITING Frame %llu\r", (unsigned long long)img->id);
                        fflush(stdout);
                    }
                }
                else
                {
                    // Image had an error (incomplete (timeout/overflow/lost)).
                    // Do any handling of this condition necessary.
                    std::cerr << "ERROR | " << get_timestamp() <<" | Frame " << img->id << "Status = " <<  img->status << std::endl;
                }
            }
            else if (status  == GEVLIB_ERROR_TIME_OUT)
            {
                n_timeouts += 1;
                std::cerr << "ERROR | " << get_timestamp() <<" | Camera time out"
                << " #" << n_timeouts << std::endl;
            }
            else {
                n_timeouts += 1;
                std::cerr << "ERROR | " << get_timestamp() <<" | Could not get image " << status
                << " #" << n_timeouts << std::endl;
            }

            if (n_timeouts > max_n_timeouts) {
                std::cerr << "FATAL ERROR | " << get_timestamp() << " | Too many timeouts." <<std::endl;
                global_error = true;
            }


            // See if a sequence in progress needs to be stopped here.
            if (global_error || ((!captureContext->enable_sequence) && (sequence_init == 1)))
            {
                // GEVBUFFILE_Close( seqFP, sequence_count );
                std::cout << "STATUS | " << get_timestamp() << " | Complete sequence has " << sequence_count << "frames" << std::endl;
                sequence_count = 0;
                sequence_init = 0;
            }


            // Synchonrous buffer cycling (saving to disk takes time).
#if USE_SYNCHRONOUS_BUFFER_CYCLING
            if (img != NULL)
            {
                // Release the buffer back to the image transfer process.
                GevReleaseImage( captureContext->camHandle, img);
            }
#endif

        if (global_error) {
            captureContext->enable_sequence = 0;
            captureContext->exit = 1;
            std::cerr << "FATAL ERROR | " << get_timestamp() << " | Error detected. Exiting capture loop..." <<std::endl;

        }

        } //while

        if (was_active) {

                // We're done reading, cancel all the queues
            for (auto& q : queue) {
                q.cancel();
            }

            // And join all the worker threads, waiting for them to finish
            processing_thread[0].join();
            // for (auto& st2 : processing_thread) {
            //     st2.join();
            // }
            // Report the timings
            total_read_time /= 1000.0;
            double total_processing_time(processing[0].total_time_ms());
            double total_write_time_a(processing[0].storage[0].total_time_ms());
            // double total_write_time_b(storage[1].total_time_ms());

            std::cout << "STATUS | " << get_timestamp() 
                << " | Completed processing " << frame_count << " images:\n"
                << "  average capture time = " << (total_read_time / frame_count) << " ms\n"
                << "  average processing time = " << (total_processing_time / frame_count) << " ms\n"
                << "  average write time A = " << (total_write_time_a / frame_count) << " ms\n";
        }

    }
    pthread_exit(0);



}


int IsTurboDriveAvailable(GEV_CAMERA_HANDLE handle)
{
    int type;
    UINT32 val = 0;

    if ( 0 == GevGetFeatureValue( handle, "transferTurboCurrentlyAbailable",  &type, sizeof(UINT32), &val) )
    {
        // Current / Standard method present - this feature indicates if TurboMode is available.
        // (Yes - it is spelled that odd way on purpose).
        return (val != 0);
    }
    else
    {
        // Legacy mode check - standard feature is not there try it manually.
        char pxlfmt_str[64] = {0};

        // Mandatory feature (always present).
        GevGetFeatureValueAsString( handle, "PixelFormat", &type, sizeof(pxlfmt_str), pxlfmt_str);

        // Set the "turbo" capability selector for this format.
        if ( 0 != GevSetFeatureValueAsString( handle, "transferTurboCapabilitySelector", pxlfmt_str) )
        {
            // Either the capability selector is not present or the pixel format is not part of the
            // capability set.
            // Either way - TurboMode is NOT AVAILABLE.....
            return 0;
        }
        else
        {
            // The capabilty set exists so TurboMode is AVAILABLE.
            // It is up to the camera to send TurboMode data if it can - so we let it.
            return 1;
        }
    }
    return 0;
}


int main(int argc, char *argv[])
{
    GEV_DEVICE_INTERFACE  pCamera[MAX_CAMERAS] = {0};
    GEV_STATUS status;
    int numCamera = 0;
    MY_CONTEXT context = {0};
    pthread_t  tid;
    char c;
    int res = 0;
    FILE *fp = NULL;
    FILE *fp2 = NULL;
    // char uniqueName[FILENAME_MAX];
    char filename[FILENAME_MAX] = {0};
    uint32_t macLow = 0; // Low 32-bits of the mac address (for file naming).
    int error_count = 0;
    int feature_count = 0;

    nice(-20);

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

    int camIndex = parser.get<int>("camera");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: Camera index "<< camIndex << std::endl;
    cv::String output = parser.get<cv::String>("output");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: Output path "<< output << std::endl;

    configFile = parser.get<cv::String>(0);
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: Configuration file "<< configFile << std::endl;
    configFileRaw = configFile.substr(0, configFile.find_last_of("."));
    size_t sep = configFileRaw.find_last_of("\\/");
    if (sep != std::string::npos)
        configFileRaw = configFileRaw.substr(sep + 1, configFileRaw.size() - sep - 1);


    context.quality = parser.get<cv::String>("quality");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: FFMPEG Quality "<< context.quality << std::endl;
    context.preset = parser.get<cv::String>("preset");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: FFMPEG preset "<< context.preset << std::endl;

    context.live_window_frame_ratio = parser.get<int>("liveratio");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: liveratio "<< context.live_window_frame_ratio << std::endl;

    context.fps = parser.get<double>("fps");
    std::cout << "STATUS | " << get_timestamp() << " | PARSER: fps "<< context.fps << std::endl;


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


    char OPENCV_PRESET[100];
    strcpy(OPENCV_PRESET,"OPENCV_PRESET=");
    strcat(OPENCV_PRESET,context.preset.c_str());
    char OPENCV_CRF[100];
    strcpy(OPENCV_CRF,"OPENCV_CRF=");
    strcat(OPENCV_CRF,context.quality.c_str());


    if(putenv(OPENCV_PRESET)!=0)
    {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| putenv failed: " << OPENCV_PRESET<< std::endl;
        exit(1);
    }
    if(putenv(OPENCV_CRF)!=0)
    {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| putenv failed: " << OPENCV_CRF<< std::endl;
        global_error = true;
    }

    gethostname(hostname, HOST_NAME_MAX);


    // Open the file.
    fp = fopen(configFile.c_str(), "r");
    if (fp == NULL)
    {
        std::cerr << "FATAL ERROR | " << get_timestamp() << "| Error opening configuration file " << configFile << std::endl;
        global_error= true;
    }   

    if (!parser.check())
    {
        parser.printErrors();
        return 0;
    }

    // Boost application RT response (not too high since GEV library boosts data receive thread to max allowed)
    // SCHED_FIFO can cause many unintentional side effects.
    // SCHED_RR has fewer side effects.
    // SCHED_OTHER (normal default scheduler) is not too bad afer all.
    if (1)
    {
        //int policy = SCHED_FIFO;
        //int policy = SCHED_OTHER;
        int policy = SCHED_BATCH;
        pthread_attr_t attrib;
        int inherit_sched = 0;
        struct sched_param param = {0};

        // Set an average RT priority (increase/decrease to tuner performance).
        param.sched_priority = (sched_get_priority_max(policy) - sched_get_priority_min(policy)) / 2;

        // Set scheduler policy
        pthread_setschedparam( pthread_self(), policy, &param); // Don't care if it fails since we can't do anyting about it.

        // Make sure all subsequent threads use the same policy.
        pthread_attr_init(&attrib);
        pthread_attr_getinheritsched( &attrib, &inherit_sched);
        if (inherit_sched != PTHREAD_INHERIT_SCHED)
        {
            inherit_sched = PTHREAD_INHERIT_SCHED;
            pthread_attr_setinheritsched(&attrib, inherit_sched);
        }
    }


    //===================================================================================
    // Set default options for the library.
    {
        GEVLIB_CONFIG_OPTIONS options = {0};

        GevGetLibraryConfigOptions( &options);
        //options.logLevel = GEV_LOG_LEVEL_OFF;
        //options.logLevel = GEV_LOG_LEVEL_TRACE;
        options.logLevel = GEV_LOG_LEVEL_NORMAL;
        GevSetLibraryConfigOptions( &options);
    }

    //====================================================================================
    // DISCOVER Cameras
    //
    // Get all the IP addresses of attached network cards.

    status = GevGetCameraList( pCamera, MAX_CAMERAS, &numCamera);

    std::cout << "STATUS | " << get_timestamp() << " | " << numCamera << " camera(s) on the network"<< std::endl;

    // Select the first camera found (unless the command line has a parameter = the camera index)
    if (numCamera == 0)
    {
        global_error = true;
    } 
    else   {

        if (camIndex >= (int)numCamera)
        {
            std::cerr << "FATAL ERROR | " << get_timestamp() << "| Camera index " << camIndex<< " out of range" << std::endl;
            std::cerr << "FATAL ERROR | " << get_timestamp() << "| only " << numCamera<< " camera(s) are present" << std::endl;
            camIndex = -1;
            global_error = true;
        }


        if (camIndex != -1)
        {
            //====================================================================
            // Connect to Camera
            //
            // Direct instantiation of GenICam XML-based feature node map.
            int i;
            int type;
            UINT32 val = 0;
            UINT32 height = 0;
            UINT32 width = 0;
            UINT32 format = 0;
            UINT32 maxHeight = 1600;
            UINT32 maxWidth = 2048;
            UINT32 maxDepth = 2;
            UINT64 size;
            UINT64 payload_size;
            int numBuffers = NUM_BUF;
            PUINT8 bufAddress[NUM_BUF];
            GenApi::CNodeMapRef Camera; // The GenICam XML-based feature node map.
            GEV_CAMERA_HANDLE handle = NULL;

            //====================================================================
            // Open the camera.
            status = GevOpenCamera( &pCamera[camIndex], GevExclusiveMode, &handle);
            if (status == 0)
            {
                //===================================================================
                // Get the XML file onto disk and use it to make the CNodeMap object.
                char xmlFileName[MAX_PATH] = {0};
                
                status = Gev_RetrieveXMLFile( handle, xmlFileName, sizeof(xmlFileName), FALSE );
                if ( status == GEVLIB_OK)
                {
                    // printf("XML stored as %s\n", xmlFileName);
                    Camera._LoadXMLFromFile( xmlFileName );
                }

                // Connect the features in the node map to the camera handle.
                status = GevConnectFeatures( handle, static_cast<void *>(&Camera));
                if ( status != 0 )
                {
                    printf("Error %d connecting node map to handle\n", status);
                }

                // Put the camera in "streaming feature mode".
                GenApi ::CCommandPtr start = Camera._GetNode("Std::DeviceRegistersStreamingStart");
                if ( start )
                {
                    try {
                            int done = FALSE;
                            int timeout = 5;
                            start->Execute();
                            while(!done && (timeout-- > 0))
                            {
                                Sleep(10);
                                done = start->IsDone();
                            }
                    }
                    // Catch all possible exceptions from a node access.
                    CATCH_GENAPI_ERROR(status);
                }
                      

                std::cout << "STATUS | " << get_timestamp() << " | Loading settings"<< std::endl;
                std::cout << "**************************************************************************" << std::endl;



                // Read the file as { feature value } pairs and write them to the camera.
                if ( status == 0 )
                {
                    char feature_name[MAX_GEVSTRING_LENGTH+1] = {0};
                    char value_str[MAX_GEVSTRING_LENGTH+1] = {0};
                    
                    while ( 2 == fscanf(fp, "%s %s", feature_name, value_str) )
                    {
                        status = 0;
                        printf("%s %s\n", feature_name, value_str);
                        // Find node and write the feature string (without validation).
                        GenApi::CNodePtr pNode = Camera._GetNode(feature_name);
                        if (pNode)
                        {
                            GenApi ::CValuePtr valNode(pNode);  
                            try {
                                valNode->FromString(value_str, false);
                            }
                            // Catch all possible exceptions from a node access.
                            CATCH_GENAPI_ERROR(status);
                            if (status != 0)
                            {
                                error_count++;
                                printf("Error restoring feature %s : with value %s\n", feature_name, value_str); 
                            }
                            else
                            {
                                feature_count++;
                            }
                        }
                        else
                        {
                            error_count++;
                            printf("Error restoring feature %s : with value %s\n", feature_name, value_str);
                        }
                    }



char feature_name2[] = "timestampControlReset";
char value_str2[] = "1";
// status = 0;
// Find node and write the feature string (without validation).
GenApi::CNodePtr pNode = Camera._GetNode(feature_name2);
if (pNode)
{
    GenApi ::CValuePtr valNode(pNode);  
    try {
        valNode->FromString(value_str2, false);
    }
    // Catch all possible exceptions from a node access.
    CATCH_GENAPI_ERROR(status);
    if (status != 0)
    {
        error_count++;
        printf("Error restoring feature %s : with value %s\n", feature_name, value_str); 
    }
    else
    {
        feature_count++;
    }
}
else
{
    error_count++;
    printf("Error restoring feature %s : with value %s\n", feature_name2, value_str2);
}

std::cout << "**************************************************************************" << std::endl;



context.t_reset = std::chrono::system_clock::now();

std::cout << "STATUS | " << get_timestamp() << "| Camera clock reset around " << context.t_reset.time_since_epoch().count()/1000  << std::endl;

GevGetFeatureValue(handle, "DeviceID", &type, sizeof(DeviceID), &DeviceID);

                }
                // End the "streaming feature mode".
                GenApi ::CCommandPtr end = Camera._GetNode("Std::DeviceRegistersStreamingEnd");
                if ( end  )
                {
                    try {
                            int done = FALSE;
                            int timeout = 5;
                            end->Execute();
                            while(!done && (timeout-- > 0))
                            {
                                Sleep(10);
                                done = end->IsDone();
                            }
                    }
                    // Catch all possible exceptions from a node access.
                    CATCH_GENAPI_ERROR(status);
                }
                
                // Validate.
                if (status == 0)
                {
                    // Iterate through all of the features calling "Get" with validation enabled.
                    // Find the standard "Root" node and dump the features.
                    GenApi::CNodePtr pRoot = Camera._GetNode("Root");
                    ValidateFeatureValues( pRoot );
                }
                

                if (error_count == 0)
                {
                    std::cout << "STATUS | " << get_timestamp() << "| " << feature_count << " Features loaded successfully" << std::endl;
                }
                else
                {
                    std::cerr << "FATAL ERROR | " << get_timestamp() << "| " << feature_count << " Features loaded successfully, " << 
                    error_count << " Features had errors " << std::endl;
                    global_error = true;
                }


// get ALL settings
                time_t t = time(0);   // get time now
                struct tm * now = localtime( & t );
                char timestamp3[80];
                strftime (timestamp3,80,"%Y%m%d-%H%M%S",now);
                std::string full_path = output + "/" + hostname + "_" + configFileRaw + "_" + DeviceID + "/applied_config/" ;
                std::string config_out = full_path + hostname + "_" + DeviceID + "_" + timestamp3 + ".config" ;

                res = mkdir_p(full_path.c_str());
                if (res != 0) {
                    std::cerr << "FATAL ERROR | " << get_timestamp() << " | Cannot create path "<< full_path <<std::endl;
                    global_error = true;
                }
                // Open the file.
                fp2 = fopen(config_out.c_str(), "w");
                if (fp2 == NULL)
                {
                    std::cerr << "FATAL ERROR | " << get_timestamp() << " | Error opening configuration file "<< config_out <<std::endl;
                    global_error = true;
                }   

                // Put the camera in "streaming feature mode".
                GenApi::CCommandPtr start1 = Camera._GetNode("Std::DeviceFeaturePersistenceStart");
                if ( start1 )
                {
                    try {
                            int done = FALSE;
                            int timeout = 5;
                            start1->Execute();
                            while(!done && (timeout-- > 0))
                            {
                                Sleep(10);
                                done = start1->IsDone();
                            }
                    }
                    // Catch all possible exceptions from a node access.
                    CATCH_GENAPI_ERROR(status);
                }
                                
                // Traverse the node map and dump all the { feature value } pairs.
                if ( status == 0 )
                {
                    // Find the standard "Root" node and dump the features.
                    GenApi::CNodePtr pRoot = Camera._GetNode("Root");
                    OutputFeatureValues( pRoot, fp2);
                }

                // End the "streaming feature mode".
                GenApi::CCommandPtr end1 = Camera._GetNode("Std::DeviceFeaturePersistenceEnd");
                if ( end1  )
                {
                    try {
                            int done = FALSE;
                            int timeout = 5;
                            end1->Execute();
                            while(!done && (timeout-- > 0))
                            {
                                Sleep(10);
                                done = end1->IsDone();
                            }
                    }
                    // Catch all possible exceptions from a node access.
                    CATCH_GENAPI_ERROR(status);
                }

                fclose(fp2);
                std::cout << "STATUS | " << get_timestamp() << "| applied configuration written to: " << config_out << std::endl;


                if((not global_error) && (error_count == 0))
                {
                    GEV_CAMERA_OPTIONS camOptions = {0};

                    // Get the low part of the MAC address (use it as part of a unique file name for saving images).
                    // Generate a unique base name to be used for saving image files
                    // based on the last 3 octets of the MAC address.
                    macLow = pCamera[camIndex].macLow;
                    macLow &= 0x00FFFFFF;
                    // snprintf(uniqueName, sizeof(uniqueName), "%s/visss_%06x", output.c_str(), macLow);

                    // // If there are multiple pixel formats supported on this camera, get one.
                    // {
                    //     char feature_name[MAX_GEVSTRING_LENGTH] =  {0};
                    //     GetPixelFormatSelection( handle, sizeof(feature_name), feature_name);
                    //     if ( GevSetFeatureValueAsString(handle, "PixelFormat", feature_name) == 0)
                    //     {
                    //         printf("\n\tUsing selected PixelFormat = %s\n\n", feature_name);
                    //     }
                    // }

                    // Go on to adjust some API related settings (for tuning / diagnostics / etc....).
                    // Adjust the camera interface options if desired (see the manual)
                    GevGetCameraInterfaceOptions( handle, &camOptions);
                    //camOptions.heartbeat_timeout_ms = 60000;		// For debugging (delay camera timeout while in debugger)
                    camOptions.heartbeat_timeout_ms = 5000;		// Disconnect detection (5 seconds)
                    camOptions.enable_passthru_mode = FALSE;
    #if TUNE_STREAMING_THREADS
                    // Some tuning can be done here. (see the manual)
                    camOptions.streamFrame_timeout_ms = 2001;				// Internal timeout for frame reception.
                    camOptions.streamNumFramesBuffered = 4;				// Buffer frames internally.
                    camOptions.streamNumFramesBuffered = 20;             // Buffer frames internally.
                    camOptions.streamMemoryLimitMax = 64 * 1024 * 1024;		// Adjust packet memory buffering limit.
                    camOptions.streamMemoryLimitMax =  2 * 20 * 8 * 1280 * 1024;     // Adjust packet memory buffering limit.
                    camOptions.streamPktSize = 8960;                            // Adjust the GVSP packet size.
                    camOptions.streamPktSize = 8928-1;                            // Adjust the GVSP packet size.
                    camOptions.streamPktDelay = 10;                         // Add usecs between packets to pace arrival at NIC.
                    camOptions.streamPktDelay = 0;                         // Add usecs between packets to pace arrival at NIC.
                    // Assign specific CPUs to threads (affinity) - if required for better performance.
                    {
                        int numCpus = _GetNumCpus();
                        if (numCpus > 1)
                        {
                            camOptions.streamThreadAffinity = numCpus - 1;
                            camOptions.serverThreadAffinity = numCpus - 2;
                        }
                    }
    #endif
                    // Write the adjusted interface options back.
                    GevSetCameraInterfaceOptions( handle, &camOptions);

                    //===========================================================
                    // Set up the frame information.....
                    // printf("Camera ROI set for \n");
                    // GevGetFeatureValue(handle, "Width", &type, sizeof(width), &width);
                    // printf("\tWidth = %d\n", width);
                    // GevGetFeatureValue(handle, "Height", &type, sizeof(height), &height);
                    // printf("\tHeight = %d\n", height);
                    // GevGetFeatureValue(handle, "PixelFormat", &type, sizeof(format), &format);
                    // printf("\tPixelFormat  = 0x%x\n", format);

                    if (camOptions.enable_passthru_mode)
                    {
                        // printf("\n\tPASSTHRU Mode is ON\n");
                    }

                    if (IsTurboDriveAvailable(handle))
                    {
                        val = 1;
                        if ( GevGetFeatureValue(handle, "transferTurboMode", &type, sizeof(UINT32), &val) == 0)
                        {
                            if (val == 1)
                            {
                                std::cout << "STATUS | " << get_timestamp() << "| Turbodrive on" << std::endl;
                            }
                            else
                            {
                                std::cerr << "FATAL ERROR | " << get_timestamp() << "| Turbodrive off" << std::endl;
                                global_error = true;
                            }
                        }
                    }
                    else
                    {
                        std::cerr << "FATAL ERROR | " << get_timestamp() << "| TurboDrive is NOT Available" << std::endl;

                        global_error = true;  
                    }

                    //
                    // End frame info
                    //============================================================

                    if ((not global_error) && (status == 0))
                    {
                        //=================================================================
                        // Set up a grab/transfer from this camera based on the settings...
                        //
                        GevGetPayloadParameters( handle,  &payload_size, (UINT32 *)&type);
                        maxHeight = height;
                        maxWidth = width;
                        maxDepth = GetPixelSizeInBytes(format);

                        // Calculate the size of the image buffers.
                        // (Adjust the number of lines in the buffer to fit the maximum expected
                        //	 chunk size - just in case it gets enabled !!!)
                        {
                            int extra_lines = (MAX_CHUNK_BYTES + width - 1) / width;
                            size = GetPixelSizeInBytes(format) * width * (height + extra_lines);
                        }

                        // Allocate image buffers
                        // (Either the image size or the payload_size, whichever is larger - allows for packed pixel formats and metadata).
                        size = (payload_size > size) ? payload_size : size;

                        for (i = 0; i < numBuffers; i++)
                        {
                            bufAddress[i] = (PUINT8)malloc(size);
                            memset(bufAddress[i], 0, size);
                        }


                        // Generate a file name from the unique base name
                        // (leave at least 16 digits for index and extension)
                        // _GetUniqueFilename_sec(filename, (sizeof(filename) - 17), uniqueName);

                        // Initialize a transfer with synchronous buffer handling.
                        // (To avoid overwriting data buffer while saving to disk).
    #if USE_SYNCHRONOUS_BUFFER_CYCLING
                        // Initialize a transfer with synchronous buffer handling.
                        status = GevInitializeTransfer( handle, SynchronousNextEmpty, size, numBuffers, bufAddress);
    #else
                        // Initialize a transfer with asynchronous buffer handling.
                        status = GevInitializeTransfer( handle, Asynchronous, size, numBuffers, bufAddress);
    #endif
                        // Create a thread to receive images from the API and save them
                        context.camHandle = handle;
                        context.base_name = output;
                        context.exit = FALSE;
                        pthread_create(&tid, NULL, ImageCaptureThread, &context);

                        // Call the main command loop or the example.
                        PrintMenu();

                        for (i = 0; i < numBuffers; i++)
                        {
                            memset(bufAddress[i], 0, size);
                        }
                        std::cout << "STATUS | " << get_timestamp() <<" | STARTING GevStartTransfer" <<std::endl;

                        status = GevStartTransfer( handle, -1);
                        if (status != 0) {
                            std::cerr << "FATAL STATUS | " << get_timestamp() <<" | Error starting grab" <<std::endl;
                            printf("0x%x  or %d\n", status, status);
                            global_error = true;
                        }
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
                        GevStopTransfer(handle);
                        context.exit = TRUE;
                        pthread_join( tid, NULL);

                        std::cout << "STATUS | " << get_timestamp() <<" | STOPPING GevStartTransfer" <<std::endl;
                        GevAbortTransfer(handle);
                        status = GevFreeTransfer(handle);
                        for (i = 0; i < numBuffers; i++)
                        {
                            free(bufAddress[i]);
                        }

                    }
                }
                GevCloseCamera(&handle);
            }
            else
            {
                std::cerr << "FATAL ERROR | " << get_timestamp() << " | Error : "<< status <<" : opening camera" << std::endl;
                global_error = true;
            }
        }
    }

    // Close down the API.
    GevApiUninitialize();

    // Close socket API
    _CloseSocketAPI ();	// must close API even on error


    //printf("Hit any key to exit\n");
    //kbhit();

    if (global_error) {
        std::cerr << "FATAL ERROR | " << get_timestamp() << " | EXIT due to fatal error" <<std::endl;
        return 1;
    } else {
        std::cout << "STATUS | " << get_timestamp() << " | All done" <<std::endl;
        return 0;
    }
}

