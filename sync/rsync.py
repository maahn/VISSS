#!/usr/bin/env python
# -*- coding: utf-8 -*-
import datetime
import errno
import glob
import os
import subprocess
import sys


def mkdir_p(path):
    try:
        os.makedirs(path)
        print('Created path %s'%path)
    except OSError as exc: # Python >2.5
        if exc.errno == errno.EEXIST and os.path.isdir(path):
            pass
        else: raise


def run_rsync(fromPath, toPath):
    """Run rsync as an argument list (no shell), so paths containing spaces or
    shell metacharacters are passed through verbatim. Returns True on success."""
    rsync = ["/usr/bin/nice", "-n", "19", "/usr/bin/rsync", "-arv", fromPath, toPath]
    print(" ".join(rsync))
    returncode = subprocess.call(rsync)
    if returncode != 0:
        # Do not abort: one unreadable/missing day must not stop the remaining
        # days and folders from being synced. Report it and keep going.
        print("ERROR: rsync %s -> %s failed with exit code %d"
              % (fromPath, toPath, returncode), file=sys.stderr)
        return False
    return True


if len(sys.argv) != 3:
    sys.exit("use: rsync.py fromRoot toRoot")

fromRoot = sys.argv[1]
toRoot = sys.argv[2]

nDays = 7


folders  = glob.glob('%s/*'%fromRoot)

if len(folders) == 0:
    sys.exit("no folders found in %s" % fromRoot)

failed = 0

for folder in folders:
	print(folder)
	folder = folder.split('/')[-1]
	for dd in range(1,nDays+1):

	    year, month, day = (datetime.date.today() - datetime.timedelta(days=dd)).strftime('%Y-%m-%d').split('-')


	    fromPath = "%s/%s/data/%s/%s/%s/"%(fromRoot, folder,year, month, day)
	    toPath = "%s/%s/data/%s/%s/%s/"%(toRoot,folder, year, month, day)
	    if not os.path.isdir(fromPath):
	        # nothing recorded that day for this instrument
	        continue
	    mkdir_p(toPath)
	    if not run_rsync(fromPath, toPath):
	        failed += 1


	fromPath = "%s/%s/applied_config/"%(fromRoot,folder)
	toPath = "%s/%s/applied_config/"%(toRoot,folder)

	if os.path.isdir(fromPath):
	    mkdir_p(toPath)
	    if not run_rsync(fromPath, toPath):
	        failed += 1

if failed:
    sys.exit("%d rsync call(s) failed" % failed)
