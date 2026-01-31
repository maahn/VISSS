# VISSS Video In Situ Snowfall Sensor
This repository contains the VISSS data acquisition software. Please see also
* VISSS data processing library https://github.com/maahn/VISSSlib
* VISSS2 hardware plans https://zenodo.org/doi/10.5281/zenodo.7640820
* VISSS3 hardware plans https://zenodo.org/doi/10.5281/zenodo.10526898

## Version history

### 0.1-MOSAiC
 * original version for MOSAiC
 * running on (too slow) Intel i7-4770

#### known bugs

* capture_id overflows at 65535 - relatively easy to fix
* capture_time drifts because it is only reset when camera starts
* record_time is assigned in the processing queue, can be a couple of seconds 
  off if queue is long
* flipped capture_time: once in a while timestamps of two consecutive frames are 
  flipped. Not clear whether frame itself is also flipped. So far only 
  observed for follower. Solution: remove flipped frames assuming meta data 
  and frame are flipped.
* ghost frames: sometimes a couple of frames are less than 1/fps apart. 
  looks like an additonal frame is inserted and delta t is reduced accordingly
  for approximate 6 frames to make up for additional frame. Origin of additonal 
  frame is unclear. So far only observed for follower.
* file_starttime: obtained from record_time, so problems with record_time apply 

### 0.2
 * For Hyytiälä and Ny-Ålesund deployments in 21/22
 * New Python interface
 * New configuration files
 * Ability to remote control instrument
 * New log files
 * extended meta data

#### known bugs
* file_starttime: obtained from record_time, so problems with record_time apply 
* record_time is assigned in the processing queue, can be a couple of seconds 
  off if queue is long
* camera time request takes a couple of frames to become active, but is applied
  in data acquisition immediately 
* .mov file sometimes broken if it contains only a single frame

### 0.2.1
* Using ffmpeg pipe instead of opencv videowriter. Gives better thread control 
  does not require custom patched opencv version any more. 


### 0.3
 * For deployments in 22/23 and after
 * changed from mov to mkv files to handle crashes better
 * write last timestamp to ascii file when closing (actually first time stamp of next file...)
 * restart data acqusition when reset clock fails
 * assign record_time as early as possible
 * apply camera time request when it is reset in camera 
 * fix cature_id overflow
 * crate timestamp in filename smarter - from camera reset_time (capture_time)

#### known bugs
* C++ restart after camera failure not reported in status file leading to capture_id reset no beeing handled properly (fixed on 14 April 2023)

### 0.3.2
* For deployments in 24/25 and after
* Store internal camera temperature as status information

### 0.4
* Use PTP time for camera time sync
* For deployments in 25/26 and after
