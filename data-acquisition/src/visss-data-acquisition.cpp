//============================================================================

/**
 * @file visss-data-acquisition.cpp
 * @brief Main data acquisition program for VISSS system
 * 
 * This file contains the main implementation for the VISSS data acquisition
 * system, which interfaces with GigE Vision cameras to capture and store
 * video data.
 */

#include "visss-data-acquisition.h"
#include "GenApi/GenApi.h" //!< GenApi lib definitions.
#include "cordef.h"
#include "gevapi.h" //!< GEV lib definitions.
// #include "gevbuffile.h"
//============================================================================

#include "frame_queue.h"
#include "storage_worker_cv.h"
// #include "storage_worker_cv.h"

// using namespace cv;
// using namespace std;

/**
 * @brief OpenCV window name for live preview
 */
#define OPENCV_WINDOW_NAME "VISSS Live Image"

/**
 * @brief Maximum number of cameras supported
 */
#define MAX_CAMERAS 2

// Set upper limit on chunk data size in case of problems with device
// implementation (Adjust this if needed).
#define MAX_CHUNK_BYTES 256

// Enable/disable transfer tuning (buffering, timeouts, thread affinity).
#define TUNE_STREAMING_THREADS 1

#define NUM_BUF 8

// Enable/disable buffer FULL/EMPTY handling (cycling)
#define USE_SYNCHRONOUS_BUFFER_CYCLING 1

// save mean and std images
#define SAVE_MEAN_STD_IMAGE 0

/**
 * @brief Command line parameters for the main application
 * 
 * Supported parameters:
 * - help (-h): Print usage
 * - output (-o): Output Path
 * - site (-s): Site string
 * - encoding (-e): ffmpeg encoding options with '@' replacing ' '
 * - liveratio (-l): Every Xth frame will be displayed in the live window
 * - fps (-f): Frames per seconds of output
 * - newfileinterval (-i): Write new file every ?s. Set to 0 to deactivate
 * - maxframes (-m): Stop after this many frames (for debugging)
 * - writeallframes (-w): Write all frames whether sth is moving or not (for debugging)
 * - rotateimage (-r): Rotate image counterclockwise [0,1]
 * - followermode (-d): Do not complain about camera timeouts
 * - nopreview: No preview window
 * - noptp (-p): No not use ptp for clock synchronization [0,1]
 * - minBrightChange (-b): Minimum brightness change to start recording [20,30]
 * - querygain (-q): Query gain and brightness [0,1]
 * - novideo: Do not store video data
 * - nometadata: Do not store meta data
 * - resetDHCP: Reset camera DHCP and exit. Config file must be present but does not matter
 * - name (-n): Camera name
 * - threads (-t): Number of storage threads
 * - config: Camera configuration file
 * - camera: Camera IP
 */
const char *params =
    "{ help h            |                   | Print usage }"
    "{ output o          | ./                | Output Path }"
    "{ site s            | none              | site string }"
    "{ encoding e        | -c:v@libx264      | ffmpeg encoding options with "
    "'@' replacing ' '}"
    "{ liveratio l       | 70                | every Xth frame will be "
    "displayed in the live window }"
    "{ fps f             | 140               | frames per seconds of output }"
    "{ newfileinterval i | 300               | write new file very ?s. Set to "
    "0 to deactivate}"
    "{ maxframes m       | -1                | stop after this many frames "
    "(for debugging) }"
    "{ writeallframes w  | 0                 | write all frames whether sth is "
    "moving or not (for debugging) }"
    "{ rotateimage r     | 0                 | rotate image counterclockwise "
    "[0,1] }"
    "{ followermode d    | 0                 | do not complain about camera "
    "timeouts }"
    "{ nopreview         |                   | no preview window }"
    "{ noptp p           | 0                 | no not use ptp for clock "
    "synchronization [0,1] }"
    "{ minBrightChange b | 20                | minimum brightnes change to "
    "start recording [20,30] }"
    "{ querygain q       | 0                 | query gain and bightness [0,1]}"
    "{ novideo           |                   | do not store video data }"
    "{ nometadata        |                   | do not store meta data }"
    "{ resetDHCP         |                   | reset camera DHCP and exit. "
    "Config file must be present but does not matter }"
    "{ name n            | VISSS             | camera name }"
    "{ threads t         | 1                 | number of storage threads }"
    "{ @config           | <none>            | camera configuration file }"
    "{ @camera           | <none>            | camera IP }";

// ====================================

/**
 * @brief Global pointer to latest buffer
 */
void *m_latestBuffer = NULL;

/**
 * @brief Context structure for image capture thread
 */
typedef struct tagMY_CONTEXT {
  GEV_CAMERA_HANDLE camHandle;
  std::string base_name;
  int enable_sequence;
  int enable_save;
  int live_window_frame_ratio;
  int fps;
  bool exit;
} MY_CONTEXT, *PMY_CONTEXT;

/**
 * @brief Output feature value pair to file
 * @param feature_name Name of the feature
 * @param value_string String representation of the value
 * @param fp File pointer to write to
 */
static void OutputFeatureValuePair(const char *feature_name,
                                   const char *value_string, FILE *fp) {
  if ((feature_name != NULL) && (value_string != NULL)) {
    // Feature : Value pair output (in one place in to ease changing formats or
    // output method - if desired).
    fprintf(fp, "%s %s\n", feature_name, value_string);
  }
}

/**
 * @brief Output feature values to file
 * @param ptrFeature Node pointer to feature
 * @param fp File pointer to write to
 */
