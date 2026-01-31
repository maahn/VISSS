#!/usr/bin/python
"""
Script to compress Sonic data files.

This script monitors a temporary directory for Sonic data files and compresses
them using gzip when they are no longer being written to.

Attributes
----------
fpath_in : str
    Input directory path containing raw Sonic data files.
fpath_out : str
    Output directory path for compressed files.
inFiles : list
    List of files in the input directory.
"""

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
    """
    Process each file in the input directory.

    Parameters
    ----------
    inFile : str
        Name of the input file to process.
    """
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
