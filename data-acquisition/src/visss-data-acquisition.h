/**
 * @file visss-data-acquisition.h
 * @brief Header file for VISSS data acquisition system
 * 
 * This header file contains declarations for the VISSS data acquisition system,
 * including structures, constants, and function prototypes.
 */

#define TELEDYNEDALSA_CHUNK_SIZE_DEVICEID 0x00000010
#define TELEDYNEDALSA_CHUNK_SIZE_DEVICEUSERID 0x00000010

#include "stdio.h"
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include <errno.h>
#include <fstream>
#include <limits.h>   /* PATH_MAX */
#include <sys/stat.h> /* mkdir(2) */

#include <sched.h>

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/container/vector.hpp>
#include <iostream>

// #include <opencv2/opencv.hpp>

#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <queue>
#include <set>
#include <signal.h>
#include <stdlib.h>
#include <thread>
#include <unistd.h>

#include <ctime>
#include <sstream>

// ============================================================================
/**
 * @brief High resolution clock type alias
 */
using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::microseconds;

// ============================================================================

/**
 * @brief Device ID buffer for camera
 */
char DeviceID[TELEDYNEDALSA_CHUNK_SIZE_DEVICEID];
/**
 * @brief Device ID metadata string
 */
std::string DeviceIDMeta;
/**
 * @brief Camera temperature string
 */
std::string cameraTemperature = "nan";
/**
 * @brief Camera temperature float value
 */
float cameraTemperatureF;
/**
 * @brief Transfer queue current block count
 */
int transferQueueCurrentBlockCount = -99;
/**
 * @brief Transfer maximum block size
 */
float transferMaxBlockSize = -99;
/**
 * @brief PTP status string
 */
char ptp_status[64] = {0};

/**
 * @brief Hostname buffer
 */
char hostname[HOST_NAME_MAX];
/**
 * @brief Number of timeouts
 */
int n_timeouts = 0;
/**
 * @brief Maximum number of timeouts
 */
int max_n_timeouts = 30;
/**
 * @brief Global error flag
 */
bool global_error = false;
/**
 * @brief Done flag
 */
int done = false;
/**
 * @brief Configuration file string
 */
cv::String configFile;
/**
 * @brief Site string
 */
std::string site = "none";
/**
 * @brief Name string
 */
std::string name = "VISSS";
/**
 * @brief Maximum frames to capture
 */
int maxframes = -1;
/**
 * @brief Frame border size
 */
int frameborder = 64;
/**
 * @brief New file interval in seconds
 */
int new_file_interval = 300;
/**
 * @brief Write all frames flag
 */
bool writeallframes = false;
/**
 * @brief Maximum number of timeouts for follower mode
 */
int max_n_timeouts1 = 0;
/**
 * @brief Store video flag
 */
bool storeVideo = true;
/**
 * @brief Store metadata flag
 */
bool storeMeta = true;
/**
 * @brief Show preview flag
 */
bool showPreview = true;
/**
 * @brief Rotate image flag
 */
bool rotateImage = false;
/**
 * @brief Query gain flag
 */
bool queryGain = false;
/**
 * @brief Reset DHCP flag
 */
bool resetDHCP = false;
/**
 * @brief No PTP flag
 */
bool noptp = false;
/**
 * @brief Number of storage threads
 */
int nStorageThreads = 1;
/**
 * @brief Encoding string
 */
std::string encoding;
/**
 * @brief Time reset point
 */
std::chrono::time_point<std::chrono::system_clock> t_reset;
/**
 * @brief Time reset unsigned integer
 */
unsigned long t_reset_uint_ = 0;
/**
 * @brief Time reset applied unsigned integer
 */
unsigned long t_reset_uint_applied = 0;

/**
 * @brief ID offset
 */
unsigned long id_offset = 0;

/**
 * @brief Histogram range array
 */