static void OutputFeatureValues(const GenApi::CNodePtr &ptrFeature, FILE *fp) {

  GenApi::CCategoryPtr ptrCategory(ptrFeature);
  if (ptrCategory.IsValid()) {
    GenApi::FeatureList_t Features;
    ptrCategory->GetFeatures(Features);
    for (GenApi::FeatureList_t::iterator itFeature = Features.begin();
         itFeature != Features.end(); itFeature++) {
      OutputFeatureValues((*itFeature), fp);
    }
  } else {
    // Store only "streamable" features (since only they can be restored).
    if (ptrFeature->IsStreamable()) {
      // Create a selector set (in case this feature is selected)
      bool selectorSettingWasOutput = false;
      GenApi::CSelectorSet selectorSet(ptrFeature);

      // Loop through all the selectors that select this feature.
      // Use the magical CSelectorSet class that handles the
      //   "set of selectors that select this feature" and indexes
      // through all possible combinations so we can save all of them.
      selectorSet.SetFirst();
      do {
        GenApi::CValuePtr valNode(ptrFeature);
        if (valNode.IsValid() && (GenApi::RW == valNode->GetAccessMode()) &&
            (ptrFeature->IsFeature())) {
          // Its a valid streamable feature.
          // Get its selectors (if it has any)
          GenApi::FeatureList_t selectorList;
          selectorSet.GetSelectorList(selectorList, true);

          for (GenApi::FeatureList_t ::iterator itSelector =
                   selectorList.begin();
               itSelector != selectorList.end(); itSelector++) {
            // Output selector : selectorValue as a feature : value pair.
            selectorSettingWasOutput = true;
            GenApi::CNodePtr selectedNode(*itSelector);
            GenApi::CValuePtr selectedValue(*itSelector);
            OutputFeatureValuePair(
                static_cast<const char *>(selectedNode->GetName()),
                static_cast<const char *>(selectedValue->ToString()), fp);
          }

          // Output feature : value pair for this selector combination
          // It just outputs the feature : value pair if there are no selectors.
          OutputFeatureValuePair(
              static_cast<const char *>(ptrFeature->GetName()),
              static_cast<const char *>(valNode->ToString()), fp);
        }

      } while (selectorSet.SetNext());
      // Reset to original selector/selected value (if any was used)
      selectorSet.Restore();

      // Save the original settings for any selector that was handled (looped
      // over) above.
      if (selectorSettingWasOutput) {
        GenApi::FeatureList_t selectingFeatures;
        selectorSet.GetSelectorList(selectingFeatures, true);
        for (GenApi::FeatureList_t ::iterator itSelector =
                 selectingFeatures.begin();
             itSelector != selectingFeatures.end(); ++itSelector) {
          GenApi::CNodePtr selectedNode(*itSelector);
          GenApi::CValuePtr selectedValue(*itSelector);
          OutputFeatureValuePair(
              static_cast<const char *>(selectedNode->GetName()),
              static_cast<const char *>(selectedValue->ToString()), fp);
        }
      }
    }
  }
}

/**
 * @brief Validate feature values
 * @param ptrFeature Node pointer to feature
 */
static void ValidateFeatureValues(const GenApi::CNodePtr &ptrFeature) {

  GenApi::CCategoryPtr ptrCategory(ptrFeature);
  if (ptrCategory.IsValid()) {
    GenApi::FeatureList_t Features;
    ptrCategory->GetFeatures(Features);
    for (GenApi::FeatureList_t::iterator itFeature = Features.begin();
         itFeature != Features.end(); itFeature++) {
      ValidateFeatureValues((*itFeature));
    }
  } else {
    // Issue a "Get" on the feature (with validate set to true).
    GenApi::CValuePtr valNode(ptrFeature);
    if ((GenApi::RW == valNode->GetAccessMode()) ||
        (GenApi::RO == valNode->GetAccessMode())) {
      int status = 0;
      try {
        valNode->ToString(true);
      }
      // Catch all possible exceptions from a node access.
      CATCH_GENAPI_ERROR(status);
      if (status != 0) {
        printf("load_features : Validation failed for feature %s\n",
               static_cast<const char *>(ptrFeature->GetName()));
      }
    }
  }
}

/**
 * @brief Get a character from standard input
 * @return Character input from user
 */
char GetKey() {
  char key = getchar();
  while ((key == '\r') || (key == '\n')) {
    key = getchar();
  }
  return key;
}

/**
 * @brief Print menu instructions to user
 */
void PrintMenu() {
  std::cout << "***************************************************************"
               "***********"
            << std::endl;
  std::cout << "*** press [Q] or [ESC] to end" << std::endl;
  std::cout << "***************************************************************"
               "***********"
            << std::endl;
}

/**
 * @brief Image capture thread function
 * @param context Pointer to capture context
 * @return NULL pointer
 */
