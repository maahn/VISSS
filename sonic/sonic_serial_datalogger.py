#!/usr/bin/python
import datetime
import time
import os
from subprocess import Popen, PIPE
import gzip

###settings
com_port ='/dev/ttyF0'
fpath="/data/nyaalesund/sonic"
fsuffix = "txt.gz"

errorFname="/data/nyaalesund/sonic/sonic_errors.txt"

# to do: update checksum for thies sonic
def checksum(st):
    return hex(reduce(lambda x,y:x^y, map(ord, st)))[2:]

#make sure the clock is already set!
#time.sleep(10)

#we need this wrapper here, otherwise cpu goes 100%
p = Popen(['python3', '/home/visss/VISSS/sonic/comPortSniffer.py',com_port], stdin=PIPE, stdout=PIPE, stderr=PIPE,encoding='utf-8')
#p = Popen(['cat',com_port],stdin=PIPE, stdout=PIPE, stderr=PIPE)

#save date variable
today = datetime.datetime.utcnow().strftime("%Y%m%d")
yearMonth = today[0:6]
Day = today[6:]
print(today)

#create/open file
try:
  os.makedirs(fpath+"/"+yearMonth+"/")
except OSError:
  pass
outFile = gzip.open(fpath+"/"+yearMonth+"/"+yearMonth+Day+"."+fsuffix,"at")

#just in case we open a file which was already created but recording was interupted:
outFile.write(" \n")

errorFile=open(errorFname,"a",1)


##read data
try:         
  while True:
    #item = item[1:-5] + "\r\n"
    #item = "%s\r\n" % (p.stdout.readline()[1:-5],)
    line = p.stdout.readline()

    #print(line)
    
    #no short lines
    if len(line) < 5:
      if len(line)>0: 
        string =datetime.datetime.utcnow().strftime("%Y%m%d%H%M%S ")+"BAD DATA (Line too short):"+ repr(line)
        print(string)           
        errorFile.write(string+"\n")
      continue
    
## to do: include checksum!     
#    #check checksum
#    if checksum(line[:-4]) != line[-4:-2]:
#       string =datetime.datetime.utcnow().strftime("%y%m%d%H%M%S ")+"BAD DATA (wrong checksum):"+ repr(line)
#       print(string)           
#       errorFile.write(string+"\r\n")
#       continue
    
    item = "%s;%s" % (datetime.datetime.utcnow().strftime("%Y%m%d%H%M%S%f"), line)#[1:-5])

    #if new day, first make new file! 
    if datetime.datetime.utcnow().strftime("%Y%m%d") != today:
      outFile.close()
      today = datetime.datetime.utcnow().strftime("%Y%m%d")
      yearMonth = today[:6]
      Day = today[6:]
    try:
      os.mkdir(fpath+"/"+yearMonth+"/")
    except OSError:
      pass
    outFile = gzip.open(fpath+"/"+yearMonth+"/"+yearMonth+Day+"."+fsuffix,"at")
    string ="sonic %s UTC %s" % (datetime.datetime.utcnow().strftime("%y%m%d%H%M%S%f"), item,)
    #print(string), 
    outFile.write(item)

except KeyboardInterrupt:
   print("stopping...")
finally:
   #p.terminate()
   outFile.close()
   print("file closed")
   errorFile.close()    
    





