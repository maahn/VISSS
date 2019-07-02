//============================================================================

#include "visss-data-acquisition.h"   
//============================================================================

#include "opencv_writer.h"   


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

//every Xth frame will be displayed in the live window
#define LIVE_WINDOW_FRAME_RATIO 1




const char* params
    = "{ help h         |                   | Print usage }"
      "{ output o       | ./                | Output Path }"
      "{ camera n       | 0                 | camera number }"
      "{ @config        | <none>            | camera configuration file }";

// ====================================



void *m_latestBuffer = NULL;

typedef struct tagMY_CONTEXT
{
    GEV_CAMERA_HANDLE camHandle;
    char 					*base_name;
    int 					enable_sequence;
    int 					enable_save;
    BOOL              exit;
} MY_CONTEXT, *PMY_CONTEXT;

// Unique name (seconds resolution in filename)
static void _GetUniqueFilename_sec( char *filename, size_t size, char *basename)
{
    // Create a filename based on the current time (to 1 seconds)
    struct timeval tm;
    uint32_t years, days, hours, seconds;

    if ((filename != NULL) && (basename != NULL) )
    {
        if (size > (16 + sizeof(basename)) )
        {

            // Get the time and turn it into a 10 msec resolution counter to use as an index.
            gettimeofday( &tm, NULL);
            years = ((tm.tv_sec / 86400) / 365);
            tm.tv_sec = tm.tv_sec - (years * 86400 * 365);
            days  = (tm.tv_sec / 86400);
            tm.tv_sec = tm.tv_sec - (days * 86400);
            hours = (tm.tv_sec / 3600);
            seconds = tm.tv_sec - (hours * 3600);

            snprintf(filename, size, "%s_%02d%03d%02d%04d", basename, (years - 30), days, hours, (int)seconds);
        }
    }
}

int mkdir_p(const char *path)
{
    /* Adapted from http://stackoverflow.com/a/2336245/119527 */
    const size_t len = strlen(path);
    char _path[PATH_MAX];
    char *p; 

    errno = 0;

    /* Copy string so its mutable */
    if (len > sizeof(_path)-1) {
        errno = ENAMETOOLONG;
        return -1; 
    }   
    strcpy(_path, path);

    /* Iterate the string */
    for (p = _path + 1; *p; p++) {
        if (*p == '/') {
            /* Temporarily truncate */
            *p = '\0';

            if (mkdir(_path, S_IRWXU) != 0) {
                if (errno != EEXIST)
                    return -1; 
            }

            *p = '/';
        }
    }   
}

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
    printf("press [Q] or [ESC] to end\n");
}