void *ImageCaptureThread(void *context) {
  MY_CONTEXT *captureContext = (MY_CONTEXT *)context;
  bool was_active = false;
  bool do_housekeeping = true;
  bool reset_clock_detected = false;
  bool first_image = true;
  bool after_first_reset = false;
  bool waiting_for_clock_reset = false;
  unsigned long timestamp_s = 0;
  unsigned long timestamp_us = 0;

  long int timeNow = 0;
  long int timeStart = 0;
  long int framesInFile = 0;
  uint last_id = 0;
  signed long last_cameratimestamp = 0; // signed to allow difference
  uint id_offset = 0;

  int type;
  UINT32 val = 0;
  float valF = 0;
  UINT32 valI = 0;

  GEV_STATUS status = 0;
  GEV_STATUS statusF = 0;

  if (captureContext != NULL) {
    int sequence_init = 0;
    unsigned int sequence_count = 0;

    // FILE *seqFP = NULL;
    // size_t len = 0;
    // char filename[FILENAME_MAX] = {0};

    // if ( captureContext->base_name != NULL)
    // {
    //     len = strlen(captureContext->base_name);
    //     strncpy( filename, captureContext->base_name, len);
    // }

    int OpenCV_Type = 0;
    OpenCV_Type = CV_8UC1;
    // int codec = cv:: VideoWriter::fourcc('H', '2', '6', '4'); // select
    // desired codec (must be available at runtime)
    int codec = cv::VideoWriter::fourcc(
        'a', 'v', 'c',
        '1'); // select desired codec (must be available at runtime)
    bool isColor = false;
    int tt = 0;
    long skipCounter = 0;

    // The synchronized queues, one per video source/storage worker pair
    std::vector<frame_queue> queue(nStorageThreads);

    // Let's create our storage workers -- let's have two, to simulate your
    // scenario and to keep it interesting, have each one write a different
    // format
    std::vector<storage_worker_cv> storage;
    std::vector<std::thread> storage_thread;

    double total_read_time(0.0);
    int32_t frame_count(0);

    std::cout << "DEBUG | " << get_timestamp() << "| " << "Capture Loop ready"
              << std::endl;
    timeStart = static_cast<long int>(time(NULL));

    // While we are still running.
    while (!captureContext->exit) {

      skipCounter++;

      GEV_BUFFER_OBJECT *img = NULL;
      GEV_STATUS status = 0;
      // Wait for images to be received
      status = GevWaitForNextImage(captureContext->camHandle, &img, 1000);
      // get time for recordtime timestamp
      high_resolution_clock::time_point tr(high_resolution_clock::now());

      if ((img != NULL) && (status == GEVLIB_OK)) {
        // skip first second
        if (skipCounter > captureContext->fps) {
          if (img->status == 0) {
            was_active = true;

            // handle clock reset
            timeNow = static_cast<long int>(time(NULL));
            if (!noptp) {
              timestamp_s = (img->timestamp) / 1e9;
              timestamp_us = (img->timestamp) / 1e3;
            } else {
              timestamp_s = (img->timestamp + t_reset_uint_applied) / 1e6;
              timestamp_us = (img->timestamp + t_reset_uint_applied);
            }
            do_housekeeping = ((new_file_interval > 0) &&
                               (timestamp_s % new_file_interval == 0) &&
                               ((timeNow - timeStart) > 10));

            if (do_housekeeping || first_image) {
              if (!noptp) {
                framesInFile = 0; // required to trigger new file generation
              } else {
                t_reset = std::chrono::system_clock::now();
                statusF = GevSetFeatureValueAsString(
                    captureContext->camHandle, "timestampControlReset", "1");
                if (statusF == GEVLIB_OK) {
                  std::cout
                      << std::endl
                      << "INFO | " << get_timestamp() << " | Reset clock to "
                      << t_reset.time_since_epoch().count() / 1000 << ". ID "
                      << img->id << std::endl;
                } else {
                  // if the clock rest does not work it typically indicates a
                  // larger problem, so better exit (and restart)
                  std::cout << std::endl
                            << "FATAL ERROR | " << get_timestamp()
                            << " | Unable to reset clock, error " << statusF
                            << std::endl;
                  global_error = true;
                }
                t_reset_uint_ = t_reset.time_since_epoch().count() / 1000;
                waiting_for_clock_reset = true;
              }
              timeStart = static_cast<long int>(time(NULL));
              // read temperature

              statusF = GevGetFeatureValue(
                  captureContext->camHandle, "DeviceTemperature", &type,
                  sizeof(cameraTemperatureF), &cameraTemperatureF);
              cameraTemperature = std::to_string(cameraTemperatureF);

              // // network statistics
              statusF += GevGetFeatureValue(
                  captureContext->camHandle, "transferQueueCurrentBlockCount",
                  &type, sizeof(transferQueueCurrentBlockCount),
                  &transferQueueCurrentBlockCount);
              statusF += GevGetFeatureValue(
                  captureContext->camHandle, "transferMaxBlockSize", &type,
                  sizeof(transferMaxBlockSize), &transferMaxBlockSize);
              statusF += GevGetFeatureValueAsString(
                  captureContext->camHandle, "ptpStatus", &type,
                  sizeof(ptp_status), ptp_status);

              if (statusF == GEVLIB_OK) {
                PrintThread{}
                    << "INFO | " << get_timestamp() << " | Temperature "
                    << cameraTemperature << ", transferMaxBlockSize MB "
                    << std::to_string(transferMaxBlockSize)
                    << ", transferQueueCurrentBlockCount "
                    << transferQueueCurrentBlockCount << ", ptpStatus "
                    << std::string(ptp_status) << std::endl;
              } else {
                // if it does not work it typically indicates a larger problem,
                // so better exit (and restart)
                std::cout << std::endl
                          << "FATAL ERROR | " << get_timestamp()
                          << " | Unable to read temperature and other status "
                             "information"
                          << statusF << std::endl;
                global_error = true;
              }

              if ((!noptp) && (std::string(ptp_status) != "Slave")) {
                std::cout << std::endl
                          << "FATAL ERROR | " << get_timestamp()
                          << " | Lost PTP clock synchronization: " << ptp_status
                          << std::endl;
                global_error = true;
              }
            }

            m_latestBuffer = img->address;

            // img->id max number is 65535 for m1280 cammera
            if ((((signed long)img->id + id_offset) - (signed long)last_id) <
                -1000) { // do not use 65535 in case frames are missed
              id_offset = id_offset + 65535;
              std::cout << std::endl
                        << "INFO | " << get_timestamp()
                        << " | frame id overflow detected " << img->id
                        << " offset " << id_offset << std::endl;
            }

            // if clock jumps back by at least 1 second, we assume the camera
            // clock was reset
            if ((noptp) && (last_cameratimestamp >= 0) &&
                ((((signed long)img->timestamp) - last_cameratimestamp) <
                 -1e6)) {
              std::cout << std::endl
                        << "INFO | " << get_timestamp()
                        << " | detected clock reset between "
                        << (img->timestamp) << " and " << last_cameratimestamp
                        << ". ID " << img->id << std::endl;
              reset_clock_detected = true;
              after_first_reset = true;
              waiting_for_clock_reset = false;

              t_reset_uint_applied = t_reset_uint_;
            } else if (first_image) {
              t_reset_uint_applied = t_reset_uint_;

            } else {
              reset_clock_detected = false;
            }

            // creates new file!
            if (reset_clock_detected || first_image) {
              framesInFile = 0;
            }
            first_image = false; // applies only to very first image

            // UINT32 TotalBuffers ;
            // UINT32 NumUsed ;
            // UINT32 NumFree;
            // UINT32 NumTrashed;
            // GevBufferCyclingMode Mode;
            // UINT32 pTotalBuffers  ;
            // UINT32 pNumUsed  ;
            // UINT32 pNumFree  ;
            // UINT32 pNumTrashed;
            // GevBufferCyclingMode *pMode ;
            // status = GevQueryTransferStatus (captureContext->camHandle,
            // &pTotalBuffers, &pNumUsed,
            // &pNumFree, &pNumTrashed,
            //  pMode);
            // if (pNumUsed>0) {
            //                     printf("%d ",pTotalBuffers);
            //                     printf("%d ",pNumUsed);
            //                     printf("%d ",pNumFree);
            //                     printf("%d ",pNumTrashed);
            //                     printf("%d ",pMode);
            //                     printf("%d ",img->id);
            // }

            if ((captureContext->enable_sequence) || (sequence_init == 1)) {

              high_resolution_clock::time_point t1(
                  high_resolution_clock::now());

              // Export to OpenCV Mat object using SapBuffer data directly
              cv::Mat exportImg(img->h, img->w, OpenCV_Type, m_latestBuffer);
              cv::Size imgSize(img->w, img->h + frameborder);

              // cv::Size imgSize = exportImg.size();
              // std:: cout << "STATUS | " << get_timestamp() << "| " << imgSize
              // << std::endl;

              MatMeta exportImgMeta;
              if (!sequence_init) {
                // init
                //--- INITIALIZE VIDEOWRITER
                for (int ss = 0; ss < nStorageThreads; ++ss) {
                  storage.emplace_back(std::ref(queue[ss]), ss,
                                       captureContext->base_name, codec,
                                       captureContext->fps, imgSize, isColor,
                                       captureContext->live_window_frame_ratio);
                }
                for (int uu = 0; uu < nStorageThreads; ++uu) {
                  // And start the worker threads for each storage worker
                  storage_thread.emplace_back(&storage_worker_cv::run,
                                              &storage[uu]);
                }
                // std::cout << "INFOmain 4: "  << std::endl;

                sequence_init = 1;
                sequence_count = 0;
                // }
              }
              // Insert a copy into all queues
              // for (auto& q : queue) {

              // Now the main capture loop
              exportImgMeta.MatImage = exportImg.clone();
              exportImgMeta.timestamp = img->timestamp + t_reset_uint_applied;
              exportImgMeta.recordtime = tr.time_since_epoch().count() / 1000;

              exportImgMeta.id = img->id + id_offset;

              // img->id max number is 65535
              if ((last_id >= 0) && (exportImgMeta.id != last_id + 1)) {
                std::cout << std::endl
                          << "ERROR | " << get_timestamp()
                          << " | missed frames between " << last_id << " and "
                          << exportImgMeta.id << std::endl;
              }

              if (queryGain) {
                GevGetFeatureValue(captureContext->camHandle, "ExposureTime",
                                   &type, sizeof(valF), &valF);
                exportImgMeta.ExposureTime = valF;
                GevGetFeatureValue(captureContext->camHandle, "Gain", &type,
                                   sizeof(valF), &valF);
                exportImgMeta.Gain = valF;
              }
              // we need new files in every thread
              if (framesInFile < nStorageThreads) {
                exportImgMeta.newFile = true;

                // GevGetFeatureValue( captureContext->camHandle,
                // "transferQueueMemorySize",  &type, sizeof(valF), &valF);
                // std::cout << " transferQueueMemorySize " << valF << " MB" <<
                // type << " ";
                std::cout << std::endl;

              } else {
                exportImgMeta.newFile = false;
              }
              tt = exportImgMeta.id % nStorageThreads;
              // std::cout << "INFOmain 5: "<< tt <<" "<< exportImgMeta.id << "
              // "<< nStorageThreads << std::endl;

              // only process data after clock reset has been confirmed,
              // otherwise timestamps are wrong
              if (!waiting_for_clock_reset) {
                queue[tt].push(exportImgMeta);
              }
              // }
              // std::cout << "INFOmain 6: "  << queue[tt].size() << std::endl;

              high_resolution_clock::time_point t2(
                  high_resolution_clock::now());
              double dt_us(static_cast<double>(
                  duration_cast<microseconds>(t2 - t1).count()));
              total_read_time += dt_us;

              last_id = exportImgMeta.id;
              last_cameratimestamp = img->timestamp;
              // last_utctimestamp_s = exportImgMeta.timestamp/1e6;

              ++frame_count;
              sequence_count++;
              framesInFile++;
              // rest n_timeout after success
              n_timeouts = 0;

              if ((maxframes > 0) && (frame_count >= maxframes)) {
                std::cout << "FATAL ERROR | " << get_timestamp()
                          << " | Reached maximum number of frames" << std::endl;
                global_error = true;
              }

              // See if we  are done.
              if (!captureContext->enable_sequence) {
                // GEVBUFFILE_Close( seqFP, sequence_count );
                std::cout << "INFO | " << get_timestamp()
                          << " | Complete sequence has " << sequence_count
                          << " frames" << std::endl;
                sequence_count = 0;
                sequence_init = 0;

                // writer.release();
              }
            }

            else {
              // printf("chunk_data = %p  : chunk_size = %d\n", img->chunk_data,
              // img->chunk_size); //???????????
              printf("STATUS | WAITING Frame %llu\n",
                     (unsigned long long)img->id);
              fflush(stdout);
            }
          } else {
            // Image had an error (incomplete (timeout/overflow/lost)).
            // Do any handling of this condition necessary.
            std::cerr << "ERROR | " << get_timestamp() << " | Frame " << img->id
                      << " Status = " << img->status << std::endl;
          }
        } else if (status == GEVLIB_ERROR_TIME_OUT) {
          if (captureContext->enable_sequence) {
            // if (followermode) {
            //     if ((new_file_interval == 0) || (timeNow % new_file_interval
            //     == 0)) {
            //         std::cerr << "INFO | " << get_timestamp() <<" | Waiting
            //         for trigger camera"
            //         << std::endl;
            //     } else {
            //         std::cerr << "STATUS | " << get_timestamp() <<" | Waiting
            //         for trigger camera"
            //         << std::endl;
            //     }
            // }
            // else {
            n_timeouts += 1;
            std::cerr << "ERROR | " << get_timestamp() << " | Camera time out"
                      << " #" << n_timeouts << std::endl;
            // }
          }
        }
      } else {
        n_timeouts += 1;
        std::cerr << "ERROR | " << get_timestamp() << " | Could not get image "
                  << status << " #" << n_timeouts << std::endl;
      }

      if (n_timeouts > max_n_timeouts1) {
        std::cerr << "FATAL ERROR | " << get_timestamp()
                  << " | Too many timeouts." << std::endl;
        global_error = true;
      }

      // See if a sequence in progress needs to be stopped here.
      if (global_error ||
          ((!captureContext->enable_sequence) && (sequence_init == 1))) {
        // GEVBUFFILE_Close( seqFP, sequence_count );
        std::cout << "INFO | " << get_timestamp() << " | Complete sequence has "
                  << sequence_count << "frames" << std::endl;
        sequence_count = 0;
        sequence_init = 0;
      }

      // Synchonrous buffer cycling (saving to disk takes time).
#if USE_SYNCHRONOUS_BUFFER_CYCLING
      if (img != NULL) {
        // Release the buffer back to the image transfer process.
        GevReleaseImage(captureContext->camHandle, img);
      }
#endif

      if (global_error) {
        captureContext->enable_sequence = 0;
        captureContext->exit = 1;
        std::cerr << "FATAL ERROR | " << get_timestamp()
                  << " | Error detected. Exiting capture loop..." << std::endl;
      }

    } // while

    if (was_active) {

      // We're done reading, cancel all the queues
      for (auto &q : queue) {
        q.cancel();
      }

      // And join all the worker threads, waiting for them to finish
      for (int ss = 0; ss < nStorageThreads; ++ss) {
        storage_thread[ss].join();

        // for (auto& st2 : storage_thread) {
        //     st2.join();
        // }
        // Report the timings
        total_read_time /= 1000.0;
        double total_storage_time(storage[ss].total_time_ms());
        // double total_write_time_a(storage[0].storage[0].total_time_ms());
        //  double total_write_time_b(storage[1].total_time_ms());

        std::cout << "INFO-" << ss << " | " << get_timestamp()
                  << " | Completed storage " << frame_count << " images,"
                  << " average capture time = "
                  << (total_read_time / frame_count) << " ms,"
                  << " average storage time = "
                  << (total_storage_time / frame_count) << " ms\n"
            //<< "  average write time A = " << (total_write_time_a /
            //frame_count) << " ms\n"
            ;
      }
    }
  }
  pthread_exit(0);
}

