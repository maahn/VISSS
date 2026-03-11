#!/usr/bin/python
"""
Serial data logger for Sonic sensors.

This script reads data from a serial port connected to a Sonic sensor and
writes it to timestamped files in a specified directory.

Attributes
----------
fpath : str
    Directory path where raw data files are stored.
fsuffix : str
    File suffix for data files.
com_port : str
    Serial port device path.
errorFname : str
    Path to error log file.
"""

import datetime
import glob
import gzip
import os
import time
from functools import reduce
from subprocess import PIPE, STDOUT, Popen

###settings

fpath = "/data/temp/sonic/"
fsuffix = "txt"


if len(glob.glob("/dev/ttyUSB0")) == 1:
    com_port = "/dev/ttyUSB0"
else:
    com_port = "/dev/ttyS0"


errorFname = "/data/sonic_errors.txt"


def checksum(st):
    """
    Calculate checksum for a string.

    Parameters
    ----------
    st : str
        Input string to calculate checksum for.

    Returns
    -------
    str
        Hexadecimal checksum string in uppercase.
    """
    return hex(reduce(lambda x, y: x ^ y, map(ord, st)))[2:].upper()


# make sure the clock is already set!
# time.sleep(10)

# we need this wrapper here, otherwise cpu goes 100%
command = ["python3", "/home/visss/VISSS/sonic/comPortSniffer.py", com_port]
p = Popen(command, stdin=PIPE, stdout=PIPE, stderr=STDOUT, encoding="utf-8")
# p = Popen(['cat',com_port],stdin=PIPE, stdout=PIPE, stderr=PIPE)

# save date variable
today = datetime.datetime.utcnow().strftime("%Y%m%d%H")
yearMonth = today[0:6]
Day = today[6:8]

# create/open file
try:
    # os.makedirs(fpath+"/"+yearMonth+"/")
    os.makedirs(fpath)
except OSError:
    pass

# count = 0
# fname = fpath+"/"+yearMonth+Day+"_"+str(count)+"."+fsuffix
hour = today[8:10]
fname = fpath + "/" + yearMonth + Day + "_" + hour + "." + fsuffix

# while os.path.exists(fname):
#   count = count + 1
#   fname = fpath+"/"+yearMonth+Day+"_"+str(count)+"."+fsuffix

outFile = open(fname, "at+")


print("Sonic serial data logger")
print(today, fname)

# just in case we open a file which was already created but recording was interupted:
outFile.write(" \n")

errorFile = open(errorFname, "a", 1)

line = ""

##read data
try:
    while True:
        # item = item[1:-5] + "\r\n"
        # item = "%s\r\n" % (p.stdout.readline()[1:-5],)
        line = p.stdout.readline()

        line = line.replace("\x02", "").replace("\x03", "")

        if p.poll() is not None:
            # print(p.stderr.readline())
            print(line)
            raise SystemError("%s stopped" % " ".join(command))
            break
        # print(line)

        # no short lines
        if len(line) < 5:
            if len(line) > 0:
                string = (
                    datetime.datetime.utcnow().strftime("%Y%m%d%H%M%S%f")
                    + "BAD DATA (Line too short):"
                    + repr(line)
                )
                print(string)
                errorFile.write(string + "\n")
            continue

        # check checksum
        if str(checksum(line[:-3])) != str(line[-3:-1]):
            string = (
                datetime.datetime.utcnow().strftime("%Y%m%d%H%M%S%f")
                + "BAD DATA (wrong checksum):"
                + repr(line)
            )
            print(string)
            errorFile.write(string + "\n")
            continue

        item = "%s;%s" % (datetime.datetime.utcnow().strftime("%Y%m%d%H%M%S%f"), line)

        # if new day, first make new file!
        now = datetime.datetime.utcnow().strftime("%Y%m%d%H")
        if now != today:
            outFile.close()
            today = now
            yearMonth = today[:6]
            Day = today[6:8]
            try:
                os.mkdir(fpath + "/")
            except OSError:
                pass

            hour = today[8:10]
            fname = fpath + "/" + yearMonth + Day + "_" + hour + "." + fsuffix

            outFile = open(fname, "at+")
            print(today, fname)

        # write data
        outFile.write(item)

except KeyboardInterrupt:
    print("stopping...")

finally:
    # p.terminate()
    outFile.close()
    print("file closed")
    errorFile.close()
