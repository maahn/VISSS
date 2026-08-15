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
import gzip
import os
import sys
import time
from subprocess import call

###settings
fpath_in = "/data/temp/sonic/"

if len(sys.argv) != 2:
    sys.exit("use: sonic_gzip_files.py outputPath  (e.g. /data/nyaalesund/sonic)")
fpath_out = sys.argv[1]

inFiles = os.listdir(fpath_in)

for inFile in inFiles:
    # Process each file in the input directory: gzip it into the dated output
    # tree and delete the original, but only once nothing has it open anymore.
    if not os.path.isfile(fpath_in + inFile):
        continue
    print(inFile)
    fileopen = call(["lsof", "-t", fpath_in + inFile])
    if fileopen != 0:
        print("gzipping file ", inFile)

        year = inFile[0:4]
        month = inFile[4:6]
        day = inFile[6:8]

        outDir = fpath_out + "/" + year + "/" + month + "/" + day
        os.makedirs(outDir, exist_ok=True)

        # Write to a .tmp name and rename only once the whole file is written,
        # so an interrupted run cannot leave a truncated .gz that looks final -
        # and only delete the source after that rename succeeded.
        outFile = outDir + "/" + inFile + ".gz"
        tmpFile = outFile + ".tmp"
        with open(fpath_in + inFile, "rb") as f_in, gzip.open(tmpFile, "wb") as f_out:
            f_out.writelines(f_in)
        os.rename(tmpFile, outFile)
        os.remove(fpath_in + inFile)
    else:
        print(inFile, " still open")