void *ImageCaptureThread( void *context)
{
    MY_CONTEXT *captureContext = (MY_CONTEXT *)context;

    if (captureContext != NULL)
    {
        int sequence_init = 0;
        unsigned int sequence_count = 0;
        int sequence_index = 0;

        // FILE *seqFP = NULL;
        size_t len = 0;
        char filename[FILENAME_MAX] = {0};

        if ( captureContext->base_name != NULL)
        {
            len = strlen(captureContext->base_name);
            strncpy( filename, captureContext->base_name, len);
        }
        

        cv::VideoWriter writer;
        int OpenCV_Type = 0;
        OpenCV_Type = CV_8UC1;
        // int codec = CV_FOURCC('H', '2', '6', '4'); // select desired codec (must be available at runtime)
        int codec = cv:: VideoWriter::fourcc('H', '2', '6', '4'); // select desired codec (must be available at runtime)
        double fps = 13;                          // framerate of the created video stream
        bool isColor = FALSE;

        // The synchronized queues, one per video source/storage worker pair
        std::vector<frame_queue> queue(1);

        // Let's create our storage workers -- let's have two, to simulate your scenario
        // and to keep it interesting, have each one write a different format
        std::vector <storage_worker> storage;
        std::vector<std::thread> storage_thread;

        int32_t const MAX_FRAME_COUNT(10);
        double total_read_time(0.0);
        int32_t frame_count(0);

        int last_id = -1;
        int last_timestamp = -1;
        // While we are still running.
        while(!captureContext->exit)
        {
            GEV_BUFFER_OBJECT *img = NULL;
            GEV_STATUS status = 0;

            // Wait for images to be received
            status = GevWaitForNextImage(captureContext->camHandle, &img, 1000);

            if ((img != NULL) && (status == GEVLIB_OK))
            {
                if (img->status == 0)
                {
                    m_latestBuffer = img->address;
                    if ((last_id>=0) && (img->id != last_id+1) ){
                        printf("MISSED %06d %06d\n",img->id, last_id);
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

printf("%llu \n",(unsigned long long)img->timestamp);

if (pNumUsed>0) {
                    printf("%d ",pTotalBuffers);
                    printf("%d ",pNumUsed);
                    printf("%d ",pNumFree);
                    printf("%d ",pNumTrashed);
                    printf("%d ",pMode);
                    printf("%d ",img->id);
                    printf("%d \n",img->timestamp-last_timestamp);
}
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



                    	printf("IN LOOP STARTING...\n");

                            cv::namedWindow( OPENCV_WINDOW_NAME, cv::WINDOW_AUTOSIZE | cv :: WINDOW_KEEPRATIO );

                            //--- INITIALIZE VIDEOWRITER




                            snprintf( &filename[len], (FILENAME_MAX - len - 1), "_seq_%06d.avi", sequence_index++);
                            // writer.open(filename, codec, fps, imgSize, isColor);

                            // writer.set(cv::VIDEOWRITER_PROP_QUALITY, 75);
                            // double qual = writer.get(cv::VIDEOWRITER_PROP_QUALITY);
                            // cout << "QUAL" << qual << "\n";

                            storage.emplace_back(std::ref(queue[0]), 0
                                , filename
                                , codec
                                , fps
                                , imgSize
                                , isColor);

                            // And start the worker threads for each storage worker
                            for (auto& s : storage) {
                                storage_thread.emplace_back(&storage_worker::run, &s);
                            }


                            // check if we succeeded


            // snprintf(filename, size, "%s_%02d%03d%02d%04d", basename, (years - 30), days, hours, (int)seconds);
            //         // 	snprintf( &filename[len], (FILENAME_MAX-len-1), "_%09llu.gevbuf", (unsigned long long)img->id);





                            // 	// Init the capture sequence
                            // 	printf("%s\n", filename); //?????
                            // 	seqFP = GEVBUFFILE_Create( filename );
                            // 	if (seqFP != NULL)
                            // 	{
                            // 		printf("Store sequence to : %s\n", filename);
                            sequence_init = 1;
                            sequence_count = 0;
                            // }
                        } else {
                        // else if (writer.isOpened())
                        // {
                            // cout << filename << ": FILE OPEN\n";


                            // Now the main capture loop

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

                            if (frame_count % LIVE_WINDOW_FRAME_RATIO == 0)
                            {
                                cv::imshow( OPENCV_WINDOW_NAME, exportImg );
                                cv::waitKey(1);
                            }
                            // Display OpenCV Image
                            // // img->ReleaseAddress( &pBuf );

                            // if ( GEVBUFFILE_AddFrame( seqFP, img ) > 0)
                            // {
                            // printf("OPENCV Add to Sequence : Frame %llu\n", (unsigned long long)img->id);
                            fflush(stdout);

                            if (frame_count>360000) {
                                captureContext->enable_sequence = 0;
                                captureContext->exit = 1;
                                printf("STOPPING!\n");
                            }
                            ++frame_count;

                            sequence_count++;

                        // } else {
                        //     cerr << filename << ": FILE NOT OPEN\n";
                        //     printf("Add to Sequence : Data Not Saved for Frame %llu\n", (unsigned long long)img->id); 

                        //     fflush(stdout);

                        // }
                        // 	if (sequence_count > FRAME_SEQUENCE_MAX_COUNT)
                        // 	{
                        // 		printf("\n Max Sequence Frame Count exceeded - closing sequence file\n");
                        // 		captureContext->enable_sequence = 0;
                        // 	}
                        // }
                        // else
                        // {
                        // 	printf("Add to Sequence : Data Not Saved for Frame %llu\r", (unsigned long long)img->id); 
                        // }
                        // }
                        // See if we  are done.
                        }
                        if ( !captureContext->enable_sequence )
                        {
                            // GEVBUFFILE_Close( seqFP, sequence_count );
                            printf("Complete sequence 1: %s has %d frames\n", filename, sequence_count);
                            sequence_count = 0;
                            sequence_init = 0;

                            // writer.release();
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
                            double total_write_time_a(storage[0].total_time_ms());
                            // double total_write_time_b(storage[1].total_time_ms());

                            std::cout << "Completed processing " << frame_count << " images:\n"
                                << "  average capture time = " << (total_read_time / frame_count) << " ms\n"
                                << "  average write time A = " << (total_write_time_a / frame_count) << " ms\n";
                                // << "  average write time B = " << (total_write_time_b / frame_count) << " ms\n";





                            printf("RELEASED\n");
                        }

                    }
                    // else if (captureContext->enable_save)
                    // {
                    // 	// Save image (example only).
                    // 	// Note : For better performace, some other scheme with multiple
                    // 	//        images (sequence) in a file using file mapping is needed.

                    // 	snprintf( &filename[len], (FILENAME_MAX-len-1), "_%09llu.gevbuf", (unsigned long long)img->id);
                    // 	GEVBUFFILE_SaveSingleBufferObject( filename, img );

                    // 	// Turn off file save for next frame
                    // 	captureContext->enable_save = 0;
                    // 	printf("Single frame saved as : %s\n", filename);
                    // 	printf("Frame %llu\r", (unsigned long long)img->id); fflush(stdout);
                    // }
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
                    printf("Frame %llu : Status = %d\n", (unsigned long long)img->id, img->status);
                }
            }
            // See if a sequence in progress needs to be stopped here.
            if ((!captureContext->enable_sequence) && (sequence_init == 1))
            {
                // GEVBUFFILE_Close( seqFP, sequence_count );
                printf("Complete sequence2 : %s has %d frames\n", filename, sequence_count);
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
    int done = FALSE;
    FILE *fp = NULL;
    int turboDriveAvailable = 0;
    char uniqueName[FILENAME_MAX];
    char filename[FILENAME_MAX] = {0};
    uint32_t macLow = 0; // Low 32-bits of the mac address (for file naming).
    int error_count = 0;
    int feature_count = 0;

    //============================================================================
    // Greetings
    printf ("\nVISSS data acquisition (%s)\n", __DATE__);

    cv :: CommandLineParser parser(argc, argv, params);

    if (parser.has("help"))
    {
        parser.printMessage();
        return 0;
    }

    int camIndex = parser.get<int>("camera");
    printf("Camera index %d \n", camIndex);
    cv::String output = parser.get<cv::String>("output");
    printf("Output path %s \n", output.c_str());

    if (mkdir(output.c_str(), S_IRWXU) != 0) {
        if (errno != EEXIST)
            return -1; 
    } 

    cv::String configFile = parser.get<cv::String>(0);
    printf("Configuration file %s \n", configFile.c_str());

    // Open the file.
    fp = fopen(configFile.c_str(), "r");
    if (fp == NULL)
    {
        printf("Error opening configuration file %s : errno = %d\n", filename, errno);
        exit(-1);
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
        // int policy = SCHED_FIFO;
        int policy = SCHED_OTHER;
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

    printf ("%d camera(s) on the network\n", numCamera);

    // Select the first camera found (unless the command line has a parameter = the camera index)
    if (numCamera != 0)
    {

        if (camIndex >= (int)numCamera)
        {
            printf("Camera index %d out of range\n", camIndex);
            printf("only %d camera(s) are present\n", numCamera);
            camIndex = -1;
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
            UINT32 isLocked = 0;
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

t_reset_1 = high_resolution_clock::now();


char feature_name2[] = "timestampControlReset";
char value_str2[] = "1";
status = 0;
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


t_reset_2 = high_resolution_clock::now();
double dt_us(static_cast<double>(duration_cast<microseconds>(t_reset_2 - t_reset_1).count()));


std::cout << "time reset in " << (dt_us / 1000.0) << " ms" << t_reset_2.time_since_epoch().count() << std::endl;

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
                    printf("%d Features loaded successfully !\n", feature_count);
                }
                else
                {
                    printf("%d Features loaded successfully : %d Features had errors\n", feature_count, error_count);
                    printf("Exiting...\n");
                }


                if (error_count == 0)
                {
                    GEV_CAMERA_OPTIONS camOptions = {0};

                    // Get the low part of the MAC address (use it as part of a unique file name for saving images).
                    // Generate a unique base name to be used for saving image files
                    // based on the last 3 octets of the MAC address.
                    macLow = pCamera[camIndex].macLow;
                    macLow &= 0x00FFFFFF;
                    snprintf(uniqueName, sizeof(uniqueName), "%s/img_%06x", output.c_str(), macLow);

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
                    printf("Camera ROI set for \n");
                    GevGetFeatureValue(handle, "Width", &type, sizeof(width), &width);
                    printf("\tWidth = %d\n", width);
                    GevGetFeatureValue(handle, "Height", &type, sizeof(height), &height);
                    printf("\tHeight = %d\n", height);
                    GevGetFeatureValue(handle, "PixelFormat", &type, sizeof(format), &format);
                    printf("\tPixelFormat  = 0x%x\n", format);

                    if (camOptions.enable_passthru_mode)
                    {
                        printf("\n\tPASSTHRU Mode is ON\n");
                    }

                    if (IsTurboDriveAvailable(handle))
                    {
                        printf("\n\tTurboDrive is : \n");
                        val = 1;
                        if ( GevGetFeatureValue(handle, "transferTurboMode", &type, sizeof(UINT32), &val) == 0)
                        {
                            if (val == 1)
                            {
                                printf("ON\n");
                            }
                            else
                            {
                                printf("OFF\n");
                            }
                        }
                    }
                    else
                    {
                        printf("\t*** TurboDrive is NOT Available ***\n");
                    }

                    printf("\n");
                    //
                    // End frame info
                    //============================================================

                    if (status == 0)
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
                        _GetUniqueFilename_sec(filename, (sizeof(filename) - 17), uniqueName);

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
                        context.base_name = filename;
                        context.exit = FALSE;
                        pthread_create(&tid, NULL, ImageCaptureThread, &context);

                        // Call the main command loop or the example.
                        PrintMenu();

                        for (i = 0; i < numBuffers; i++)
                        {
                            memset(bufAddress[i], 0, size);
                        }
                        printf("STARING GevStartTransfer\n");
                        status = GevStartTransfer( handle, -1);
                        if (status != 0) printf("Error starting grab - 0x%x  or %d\n", status, status);
                        printf("STARTED GevStartTransfer\n");

                        context.enable_sequence = 1;



                        while(!done)
                        {
                            c = GetKey();


                            if ((c == 0x1b) || (c == 'q') || (c == 'Q'))
                            {
                                context.enable_sequence = 0; // End sequence if active.
                                GevStopTransfer(handle);
                                done = TRUE;
                                context.exit = TRUE;
                                pthread_join( tid, NULL);
                            }
                        }

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
                printf("Error : 0x%0x : opening camera\n", status);
            }
        }
    }

    // Close down the API.
    GevApiUninitialize();

    // Close socket API
    _CloseSocketAPI ();	// must close API even on error


    //printf("Hit any key to exit\n");
    //kbhit();

    return 0;
}

