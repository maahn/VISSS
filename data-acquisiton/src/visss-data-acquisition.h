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
#include <unistd.h>
#include <set>

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

std::string get_timestamp(){
    char s[100];

    time_t t = time(NULL);
    struct tm * p = localtime(&t);

    strftime(s, 100, "%y-%m-%d %H:%M:%S", p);

    std::string t_string(s, strlen(s));

    return t_string;
   
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
    std::cout << "STATUS | " << " Created path " << _path <<std::endl;
    return 0;
}
