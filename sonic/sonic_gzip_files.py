#!/usr/bin/python
import datetime
import time
import os
from subprocess import call
import gzip

###settings
fpath_in = "/data/temp/sonic/"
inFiles = os.listdir(fpath_in)
fpath_out = "/data/nyaalesund/sonic"

for inFile in inFiles:

  fileopen = call(['lsof',  '-t', fpath_in+inFile])
  if fileopen != 0:
    print('gzipping file ', inFile)

    yearMonth = inFile[0:6]
    
    try:
      os.makedirs(fpath_out+"/"+yearMonth+"/")
    except OSError:
      pass
    
    f_in = open(fpath_in+inFile, 'rb')  
    f_out = gzip.open(fpath_out + '/' + yearMonth + '/' + inFile + '.gz', 'wb')
    f_out.writelines(f_in)
    f_out.close()
    os.remove(fpath_in+inFile)