// histogram
// float range[] = {20,30,40,60,80,100,120, 256  }; //the upper boundary is
// exclusive
float range[8]; // = {30,40,60,80,100,120, 140, 256  }; //the upper boundary is
                // exclusive
/**
 * @brief Histogram size
 */
int histSize = 7;
/**
 * @brief Histogram range pointer
 */
const float *histRange = {range};
/**
 * @brief Minimum moving pixel threshold
 */
int minMovingPixel = 20;

/**
 * @brief Structure to hold image metadata
 */
struct MatMeta {
  cv::Mat MatImage;
  unsigned long timestamp;
  unsigned long recordtime;
  unsigned long id;
  bool newFile;
  float ExposureTime;
  float Gain;
};

/**
 * @brief Get formatted timestamp string
 * @return Formatted timestamp string
 */
std::string get_timestamp() {
  char buffer[26];
  int millisec;
  struct tm *tm_info;
  struct timeval tv;

  gettimeofday(&tv, NULL);

  millisec = lrint(tv.tv_usec / 100000.0); // Round to nearest tenth sec
  if (millisec >= 10) { // Allow for rounding up to nearest second
    millisec -= 10;
    tv.tv_sec++;
  }

  tm_info = localtime(&tv.tv_sec);

  strftime(buffer, 26, "%y-%m-%d %H:%M:%S", tm_info);
  // int n_zero = 3;

  std::string millisec_str = std::to_string(millisec);
  std::string t_string(buffer, strlen(buffer));
  // t_string = t_string + '.' + std::string(n_zero - millisec_str.length(),
  // '0') + millisec_str;
  t_string = t_string + '.' + millisec_str;

  return t_string;
}

/**
 * @brief Create symbolic link
 * @param target Target path
 * @param link Link path
 */
void create_symlink(std::string target, std::string link) {
  int result;

  char tmp[200];
  strcpy(tmp, link.c_str());
  strcat(tmp, ".tmp");

  result = symlink(target.c_str(), tmp);
  rename(tmp, link.c_str());
}

/**
 * @brief Signal handler for SIGINT/SIGTERM
 * @param s Signal number
 */
void signal_handler(int s) {
  std::cout << "INFO | " << get_timestamp() << "| Catched signal Ctrl-C"
            << std::endl;
  done = true;
}
/**
 * @brief Null signal handler for SIGALRM
 * @param s Signal number
 */
void signal_handler_null(int s) {}

/**
 * @brief Create directory recursively
 * @param path Path to create
 * @return 0 on success, -1 on failure
 */
int mkdir_p(const char *path) {
  /* Adapted from http://stackoverflow.com/a/2336245/119527 */
  const size_t len = strlen(path);
  char _path[PATH_MAX];
  char *p;

  errno = 0;

  /* Copy string so its mutable */
  if (len > sizeof(_path) - 1) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(_path, path);

  /* Iterate the string */
  for (p = _path + 1; *p; p++) {
    if (*p == '/') {
      /* Temporarily truncate */
      *p = '\0';

      if (mkdir(_path, (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH |
                        S_ISVTX)) != 0) {
        if (errno != EEXIST)
          return -1;
      }

      *p = '/';
    }
  }
  // std::cout << "STATUS | " << get_timestamp() << "| Created path " << _path
  // <<std::endl;
  return 0;
}

/**
 * @brief Convert OpenCV matrix type to string
 * @param type OpenCV matrix type
 * @return String representation of type
 */
std::string type2str(int type) {
  std::string r;

  uchar depth = type & CV_MAT_DEPTH_MASK;
  uchar chans = 1 + (type >> CV_CN_SHIFT);

  switch (depth) {
  case CV_8U:
    r = "8U";
    break;
  case CV_8S:
    r = "8S";
    break;
  case CV_16U:
    r = "16U";
    break;
  case CV_16S:
    r = "16S";
    break;
  case CV_32S:
    r = "32S";
    break;
  case CV_32F:
    r = "32F";
    break;
  case CV_64F:
    r = "64F";
    break;
  default:
    r = "User";
    break;
  }

  r += "C";
  r += (chans + '0');

  return r;
}
// std::string ty =  type2str( nPixel.type() );
// printf("nPixel: %s %dx%d \n", ty.c_str(), nPixel.cols, nPixel.rows );

