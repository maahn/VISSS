# visss

Video In Situ Snowfall Sensor data acquisition software

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

#### 0.2.1
* Using ffmpeg pipe instead of opencv videowriter. Gives better thread control 
  does not require custom patched opencv version any more. 


### 0.3
 * For deployments in 22/23

#### planed changes
 * change from mov to mkv files to handle crashes better
 * assign record_time as early as possible
 * apply camera time request when it is reset in camera


