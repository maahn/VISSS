#!/usr/bin/python
import datetime
import time
import os
import sys
from subprocess import call
import gzip

###settings
fpath_in = "/data/temp/sonic/"
inFiles = os.listdir(fpath_in)
fpath_out = "/data/nyaalesund/sonic"
fpath_out = sys.argv[1]

for inFile in inFiles:
  print(inFile)
  fileopen = call(['lsof',  '-t', fpath_in+inFile])
  if fileopen != 0:
    print('gzipping file ', inFile)

    yearMonth = inFile[0:6]
    year = inFile[0:4]
    month = inFile[4:6]
    day = inFile[6:8]

    try:
      os.makedirs(fpath_out+"/"+year+"/")
    except OSError:
      pass
    try:
      os.makedirs(fpath_out+"/"+year+"/"+month)
    except OSError:
      pass
    try:
      os.makedirs(fpath_out+"/"+year+"/"+month+"/"+day)
    except OSError:
      pass

    f_in = open(fpath_in+inFile, 'rb')  
    f_out = gzip.open(fpath_out + '/' + year + '/' + month + '/' + day + '/' + inFile + '.gz', 'wb')
    f_out.writelines(f_in)
    f_out.close()
    os.remove(fpath_in+inFile)
  else:
    print(inFile, " still open")


