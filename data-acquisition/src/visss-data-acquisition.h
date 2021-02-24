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

#include "cordef.h"
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
#include <signal.h>
#include <stdlib.h>

// ============================================================================
using std::chrono::high_resolution_clock;
using std::chrono::duration_cast;
using std::chrono::microseconds;

// ============================================================================



char DeviceID[TELEDYNEDALSA_CHUNK_SIZE_DEVICEID];
char hostname[HOST_NAME_MAX];
int n_timeouts = 0;
int max_n_timeouts = 30;
bool global_error = false;
int done = FALSE;
cv::String configFile;
std::string configFileRaw;
int maxframes = -1;
int frameborder = 64;
int new_file_interval = 300;
bool writeallframes = false;

//histogram
float range[] = {10,20,30,40,60,80,100,120, 256  }; //the upper boundary is exclusive
int histSize = 8;
const float* histRange = { range };
int minMovingPixel = 20;


struct MatMeta {
  cv::Mat MatImage;
  unsigned long timestamp;
  unsigned long id;
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
   std::cout << "STATUS | " << get_timestamp() << "| Catched signal Ctrl-C" <<std::endl;
   done = TRUE; 

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

static void OutputFeatureValuePair( const char *feature_name, const char *value_string, FILE *fp )
{
    if ( (feature_name != NULL)  && (value_string != NULL) )
    {
        // Feature : Value pair output (in one place in to ease changing formats or output method - if desired).
        fprintf(fp, "%s %s\n", feature_name, value_string);
    }
}


static void OutputFeatureValues( const GenApi::CNodePtr &ptrFeature, FILE *fp )
{
    
   GenApi::CCategoryPtr ptrCategory(ptrFeature);
   if( ptrCategory.IsValid() )
   {
       GenApi::FeatureList_t Features;
       ptrCategory->GetFeatures(Features);
       for( GenApi::FeatureList_t::iterator itFeature=Features.begin(); itFeature!=Features.end(); itFeature++ )
       {    
          OutputFeatureValues( (*itFeature), fp );
       }
   }
   else
   {
        // Store only "streamable" features (since only they can be restored).
        if ( ptrFeature->IsStreamable() )
        {
            // Create a selector set (in case this feature is selected)
            bool selectorSettingWasOutput = false;
            GenApi::CSelectorSet selectorSet(ptrFeature);
            
            // Loop through all the selectors that select this feature.
            // Use the magical CSelectorSet class that handles the 
            //   "set of selectors that select this feature" and indexes
            // through all possible combinations so we can save all of them.
            selectorSet.SetFirst();
            do
            {
                GenApi::CValuePtr valNode(ptrFeature);  
                if ( valNode.IsValid() && (GenApi::RW == valNode->GetAccessMode()) && (ptrFeature->IsFeature()) )
                {
                    // Its a valid streamable feature.
                    // Get its selectors (if it has any)
                    GenApi::FeatureList_t selectorList;
                    selectorSet.GetSelectorList( selectorList, true );

                    for ( GenApi::FeatureList_t ::iterator itSelector=selectorList.begin(); itSelector!=selectorList.end(); itSelector++ )  
                    {
                        // Output selector : selectorValue as a feature : value pair.
                        selectorSettingWasOutput = true;
                        GenApi::CNodePtr selectedNode( *itSelector);
                        GenApi::CValuePtr selectedValue( *itSelector);
                        OutputFeatureValuePair(static_cast<const char *>(selectedNode->GetName()), static_cast<const char *>(selectedValue->ToString()), fp);
                    }
                        
                    // Output feature : value pair for this selector combination 
                    // It just outputs the feature : value pair if there are no selectors. 
                    OutputFeatureValuePair(static_cast<const char *>(ptrFeature->GetName()), static_cast<const char *>(valNode->ToString()), fp);                   
                }
                
            } while( selectorSet.SetNext());
            // Reset to original selector/selected value (if any was used)
            selectorSet.Restore();
            
            // Save the original settings for any selector that was handled (looped over) above.
            if (selectorSettingWasOutput)
            {
                GenApi::FeatureList_t selectingFeatures;
                selectorSet.GetSelectorList( selectingFeatures, true);
                for ( GenApi::FeatureList_t ::iterator itSelector = selectingFeatures.begin(); itSelector != selectingFeatures.end(); ++itSelector)
                {
                    GenApi::CNodePtr selectedNode( *itSelector);
                    GenApi::CValuePtr selectedValue( *itSelector);
                    OutputFeatureValuePair(static_cast<const char *>(selectedNode->GetName()), static_cast<const char *>(selectedValue->ToString()), fp);
                } 
            }
        }
    }
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
