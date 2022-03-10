#! /usr/bin/python3
import serial
import sys

try:
  com_port =sys.argv[1]
except: 
  sys.exit("use: python com.py comPort [sendString]")

try:
  sendString=sys.argv[2]
  sendData = True
except IndexError:
  sendData=False

  
try:
  serialPort = serial.Serial(com_port, 57600, timeout=10800, rtscts=False, dsrdtr=False, xonxoff=True,bytesize=serial.EIGHTBITS,parity=serial.PARITY_NONE)
  #print(serialPort.name)
except: 
  sys.exit("Could not open port %s!"%com_port)

try:
  if sendData:
    print("S: ", sendString)
    serialPort.write(sendString+"\r\n")
    sendData = False
  while True:
    #one dataset is around 20000 bytes
    item = serialPort.read(size=2000)
    sys.stdout.write(item.decode('utf-8')) # Nina: I had to include decode('utf-8') becauuse python error in write 'must be str not bytes'
    sys.stdout.flush()
except:
    serialPort.close()
    
  

