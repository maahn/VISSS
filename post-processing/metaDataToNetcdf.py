import cv2
import sys
import time 
import glob
import os
import socket

import datetime
import pandas as pd
import matplotlib.pyplot as plt

import matplotlib as mpl
import numpy as np
import xarray as xr
from copy import deepcopy

'''
identify interesting time stamps
'''

threshs = np.array([20,30,40,60,80,100,120])
cameras = ['master_S1145792', 'slave_S1143155']
movieExtension = 'mov'
site = 'mosaic'
frame_width = 1280
frame_height = 1024
height_offset = 64
fps = 140
minMovingPix = 20 #need to move at the same time

writeVideo = False

path = '/projekt4/ag_maahn/visss_mosaic/%s/mosaic/visss%i_visss_%s/data'
# path = '/Users/mmaahn/data/VISSS_data/mosaic_keydays/%s/visss%i_visss_%s'
outPath = '/projekt1/ag_maahn/VISSS/mosaic/movingPix/'


def getMetaData(fname, camera, threshs, minMovingPix, stopAfter=-1):

    ### meta data ####
    metaFname = fname.replace('mov', 'txt').replace('mp4', 'txt')

    record_starttime = datetime.datetime.strptime(fname.split('_')[-1].split('.')[0], '%Y%m%d-%H%M%S')

    with   open(metaFname) as f:
        firstLine = f.readline()
        if firstLine.startswith('# VISSS file format version: 0.2'):
            asciiVersion = 0.2
            asciiNames = ['capture_time', 'record_time', 'capture_id', 'mean', 'std']
            gitTag = f.readline().split(':')[1].lstrip().rstrip()
            gitBranch = f.readline().split(':')[1].lstrip().rstrip()
            skip = f.readline()
        elif firstLine.startswith('# VISSS file format version: 0.3'):
            raise NotImplementedError
        else:
            asciiVersion = 0.1
            asciiNames = ['capture_time', 'record_time', 'capture_id']
            gitTag= '-'
            gitBranch = '-'
        capture_starttime = f.readline().split(':')[1].lstrip().rstrip()
        capture_starttime = datetime.datetime.utcfromtimestamp(int(capture_starttime)*1e-6)
        serialnumber = f.readline().split(':')[1].lstrip().rstrip()
        configuration = f.readline().split(':')[1].lstrip().rstrip()
        hostname = f.readline().split(':')[1].lstrip().rstrip()



    metaDat = pd.read_csv(metaFname, comment='#', names=asciiNames)
    metaDat.index = metaDat.index.set_names('record_id')
    diffs = metaDat.capture_id.diff()
    diffs[diffs<0] = 1
    
    assert diffs.min() > 0

    newIndex = np.cumsum(diffs)
    newIndex[0] = 0
    newIndex = newIndex.astype(int)
    metaDat['capture_id'] = newIndex
    metaDat = xr.Dataset(metaDat)

    metaDat['capture_time'] = xr.DataArray([datetime.datetime.utcfromtimestamp(t1*1e-6) for t1 in metaDat['capture_time'].values], coords=metaDat['capture_time'].coords)
    metaDat['record_time'] = xr.DataArray([datetime.datetime.utcfromtimestamp(t1*1e-6) for t1 in metaDat['record_time'].values], coords=metaDat['record_time'].coords)

    metaDat['capture_starttime'] = capture_starttime
    metaDat['serialnumber'] = serialnumber
    metaDat['configuration'] = configuration
    metaDat['hostname'] = hostname
    metaDat['gitTag'] = gitTag
    metaDat['gitBranch'] = gitBranch

    inVid = cv2.VideoCapture(fname)

    ii = -1
    if not inVid.isOpened:
        print('ERROR Unable to open: ' + fname)
        with open('%s_errors.txt'%camera, 'a') as f:
            f.write('ERROR Unable to open: ' + fname)
        return None

    nFrames = int(inVid.get(cv2.CAP_PROP_FRAME_COUNT))
    
    if (nFrames == len(metaDat.record_id) +1) && (nFrames < 41900) # bug in mosaic software version
        print('WARNING number of frames do not match %i %i \n'%(nFrames,len(metaDat.record_id) ))
        with open('%s_warnings.txt'%camera, 'a') as f:
            f.write('WARNING number of frames do not match %i %i %s \n'%(nFrames,len(metaDat.record_id),fname ))

    elif nFrames != len(metaDat.record_id):
        print('ERROR number of frames do not match %i %i \n'%(nFrames,len(metaDat.record_id) ))
        with open('%s_errors.txt'%camera, 'a') as f:
            f.write('ERROR number of frames do not match %i %i %s \n'%(nFrames,len(metaDat.record_id),fname ))
        return None

    oldFrame = np.zeros((frame_height, frame_width), dtype=np.uint8)
    nChangedPixel = np.zeros((len(metaDat.record_id), len(threshs)), dtype=int)



    print(fname)
    while True:

        ii += 1

        

        ret, frame = inVid.read()

        try:
            subFrame = frame[height_offset:,:,0]
        except TypeError: # frame is None at the end of the file
            break

        squarediff = cv2.absdiff(subFrame,oldFrame)

        
        for tt, thresh in enumerate(threshs):
            try:
                nChangedPixel[ii, tt] = ((squarediff >= thresh)).sum()
            except IndexError:
                with open('%s_warnings.txt'%camera, 'a') as f:
                    f.write('WARNING number of frames do not match %i %i  \n'%(ii, tt))
                print('WARNING number of frames do not match %i %i  \n'%(ii, tt))



        #print(metaDat['capture_time'][ii].values, metaDat['record_time'][ii].values,metaDat['capture_id'][ii].values,  nChangedPixel[ii])    

        #     continue
        #if writeVideo and (ii != 0) and (nChangedPixel[ii, -1] >0):
        #    outVid.write(frame)
        #    videoWritten = True
        #if (stopAfter > 0) and (ii> stopAfter): #REMOVE

         #    break

        #if False:
            #print(ii,
                # (squarediff.astype(float).mean(-1) > 1).sum(), 
                # (squarediff.astype(float).mean(-1) > 5).sum(),
                #nChangedPixel[ii],
                #)

            #if (s20>0) and (s30==0):

                #cv2.imshow('Frame', squarediff.astype(np.uint8)   )
                #cv2.imshow('Frame2', frame   )


                #keyboard = cv2.waitKey(50)
                #if keyboard == 'q' or keyboard == 27:
                #    break
                # if ii >10:
                #     asdfgfhgj

        oldFrame = deepcopy(subFrame)


    inVid.release()
    



    metaDat['nMovingPixel'] = xr.DataArray(nChangedPixel, coords=[metaDat.record_id, xr.DataArray(threshs, dims=['nMovingPixelThresh'])])
    metaDat = metaDat.expand_dims({'record_starttime':[record_starttime]})

    if (metaDat.nMovingPixel.squeeze().sel(nMovingPixelThresh=100)>minMovingPix).sum()> 1:
        with open('%s_nMoving100.txt'%camera, 'a') as f:
            f.write('%s %i\n'%(fname, (metaDat.nMovingPixel.squeeze().sel(nMovingPixelThresh=100)>0).sum().values))
    elif (metaDat.nMovingPixel.squeeze().sel(nMovingPixelThresh=20)>minMovingPix).sum()> 1:
        with open('%s_nMoving20.txt'%camera, 'a') as f:
            f.write('%s %s\n'%(fname, (metaDat.nMovingPixel.squeeze().sel(nMovingPixelThresh=20)>0).sum().values))
    else:
        with open('%s_nMoving0.txt'%camera, 'a') as f:
            f.write('%s\n'%(fname))

    return metaDat



