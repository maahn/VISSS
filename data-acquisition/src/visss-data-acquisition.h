#define TELEDYNEDALSA_CHUNK_SIZE_DEVICEID      0x00000010
#define TELEDYNEDALSA_CHUNK_SIZE_DEVICEUSERID  0x00000010


#include "stdio.h"
#include <sys/time.h>
#include <time.h>
#include <math.h>

#include <fstream>
#include <limits.h>     /* PATH_MAX */
#include <sys/stat.h>   /* mkdir(2) */
#include <errno.h>


#include <sched.h>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/core/utils/logger.hpp>

#include <iostream>
#include <boost/algorithm/string.hpp>
#include <boost/container/vector.hpp>

// #include <opencv2/opencv.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <unistd.h>
#include <set>
#include <signal.h>
#include <stdlib.h>
#include <iomanip>

// ============================================================================
using std::chrono::high_resolution_clock;
using std::chrono::duration_cast;
using std::chrono::microseconds;

// ============================================================================



char DeviceID[TELEDYNEDALSA_CHUNK_SIZE_DEVICEID];
std::string DeviceIDMeta;
char hostname[HOST_NAME_MAX];
int n_timeouts = 0;
int max_n_timeouts = 30;
bool global_error = false;
int done = false;
cv::String configFile;
std::string site = "none";
std::string name = "VISSS";
int maxframes = -1;
int frameborder = 64;
int new_file_interval = 300;
bool writeallframes = false;
bool followermode = false;
bool storeVideo = true;
bool storeMeta = true;
bool showPreview = true;
bool queryGain = false;
int nStorageThreads = 1;
std::string encoding;
std::chrono::time_point<std::chrono::system_clock> t_reset;
unsigned long t_reset_uint_ = 0;
unsigned long t_reset_uint_applied = 0;

//histogram
//float range[] = {20,30,40,60,80,100,120, 256  }; //the upper boundary is exclusive
float range[8];// = {30,40,60,80,100,120, 140, 256  }; //the upper boundary is exclusive
int histSize = 7;
const float* histRange = { range };
int minMovingPixel = 20;


struct MatMeta {
  cv::Mat MatImage;
  unsigned long timestamp;
  unsigned long recordtime;
  unsigned long id;
  bool newFile;
  float ExposureTime;
  float Gain;
}; 

std::string get_timestamp(){
  char buffer[26];
  int millisec;
  struct tm* tm_info;
  struct timeval tv;

  gettimeofday(&tv, NULL);

  millisec = lrint(tv.tv_usec/100000.0); // Round to nearest tenth sec
  if (millisec>=10) { // Allow for rounding up to nearest second
    millisec -=10;
    tv.tv_sec++;
  }

  tm_info = localtime(&tv.tv_sec);

  strftime(buffer, 26, "%y-%m-%d %H:%M:%S", tm_info);
  //int n_zero = 3;

  std::string millisec_str = std::to_string(millisec);
  std::string t_string(buffer, strlen(buffer));
  //t_string = t_string + '.' + std::string(n_zero - millisec_str.length(), '0') + millisec_str;
  t_string = t_string + '.' + millisec_str;

  return t_string;
   
}


void create_symlink(std::string target, std::string link) 
{
    int result;

    char tmp[200];
    strcpy(tmp, link.c_str());
    strcat(tmp, ".tmp");

    result = symlink(target.c_str(), tmp);
    rename(tmp, link.c_str());

}


void signal_handler(int s){
   std::cout << "INFO | " << get_timestamp() << "| Catched signal Ctrl-C" <<std::endl;
   done = true; 

}
void signal_handler_null(int s) {}

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

            if (mkdir(_path, (S_IRWXU | S_IRGRP |S_IXGRP |S_IROTH |S_IXOTH| S_ISVTX  )) != 0) {
                if (errno != EEXIST)
                    return -1; 
            }

            *p = '/';
        }
    }
    //std::cout << "STATUS | " << get_timestamp() << "| Created path " << _path <<std::endl;
    return 0;
}


//https://stackoverflow.com/questions/10167534/how-to-find-out-what-type-of-a-mat-object-is-with-mattype-in-opencv
std::string type2str(int type) {
  std::string r;

  uchar depth = type & CV_MAT_DEPTH_MASK;
  uchar chans = 1 + (type >> CV_CN_SHIFT);

  switch ( depth ) {
    case CV_8U:  r = "8U"; break;
    case CV_8S:  r = "8S"; break;
    case CV_16U: r = "16U"; break;
    case CV_16S: r = "16S"; break;
    case CV_32S: r = "32S"; break;
    case CV_32F: r = "32F"; break;
    case CV_64F: r = "64F"; break;
    default:     r = "User"; break;
  }

  r += "C";
  r += (chans+'0');

  return r;
}
//std::string ty =  type2str( nPixel.type() );
//printf("nPixel: %s %dx%d \n", ty.c_str(), nPixel.cols, nPixel.rows );


//https://stackoverflow.com/questions/1120140/how-can-i-read-and-parse-csv-files-in-c


std::vector<std::string> getNextLineAndSplitIntoTokens(std::istream& str)
{
    std::vector<std::string>   result;
    std::string                line;
    std::getline(str,line);

    std::stringstream          lineStream(line);
    std::string                cell;

    while(std::getline(lineStream,cell, ','))
    {
        result.push_back(cell);
    }
    // This checks for a trailing comma with no data after it.
    if (!lineStream && cell.empty())
    {
        // If there was a trailing comma then add an empty element.
        result.push_back("");
    }
    return result;
}

//https://stackoverflow.com/questions/1798112/removing-leading-and-trailing-spaces-from-a-string
std::string trim(const std::string& str,
                 const std::string& whitespace = " \t")
{
    const auto strBegin = str.find_first_not_of(whitespace);
    if (strBegin == std::string::npos)
        return ""; // no content

    const auto strEnd = str.find_last_not_of(whitespace);
    const auto strRange = strEnd - strBegin + 1;

    return str.substr(strBegin, strRange);
}

//https://bytes.com/topic/c/answers/451533-convert-ip-address-long-value
unsigned long iptoul(std::string ipstr)
{

char *ip = &ipstr[0];

char i,*tmp;
unsigned long val=0, cvt;

tmp=strtok(ip,".");
for (i=0;i<4;i++)
{
sscanf(tmp,"%lu",&cvt);
val<<=8;
val|=(unsigned char)cvt;
tmp=strtok(NULL,".");
}
return(val);
}


// https://stackoverflow.com/questions/14718124/how-to-easily-make-stdcout-thread-safe/47480827#47480827
/** Thread safe cout class
  * Exemple of use:
  *    PrintThread{} << "Hello world!" << std::endl;
  */
class PrintThread: public std::ostringstream
{
public:
    PrintThread() = default;

    ~PrintThread()
    {
        std::lock_guard<std::mutex> guard(_mutexPrint);
        std::cout << this->str();
    }

private:
    static std::mutex _mutexPrint;
};

std::mutex PrintThread::_mutexPrint{};

// https://stackoverflow.com/questions/34857119/how-to-convert-stdchronotime-point-to-string
using time_point = std::chrono::system_clock::time_point;
std::string serializeTimePoint( const time_point& time, const std::string& format)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::gmtime(&tt); //GMT (UTC)
    //std::tm tm = *std::localtime(&tt); //Locale time-zone, usually UTC by default.
    std::stringstream ss;
    ss << std::put_time( &tm, format.c_str() );
    return ss.str();
}