/**
 * @brief Split CSV line into tokens
 * @param str Input stream
 * @return Vector of strings containing tokens
 */
std::vector<std::string> getNextLineAndSplitIntoTokens(std::istream &str) {
  std::vector<std::string> result;
  std::string line;
  std::getline(str, line);

  std::stringstream lineStream(line);
  std::string cell;

  while (std::getline(lineStream, cell, ',')) {
    result.push_back(cell);
  }
  // This checks for a trailing comma with no data after it.
  if (!lineStream && cell.empty()) {
    // If there was a trailing comma then add an empty element.
    result.push_back("");
  }
  return result;
}

/**
 * @brief Trim whitespace from string
 * @param str Input string
 * @param whitespace Whitespace characters to trim
 * @return Trimmed string
 */
std::string trim(const std::string &str,
                 const std::string &whitespace = " \t") {
  const auto strBegin = str.find_first_not_of(whitespace);
  if (strBegin == std::string::npos)
    return ""; // no content

  const auto strEnd = str.find_last_not_of(whitespace);
  const auto strRange = strEnd - strBegin + 1;

  return str.substr(strBegin, strRange);
}

/**
 * @brief Convert IP address string to unsigned long
 * @param ipstr IP address string
 * @return Unsigned long representation of IP
 */
unsigned long iptoul(std::string ipstr) {

  char *ip = &ipstr[0];

  char i, *tmp;
  unsigned long val = 0, cvt;

  tmp = strtok(ip, ".");
  for (i = 0; i < 4; i++) {
    sscanf(tmp, "%lu", &cvt);
    val <<= 8;
    val |= (unsigned char)cvt;
    tmp = strtok(NULL, ".");
  }
  return (val);
}

/**
 * @brief Thread safe cout class
 * Example of use:
 *    PrintThread{} << "Hello world!" << std::endl;
 */
class PrintThread : public std::ostringstream {
public:
  PrintThread() = default;

  ~PrintThread() {
    std::lock_guard<std::mutex> guard(_mutexPrint);
    std::cout << this->str();
  }

private:
  static std::mutex _mutexPrint;
};

std::mutex PrintThread::_mutexPrint{};

/**
 * @brief Serialize time point to string
 * @param time Time point to serialize
 * @param format Format string
 * @return Formatted time string
 */
using time_point = std::chrono::system_clock::time_point;
std::string serializeTimePoint(const time_point &time,
                               const std::string &format) {
  std::time_t tt = std::chrono::system_clock::to_time_t(time);
  std::tm tm = *std::gmtime(&tt); // GMT (UTC)
  // std::tm tm = *std::localtime(&tt); //Locale time-zone, usually UTC by
  // default.
  std::stringstream ss;
  ss << std::put_time(&tm, format.c_str());
  return ss.str();
}

/**
 * @brief Format Unix time in microseconds
 * @param unixTimeMicros Unix time in microseconds
 * @return Formatted time string
 */
std::string formatUnixTimeMicros(int64_t unixTimeMicros) {
  using namespace std::chrono;

  // Convert microseconds to system_clock::time_point
  auto micros = microseconds(unixTimeMicros);
  auto tp = system_clock::time_point(micros);

  // Extract milliseconds part (3 digits)
  auto ms_part = duration_cast<milliseconds>(micros % seconds(1)).count();

  // Convert to time_t for formatting
  std::time_t tt = system_clock::to_time_t(tp);
  std::tm tm = *std::localtime(&tt);

  // Format time and append milliseconds
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y/%m/%d %H:%M:%S") << '.' << std::setfill('0')
      << std::setw(3) << ms_part;

  return oss.str();
}
