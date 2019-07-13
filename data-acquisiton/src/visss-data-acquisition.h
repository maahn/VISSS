#define TELEDYNEDALSA_CHUNK_SIZE_DEVICEID      0x00000010
#define TELEDYNEDALSA_CHUNK_SIZE_DEVICEUSERID  0x00000010


#include "stdio.h"
#include <fstream>
#include <limits.h>     /* PATH_MAX */
#include <sys/stat.h>   /* mkdir(2) */
#include <errno.h>

// #include "cordef.h"
#include "GenApi/GenApi.h"      //!< GenApi lib definitions.
#include "gevapi.h"             //!< GEV lib definitions.
// #include "gevbuffile.h"

#include <sched.h>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/core/utility.hpp>
#include <iostream>

// #include <opencv2/opencv.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
// ============================================================================
using std::chrono::high_resolution_clock;
using std::chrono::duration_cast;
using std::chrono::microseconds;

// ============================================================================



char DeviceID[TELEDYNEDALSA_CHUNK_SIZE_DEVICEID];

struct MatMeta {
  cv::Mat MatImage;
  unsigned long timestamp;
  unsigned long id;
}; 