/**
 * @brief Check if TurboDrive is available for camera
 * @param handle Camera handle
 * @return 1 if available, 0 if not
 */
int IsTurboDriveAvailable(GEV_CAMERA_HANDLE handle) {
  int type;
  UINT32 val = 0;

  if (0 == GevGetFeatureValue(handle, "transferTurboCurrentlyAbailable", &type,
                              sizeof(UINT32), &val)) {
    // Current / Standard method present - this feature indicates if TurboMode
    // is available. (Yes - it is spelled that odd way on purpose).
    return (val != 0);
  } else {
    // Legacy mode check - standard feature is not there try it manually.
    char pxlfmt_str[64] = {0};

    // Mandatory feature (always present).
    GevGetFeatureValueAsString(handle, "PixelFormat", &type, sizeof(pxlfmt_str),
                               pxlfmt_str);

    // Set the "turbo" capability selector for this format.
    if (0 != GevSetFeatureValueAsString(
                 handle, "transferTurboCapabilitySelector", pxlfmt_str)) {
      // Either the capability selector is not present or the pixel format is
      // not part of the capability set. Either way - TurboMode is NOT
      // AVAILABLE.....
      return 0;
    } else {
      // The capabilty set exists so TurboMode is AVAILABLE.
      // It is up to the camera to send TurboMode data if it can - so we let it.
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Main entry point for the data acquisition program
 * @param argc Number of command line arguments
 * @param argv Array of command line arguments
 * @return Exit status
 */
int main(int argc, char *argv[]) {
  // GEV_DEVICE_INTERFACE  pCamera[MAX_CAMERAS] = {0};
  GEV_STATUS status;
  GEV_STATUS status2;
  int numCamera = 0;
  MY_CONTEXT context = {0};
  pthread_t tid;
  char c;
  int res = 0;
  int writeallframes1;
  int noptp1;
  int rotateImage1;
  int queryGain1;
  int followermode1;
  int rotate1;
  int minBrightnessChange;
  FILE *fp = NULL;
  FILE *fp2 = NULL;
  // char uniqueName[FILENAME_MAX];
  char filename[FILENAME_MAX] = {0};
  // uint32_t macLow = 0; // Low 32-bits of the mac address (for file naming).
  int error_count = 0;
  int feature_count = 0;

  nice(-15);

  //============================================================================
  // Greetings
  std::cout << "VISSS data acquisition (" << __DATE__ << ")" << std::endl;
  std::cout << "***************************************************************"
               "***********"
            << std::endl;

  cv ::CommandLineParser parser(argc, argv, params);

  if (parser.has("help")) {
    parser.printMessage();
    return 0;
  }

  cv::String output = parser.get<cv::String>("output");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: Output path "
            << output << std::endl;

  configFile = parser.get<cv::String>(0);
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: Configuration file "
            << configFile << std::endl;

  std::string camIP = parser.get<cv::String>(1);
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: Camera IP " << camIP
            << std::endl;
  uint long camIPl = iptoul(camIP);
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: Camera IP long "
            << camIPl << std::endl;
  int camIndex = 0;

  encoding = parser.get<cv::String>("encoding");
  // std::cout << "DEBUG | " << get_timestamp() << " | PARSER: FFMPEG encoding
  // "<< encoding << std::endl;
  replace(encoding.begin(), encoding.end(), '@', ' ');
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: FFMPEG encoding "
            << encoding << std::endl;

  site = parser.get<cv::String>("site");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: site " << site
            << std::endl;

  name = parser.get<cv::String>("name");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: name " << name
            << std::endl;

  nStorageThreads = parser.get<int>("threads");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: threads "
            << nStorageThreads << std::endl;

  context.live_window_frame_ratio = parser.get<int>("liveratio");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: liveratio "
            << context.live_window_frame_ratio << std::endl;

  new_file_interval = parser.get<int>("newfileinterval");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: newfileinterval "
            << new_file_interval << std::endl;

  context.fps = parser.get<int>("fps");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: fps " << context.fps
            << std::endl;
  // if there is more than one thread, reduce frame rate of the output
  // accordingly
  context.fps = context.fps / nStorageThreads;

  maxframes = parser.get<int>("maxframes");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: maxframes "
            << maxframes << std::endl;

  writeallframes1 = parser.get<int>("writeallframes");
  if (writeallframes1 == 0) {
    writeallframes = false;
  } else if (writeallframes1 == 1) {
    writeallframes = true;
  } else {
    std::cerr << "FATAL ERROR | " << get_timestamp()
              << "| writeallframes must be 0 or 1 " << writeallframes1
              << std::endl;
    global_error = true;
  }
  followermode1 = parser.get<int>("followermode");
  if (followermode1 == 0) {
    max_n_timeouts1 = max_n_timeouts;
  } else if (followermode1 == 1) {
    max_n_timeouts1 =
        max_n_timeouts * 10; // tolerate more timeouts as a follower
  } else {
    std::cerr << "FATAL ERROR | " << get_timestamp()
              << "| followermode must be 0 or 1 " << followermode1 << std::endl;
    global_error = true;
  }
  noptp1 = parser.get<int>("noptp");
  if (noptp1 == 0) {
    noptp = false;
  } else if (noptp1 == 1) {
    noptp = true;
  } else {
    std::cerr << "FATAL ERROR | " << get_timestamp()
              << "| noptp must be 0 or 1 " << noptp1 << std::endl;
    global_error = true;
  }

  minBrightnessChange = parser.get<int>("minBrightChange");
  if (minBrightnessChange == 20) {
    range[0] = 20;
    range[1] = 30;
    range[2] = 40;
    range[3] = 60;
    range[4] = 80;
    range[5] = 100;
    range[6] = 120;
    range[7] = 256; // the upper boundary is exclusive;
  } else if (minBrightnessChange == 30) {
    range[0] = 30;
    range[1] = 40;
    range[2] = 60;
    range[3] = 80;
    range[4] = 100;
    range[5] = 120;
    range[6] = 140;
    range[7] = 256; // the upper boundary is exclusive;
  } else {
    std::cerr << "FATAL ERROR | " << get_timestamp()
              << "| minBrightChange must be 20 or 30 " << minBrightnessChange
              << std::endl;
    global_error = true;
  }

  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: writeallframes "
            << writeallframes << " " << writeallframes1 << std::endl;
  showPreview = !parser.has("nopreview");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: showPreview "
            << showPreview << std::endl;
  storeVideo = !parser.has("novideo");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: storeVideo "
            << storeVideo << std::endl;
  storeMeta = !parser.has("nometadata");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: storeMeta "
            << storeMeta << std::endl;
  rotateImage1 = parser.get<int>("rotateimage");
  if (rotateImage1 == 0) {
    rotateImage = false;
  } else if (rotateImage1 == 1) {
    rotateImage = true;
  } else {
    std::cerr << "FATAL ERROR | " << get_timestamp()
              << "| rotateImage must be 0 or 1 " << rotateImage1 << std::endl;
    global_error = true;
  }
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: rotateImage "
            << rotateImage << std::endl;

  queryGain1 = parser.get<int>("querygain");
  if (queryGain1 == 0) {
    queryGain = false;
  } else if (queryGain1 == 1) {
    queryGain = true;
  } else {
    std::cerr << "FATAL ERROR | " << get_timestamp()
              << "| queryGain must be 0 or 1 " << queryGain1 << std::endl;
    global_error = true;
  }
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: queryGain "
            << queryGain << std::endl;

  resetDHCP = parser.has("resetDHCP");
  std::cout << "DEBUG | " << get_timestamp() << " | PARSER: reset " << resetDHCP
            << std::endl;

  gethostname(hostname, HOST_NAME_MAX);

  // Open the file.
  fp = fopen(configFile.c_str(), "r");
  if (fp == NULL) {
    std::cerr << "FATAL ERROR | " << get_timestamp()
              << "| Error opening configuration file " << configFile
              << std::endl;
    global_error = true;
  }

  if (!parser.check()) {
    parser.printErrors();
    return 0;
  }

  // Boost application RT response (not too high since GEV library boosts data
  // receive thread to max allowed) SCHED_FIFO can cause many unintentional side
  // effects. SCHED_RR has fewer side effects. SCHED_OTHER (normal default
  // scheduler) is not too bad afer all.
  if (1) {
    // int policy = SCHED_FIFO;
    int policy = SCHED_RR;
    // int policy = SCHED_OTHER;
    pthread_attr_t attrib;
    int inherit_sched = 0;
    struct sched_param param = {0};

    // Set an average RT priority (increase/decrease to tuner performance).
    param.sched_priority =
        (sched_get_priority_max(policy) - sched_get_priority_min(policy)) / 2;

    // Set scheduler policy
    pthread_setschedparam(
        pthread_self(), policy,
        &param); // Don't care if it fails since we can't do anyting about it.

    // Make sure all subsequent threads use the same policy.
    pthread_attr_init(&attrib);
    pthread_attr_getinheritsched(&attrib, &inherit_sched);
    if (inherit_sched != PTHREAD_INHERIT_SCHED) {
      inherit_sched = PTHREAD_INHERIT_SCHED;
      pthread_attr_setinheritsched(&attrib, inherit_sched);
    }
  }

  //===================================================================================
  // Set default options for the library.
  {
    GEVLIB_CONFIG_OPTIONS options = {0};

    GevGetLibraryConfigOptions(&options);
    // options.logLevel = GEV_LOG_LEVEL_OFF;
    // options.logLevel = GEV_LOG_LEVEL_TRACE;
    options.logLevel = GEV_LOG_LEVEL_NORMAL;
    GevSetLibraryConfigOptions(&options);
  }

  //====================================================================================
  // DISCOVER Cameras
  //
  status = GevApiInitialize();
  // std::cout << "STATUS | " << get_timestamp() << " | " << numCamera << "
  // camera(s) on the network"<< std::endl;

  // Select the first camera found (unless the command line has a parameter =
  // the camera index)

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

  char DeviceFirmwareVersion[64] = {0};

  //====================================================================
  // Open the camera.
  status = GevOpenCameraByAddress(camIPl, GevExclusiveMode, &handle);
  if (status == 0) {
    //===================================================================
    // Get the XML file onto disk and use it to make the CNodeMap object.
    char xmlFileName[MAX_PATH] = {0};

    status =
        Gev_RetrieveXMLFile(handle, xmlFileName, sizeof(xmlFileName), false);
    if (status == GEVLIB_OK) {
      // printf("XML stored as %s\n", xmlFileName);
      Camera._LoadXMLFromFile(xmlFileName);
    }

    // Connect the features in the node map to the camera handle.
    status = GevConnectFeatures(handle, static_cast<void *>(&Camera));
    if (status != 0) {
      printf("Error %d connecting node map to handle\n", status);
    }

    // Put the camera in "streaming feature mode".
    GenApi ::CCommandPtr start =
        Camera._GetNode("Std::DeviceRegistersStreamingStart");
    if (start) {
      try {
        int done = false;
        int timeout = 5;
        start->Execute();
        while (!done && (timeout-- > 0)) {
          Sleep(10);
          done = start->IsDone();
        }
      }
      // Catch all possible exceptions from a node access.
      CATCH_GENAPI_ERROR(status);
    }

    std::cout << "DEBUG | " << get_timestamp() << " | Loading settings"
              << std::endl;
    std::cout << "*************************************************************"
                 "*************"
              << std::endl;

    // handle resets
    if ((status == 0) and (resetDHCP)) {
      status2 = GevSetFeatureValueAsString(
          handle, "GevCurrentIPConfigurationDHCP", "1");
      status2 += GevSetFeatureValueAsString(
          handle, "GevCurrentIPConfigurationPersistentIP", "0");

      if (status2 != 0) {
        std::cerr << "FATAL ERROR | " << get_timestamp()
                  << " | Error resetting camera" << std::endl;
      } else {
        std::cout << "INFO | " << get_timestamp() << "| "
                  << "Reset CameraDHCP. Exiting" << std::endl;
      }
      global_error = true;

    }

    // Read the file as { feature value } pairs and write them to the camera.
    else if (status == 0) {
      char feature_name[MAX_GEVSTRING_LENGTH + 1] = {0};
      char value_str[MAX_GEVSTRING_LENGTH + 1] = {0};

      while (2 == fscanf(fp, "%s %s", feature_name, value_str)) {
        status = 0;
        std::cout << "INFO | " << get_timestamp() << " | Set feature "
                  << feature_name << " : " << value_str << std::endl;
        // Find node and write the feature string (without validation).
        GenApi::CNodePtr pNode = Camera._GetNode(feature_name);
        if (pNode) {
          GenApi ::CValuePtr valNode(pNode);
          try {
            valNode->FromString(value_str, false);
          }
          // Catch all possible exceptions from a node access.
          CATCH_GENAPI_ERROR(status);
          if (status != 0) {
            error_count++;
            std::cout << "ERROR | " << get_timestamp()
                      << " | Error restoring feature " << feature_name
                      << " : with value " << value_str << std::endl;
          } else {
            feature_count++;
          }
        } else {
          error_count++;
          std::cout << "ERROR | " << get_timestamp()
                    << " | Error restoring feature " << feature_name
                    << " : with value " << value_str << std::endl;
        }
      }
      std::cout << "***********************************************************"
                   "***************"
                << std::endl;

      // char feature_name2[] = "timestampControlReset";
      // char value_str2[] = "1";
      // // status = 0;
      // // Find node and write the feature string (without validation).
      // GenApi::CNodePtr pNode = Camera._GetNode(feature_name2);
      // if (pNode)
      // {
      // GenApi ::CValuePtr valNode(pNode);
      // try {
      // valNode->FromString(value_str2, false);
      // }
      // // Catch all possible exceptions from a node access.
      // CATCH_GENAPI_ERROR(status);
      // if (status != 0)
      // {
      // error_count++;
      // printf("Error restoring feature %s : with value %s\n", feature_name,
      // value_str);
      // }
      // else
      // {
      // feature_count++;
      // }
      // }
      // else
      // {
      // error_count++;
      // printf("Error restoring feature %s : with value %s\n", feature_name2,
      // value_str2);
      // }

      // context.t_reset = std::chrono::system_clock::now();

      // std::cout << "INFO | " << get_timestamp() << "| Camera clock reset
      // around " << context.t_reset.time_since_epoch().count()/1000  <<
      // std::endl;

      // get device ID
      GevGetFeatureValue(handle, "DeviceID", &type, sizeof(DeviceID),
                         &DeviceID);
      DeviceIDMeta += DeviceID;
    }
    // End the "streaming feature mode".
    GenApi ::CCommandPtr end =
        Camera._GetNode("Std::DeviceRegistersStreamingEnd");
    if (end) {
      try {
        int done = false;
        int timeout = 5;
        end->Execute();
        while (!done && (timeout-- > 0)) {
          Sleep(10);
          done = end->IsDone();
        }
      }
      // Catch all possible exceptions from a node access.
      CATCH_GENAPI_ERROR(status);
    }

    // Validate.
    if (status == 0) {
      // Iterate through all of the features calling "Get" with validation
      // enabled. Find the standard "Root" node and dump the features.
      GenApi::CNodePtr pRoot = Camera._GetNode("Root");
      ValidateFeatureValues(pRoot);
    }

    if (error_count == 0) {
      std::cout << "DEBUG | " << get_timestamp() << "| " << feature_count
                << " Features loaded successfully" << std::endl;
    } else {
      std::cerr << "FATAL ERROR | " << get_timestamp() << "| " << feature_count
                << " Features loaded successfully, " << error_count
                << " Features had errors " << std::endl;
      global_error = true;
    }

    GevGetFeatureValue(handle, "DeviceFirmwareVersion", &type,
                       sizeof(DeviceFirmwareVersion), &DeviceFirmwareVersion);
    std::cout << "INFO | " << get_timestamp() << "| DeviceFirmwareVersion "
              << DeviceFirmwareVersion << std::endl;

    // get ALL settings to dump the to file
    time_t t = time(0); // get time now
    struct tm *now = localtime(&t);
    char timestamp3[80];
    strftime(timestamp3, 80, "%Y%m%d-%H%M%S", now);

    // std::string full_path = output + "/" + hostname + "_" + name + "_" +
    // DeviceID + "/applied_config/" ; std::string config_out = full_path +
    // hostname + "_" + DeviceID + "_" + timestamp3 + ".config" ;

    // res = mkdir_p(full_path.c_str());
    // if (res != 0) {
    //     std::cerr << "FATAL ERROR | " << get_timestamp() << " | Cannot create
    //     path "<< full_path <<std::endl; global_error = true;
    // }
    // // Open the file to dump the configuration
    // fp2 = fopen(config_out.c_str(), "w");
    // if (fp2 == NULL)
    // {
    //     std::cerr << "FATAL ERROR | " << get_timestamp() << " | Error opening
    //     configuration file "<< config_out <<std::endl; global_error = true;
    // }

    // Put the camera in "streaming feature mode".
    GenApi::CCommandPtr start1 =
        Camera._GetNode("Std::DeviceFeaturePersistenceStart");
    if (start1) {
      try {
        int done = false;
        int timeout = 5;
        start1->Execute();
        while (!done && (timeout-- > 0)) {
          Sleep(10);
          done = start1->IsDone();
        }
      }
      // Catch all possible exceptions from a node access.
      CATCH_GENAPI_ERROR(status);
    }

    // Traverse the node map and dump all the { feature value } pairs.
    if (status == 0) {
      // Find the standard "Root" node and dump the features.
      GenApi::CNodePtr pRoot = Camera._GetNode("Root");
      // OutputFeatureValues( pRoot, fp2);
    }

    // End the "streaming feature mode".
    std::cout << "DEBUG | " << get_timestamp() << "| "
              << "Ending streaming feature mode" << std::endl;
    GenApi::CCommandPtr end1 =
        Camera._GetNode("Std::DeviceFeaturePersistenceEnd");
    if (end1) {
      try {
        int done = false;
        int timeout = 5;
        end1->Execute();
        while (!done && (timeout-- > 0)) {
          Sleep(10);
          done = end1->IsDone();
        }
      }
      // Catch all possible exceptions from a node access.
      CATCH_GENAPI_ERROR(status);
    }

    // fclose(fp2);
    // std::cout << "INFO | " << get_timestamp() << "| applied configuration
    // written to: " << config_out << std::endl;

    if ((not global_error) && (error_count == 0)) {
      GEV_CAMERA_OPTIONS camOptions = {0};

      // Get the low part of the MAC address (use it as part of a unique file
      // name for saving images). Generate a unique base name to be used for
      // saving image files based on the last 3 octets of the MAC address.
      // macLow = pCamera[camIndex].macLow;
      // macLow &= 0x00FFFFFF;
      // snprintf(uniqueName, sizeof(uniqueName), "%s/visss_%06x",
      // output.c_str(), macLow);

      // // If there are multiple pixel formats supported on this camera, get
      // one.
      // {
      //     char feature_name[MAX_GEVSTRING_LENGTH] =  {0};
      //     GetPixelFormatSelection( handle, sizeof(feature_name),
      //     feature_name); if ( GevSetFeatureValueAsString(handle,
      //     "PixelFormat", feature_name) == 0)
      //     {
      //         printf("\n\tUsing selected PixelFormat = %s\n\n",
      //         feature_name);
      //     }
      // }

      // Go on to adjust some API related settings (for tuning / diagnostics /
      // etc....). Adjust the camera interface options if desired (see the
      // manual)
      GevGetCameraInterfaceOptions(handle, &camOptions);
      // camOptions.heartbeat_timeout_ms = 60000;      // For debugging (delay
      // camera timeout while in debugger)
      camOptions.heartbeat_timeout_ms =
          5000; // Disconnect detection (5 seconds)
      camOptions.enable_passthru_mode = false;

      camOptions.streamNumFramesBuffered = 200; // Buffer frames internally.
      camOptions.streamMemoryLimitMax =
          50 * 8 * 2064 * 1544; // Adjust packet memory buffering limit.

#if TUNE_STREAMING_THREADS
      // Some tuning can be done here. (see the manual)
      camOptions.streamFrame_timeout_ms =
          2001; // Internal timeout for frame reception.
      camOptions.streamNumFramesBuffered = 200; // Buffer frames internally.
      camOptions.streamMemoryLimitMax =
          100 * 8 * 2064 * 1544; // Adjust packet memory buffering limit.
      // camOptions.streamPktSize = 8960;                            // Adjust
      // the GVSP packet size. camOptions.streamPktSize = 8960-1; // Adjust the
      // GVSP packet size.
      camOptions.streamPktDelay =
          10; // Add usecs between packets to pace arrival at NIC.
      // Assign specific CPUs to threads (affinity) - if required for better
      // performance.
      {
        int numCpus = _GetNumCpus();
        if (numCpus > 1) {
          camOptions.streamThreadAffinity = 2 * followermode1 + 2;
          camOptions.serverThreadAffinity = 2 * followermode1 + 3;
        }
      }
#endif
      std::cout << "DEBUG | " << get_timestamp() << "| "
                << "Configuring Camera " << std::endl;
      // Write the adjusted interface options back.
      GevSetCameraInterfaceOptions(handle, &camOptions);

      //===========================================================
      // Set up the frame information.....
      // printf("Camera ROI set for \n");
      // GevGetFeatureValue(handle, "Width", &type, sizeof(width), &width);
      // printf("\tWidth = %d\n", width);
      // GevGetFeatureValue(handle, "Height", &type, sizeof(height), &height);
      // printf("\tHeight = %d\n", height);
      // GevGetFeatureValue(handle, "PixelFormat", &type, sizeof(format),
      // &format); printf("\tPixelFormat  = 0x%x\n", format);

      if (camOptions.enable_passthru_mode) {
        // printf("\n\tPASSTHRU Mode is ON\n");
      }

      if (IsTurboDriveAvailable(handle)) {
        val = 1;
        if (GevGetFeatureValue(handle, "transferTurboMode", &type,
                               sizeof(UINT32), &val) == 0) {
          if (val != 1) {
            std::cerr << "FATAL ERROR | " << get_timestamp()
                      << "| Turbodrive off" << std::endl;
            global_error = true;
          } else {
            std::cout << "DEBUG | " << get_timestamp() << "| Turbodrive on"
                      << std::endl;
          }
        }
      } else {
        std::cerr << "FATAL ERROR | " << get_timestamp()
                  << "| TurboDrive is NOT Available" << std::endl;

        global_error = true;
      }

      //
      // End frame info
      //============================================================

      if (!noptp) {
        status = GevGetFeatureValueAsString(handle, "ptpStatus", &type,
                                            sizeof(ptp_status), ptp_status);
        if (status != GEVLIB_OK) {
          // if it does not work it typically indicates a larger problem, so
          // better exit (and restart)
          std::cout << std::endl
                    << "FATAL ERROR | " << get_timestamp()
                    << " | Unable to read ptp statusnformation" << status
                    << std::endl;
          global_error = true;
        }
        int ptpcounter = 0;
        while (std::string(ptp_status) != "Slave") {
          std::cout << "INFO | " << get_timestamp() << "| "
                    << "Waiting for PTP: " << std::string(ptp_status)
                    << std::endl;
          std::this_thread::sleep_for(std::chrono::seconds(1));
          status = GevGetFeatureValueAsString(handle, "ptpStatus", &type,
                                              sizeof(ptp_status), ptp_status);
          if (status != GEVLIB_OK) {
            // if it does not work it typically indicates a larger problem, so
            // better exit (and restart)
            std::cout << std::endl
                      << "FATAL ERROR | " << get_timestamp()
                      << " | Unable to read ptp statusnformation" << status
                      << std::endl;
            global_error = true;
            break;
          }
          if (ptpcounter > 30) {
            // if it does not work it typically indicates a larger problem, so
            // better exit (and restart)
            std::cout << std::endl
                      << "FATAL ERROR | " << get_timestamp()
                      << " | Unable to synchronize PTP clock: "
                      << std::string(ptp_status) << std::endl;
            global_error = true;
            break;
          }

          ++ptpcounter;
        }
      }

      if ((not global_error) && (status == 0)) {
        //=================================================================
        // Set up a grab/transfer from this camera based on the settings...
        //
        GevGetPayloadParameters(handle, &payload_size, (UINT32 *)&type);
        maxHeight = height;
        maxWidth = width;
        maxDepth = GetPixelSizeInBytes(format);

        // Calculate the size of the image buffers.
        // (Adjust the number of lines in the buffer to fit the maximum expected
        //   chunk size - just in case it gets enabled !!!)
        {
          int extra_lines = (MAX_CHUNK_BYTES + width - 1) / width;
          size = GetPixelSizeInBytes(format) * width * (height + extra_lines);
        }

        // Allocate image buffers
        // (Either the image size or the payload_size, whichever is larger -
        // allows for packed pixel formats and metadata).
        size = (payload_size > size) ? payload_size : size;

        for (i = 0; i < numBuffers; i++) {
          bufAddress[i] = (PUINT8)malloc(size);
          memset(bufAddress[i], 0, size);
        }

        // Generate a file name from the unique base name
        // (leave at least 16 digits for index and extension)
        // _GetUniqueFilename_sec(filename, (sizeof(filename) - 17),
        // uniqueName);

        // Initialize a transfer with synchronous buffer handling.
        // (To avoid overwriting data buffer while saving to disk).
        std::cout << "DEBUG | " << get_timestamp() << "| "
                  << "Inititalizing Transfer" << std::endl;
#if USE_SYNCHRONOUS_BUFFER_CYCLING
        // Initialize a transfer with synchronous buffer handling.
        status = GevInitializeTransfer(handle, SynchronousNextEmpty, size,
                                       numBuffers, bufAddress);
#else
        // Initialize a transfer with asynchronous buffer handling.
        status = GevInitializeTransfer(handle, Asynchronous, size, numBuffers,
                                       bufAddress);
#endif
        // Create a thread to receive images from the API and save them
        context.camHandle = handle;
        context.base_name = output;
        context.exit = false;

        pthread_create(&tid, NULL, ImageCaptureThread, &context);

        // Call the main command loop or the example.
        PrintMenu();

        for (i = 0; i < numBuffers; i++) {
          memset(bufAddress[i], 0, size);
        }
        // std::cout << "STATUS | " << get_timestamp() <<" | STARTING
        // GevStartTransfer" <<std::endl;

        std::cout << "DEBUG | " << get_timestamp() << "| "
                  << "Starting Transfer" << std::endl;
        status = GevStartTransfer(handle, -1);
        if (status != 0) {
          std::cerr << "FATAL ERROR | " << get_timestamp()
                    << " | Error starting grab" << std::endl;
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

        while (!(global_error) && (!done)) {
          alarm(1);
          c = GetKey();

          if ((c == 0x1b) || (c == 'q') || (c == 'Q')) {
            done = true;
          }
        }

        context.enable_sequence = 0; // End sequence if active.
        GevStopTransfer(handle);
        context.exit = true;
        pthread_join(tid, NULL);

        // std::cout << "STATUS | " << get_timestamp() <<" | STOPPING
        // GevStartTransfer" <<std::endl;
        GevAbortTransfer(handle);
        status = GevFreeTransfer(handle);
        for (i = 0; i < numBuffers; i++) {
          free(bufAddress[i]);
        }
      }
    }
    GevCloseCamera(&handle);
  } else {
    std::cerr << "FATAL ERROR | " << get_timestamp() << " | Error : " << status
              << " : opening camera" << std::endl;

    std::cerr << "ERRORS: " << GEVLIB_ERROR_API_NOT_INITIALIZED << " FATAL "
              << GEVLIB_ERROR_INVALID_HANDLE
              << " FATAL " GEVLIB_ERROR_INSUFFICIENT_MEMORY
              << " FATAL " GEVLIB_ERROR_NO_CAMERA << " FATAL "
              << GEV_STATUS_ACCESS_DENIED << std::endl;
    global_error = true;
  }

  // Close down the API.
  GevApiUninitialize();

  // Close socket API
  _CloseSocketAPI(); // must close API even on error

  // printf("Hit any key to exit\n");
  // kbhit();

  if (global_error) {
    std::cerr << "FATAL ERROR | " << get_timestamp()
              << " | EXIT due to fatal error" << std::endl;
    return 1;
  } else {
    std::cout << "INFO | " << get_timestamp() << " | All done" << std::endl;
    return 0;
  }
}
