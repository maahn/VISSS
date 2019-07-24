#!/usr/bin/env python
# -*- coding: utf-8 -*-
import sys
import os
import datetime
import errno
import glob

def mkdir_p(path):
    try:
        os.makedirs(path)
        print('Created path %s'%path)
    except OSError as exc: # Python >2.5
        if exc.errno == errno.EEXIST and os.path.isdir(path):
            pass
        else: raise

fromRoot = sys.argv[1]
toRoot = sys.argv[2]

nDays = 7


folders  = glob.glob('%s/*'%fromRoot)

assert len(folders) >0

for folder in folders:
	print(folder)
	folder = folder.split('/')[-1]
	for dd in range(1,nDays+1):

	    year, month, day = (datetime.date.today() - datetime.timedelta(days=dd)).strftime('%Y-%m-%d').split('-')


	    fromPath = "%s/%s/data/%s/%s/%s/"%(fromRoot, folder,year, month, day)
	    toPath = "%s/%s/data/%s/%s/%s/"%(toRoot,folder, year, month, day)
	    mkdir_p(toPath)
	    rsync = "/usr/bin/nice -n 19 /usr/bin/rsync -arv %s %s"%(fromPath,toPath)
	    print(rsync)
	    os.system(rsync)


	fromPath = "%s/%s/applied_config/"%(fromRoot,folder)
	toPath = "%s/%s/applied_config/"%(toRoot,folder)

	mkdir_p(toPath)
	rsync = "/usr/bin/nice -n 19 /usr/bin/rsync -arv %s %s"%(fromPath,toPath)
	print(rsync)
	os.system(rsync)