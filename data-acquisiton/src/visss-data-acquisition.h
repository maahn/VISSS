#define TELEDYNEDALSA_CHUNK_SIZE_DEVICEID      0x00000010
#define TELEDYNEDALSA_CHUNK_SIZE_DEVICEUSERID  0x00000010


#include "stdio.h"
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
#include <opencv2/video.hpp>
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
int                     live_window_frame_ratio;


struct MatMeta {
  cv::Mat MatImage;
  unsigned long timestamp;
  unsigned long id;
  bool newFile = FALSE;
  std::string filename;
}; 

std::string get_timestamp(){
    char s[100];

    time_t t = time(NULL);
    struct tm * p = localtime(&t);

    strftime(s, 100, "%y-%m-%d %H:%M:%S", p);

    std::string t_string(s, strlen(s));

    return t_string;
   
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

            if (mkdir(_path, S_IRWXU) != 0) {
                if (errno != EEXIST)
                    return -1; 
            }

            *p = '/';
        }
    }
    std::cout << "STATUS | " << get_timestamp() << "| Created path " << _path <<std::endl;
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