for cc, camera in enumerate(cameras):
    
    pathCam = path%(camera.split('_')[0], cc+1, camera)
    print(pathCam)
    
    years = sorted(glob.glob('%s/*'%pathCam))
    years = [year.split('/')[-1] for year in years]
    np.random.shuffle(years)
    
    for year in years:
        months = sorted(glob.glob('%s/%s/*'%(pathCam, year)))
        months = [month.split('/')[-1] for month in months]
        np.random.shuffle(months)


        for month in months:
            days = sorted(glob.glob('%s/%s/%s/*'%(pathCam, year, month)))
            days = [day.split('/')[-1] for day in days]
            np.random.shuffle(days)

            for day in days:
                fnamesPattern = '%s/%s/%s/%s/*.%s'%(pathCam, year, month, day, movieExtension)
                fnames = sorted(glob.glob(fnamesPattern))
                outFName = '%s/%s_%s_%s%s%s.nc'%(outPath,site,camera, year, month, day)
                outVName = '%s/%s_%s_%s%s%s.extracted.mp4'%(outPath,site,camera, year, month, day)
                videoWritten = False

                if os.path.isfile(outFName) or os.path.isfile('%s.inprogress'%outFName) or os.path.isfile('%s.nodata'%outFName):
                    print('%s skipping'%outFName)
                    continue

                if len(fnames) == 0:
                    print('ERROR', 'no files', fnamesPattern)
                    with open('%s_errors.txt'%camera, 'a') as f:
                        f.write('ERROR %s %s \n'%( 'no files', fnamesPattern))
                    continue

                with open('%s.inprogress'%outFName, 'w') as f:
                    f.write('%i %s'%(os.getpid(),socket.gethostname()))

                print('Processing %s'%outFName)

                metaDats = []
                fourcc = cv2.VideoWriter_fourcc('a', 'v', 'c', '1')

                if writeVideo: 
                    outVid = cv2.VideoWriter(outVName,fourcc, fps//10, (frame_width,frame_height+height_offset))
                for fname in fnames:

                    metDat = getMetaData(fname, camera, threshs, minMovingPix)

                    if metDat is not None:
                        metaDats.append(metaDat)
                    # break #REMOVE


                if writeVideo: 
                    outVid.release() 

                    if not videoWritten:
                        os.remove(outVName)
                        with open('%s.noextracted'%outVName, 'w') as f:
                            f.write('noextracted')

                if len(metaDats) >0:
                    metaDats = xr.concat(metaDats, 'record_starttime')


                    comp = dict(zlib=True, complevel=5)
                    encoding = {var: comp for var in metaDats.data_vars}
                    metaDats.to_netcdf(outFName, encoding=encoding)
                    metaDats.close()    
                else:
                    with open('%s.nodata'%outFName, 'w') as f:
                        f.write('nodata')
                #os.remove('%s.inprogress'%outFName)

                