#!/usr/bin/env python
"""
- read output from a subprocess in a background thread
- show the output in the GUI


todo: https://pypi.org/project/CppPythonSocket/ (named pipe faster!9)
https://github.com/goldsborough/ipc-bench
https://stackoverflow.com/questions/1268252/possible-to-share-in-memory-data-between-2-separate-processes
https://stackoverflow.com/questions/60949451/how-to-send-a-cvmat-to-python-over-shared-memory/60959732#60959732
https://stackoverflow.com/questions/60966568/example-of-ipc-between-two-process-with-opencv-cvmat-object-c-as-server-an

"""
import sys
import os
from itertools import islice
from subprocess import Popen, PIPE, STDOUT
from textwrap import dedent
from threading import Thread
import yaml
from pathlib import Path
from copy import deepcopy
import time

import tkinter as tk # Python 3
from tkinter import ttk


from queue import Queue, Empty # Python 3

def iter_except(function, exception):
    """Works like builtin 2-argument `iter()`, but stops on `exception`."""
    try:
        while True:
            yield function()
    except exception:
        return

def save_settings(event):
    #gather setings
    settings['geometry']  = root.geometry()

    #write settings
    with open(SETTINGSFILE, "w+") as stream:
        yaml.dump(settings, stream, default_flow_style=False, allow_unicode=True)


def read_settings():
    try:
        with open(SETTINGSFILE, 'r') as stream:
            return yaml.safe_load(stream)
    except (FileNotFoundError, yaml.YAMLError) as e:
        print(e)
        return {}

ROOTPATH=os.path.dirname(os.path.abspath(__file__))
print(ROOTPATH)

home = str(Path.home())
SETTINGSFILE = "%s/.visss.yaml"%home
DEFAULTSETTINGS = {
        'geometry' : "%dx%d" % (300,300),
        'environmentFile' : None,
        'camera1File' : None,
        'camera2File' : None,
    }

class DisplaySubprocessOutputDemo:
    def __init__(self, root, mainframe, command, autostart):
        self.root = root
        self.mainframe = mainframe
        self.command = command

        self.status = tk.StringVar()
        self.status.set('idle')

        frame1=ttk.Frame(mainframe)
        frame1.pack(side=tk.LEFT,fill=tk.BOTH, expand=True)


        config = ttk.Frame(frame1)
        config.pack(side=tk.TOP, fill=tk.X)
        self.startStopButton = ttk.Button(config, text="Start/Stop", command=self.clickStartStop)
        self.startStopButton.pack(side = tk.LEFT, anchor=tk.NW)


        self.statusWidget = ttk.Label(config, textvariable=self.status)
        self.statusWidget.pack(side = tk.LEFT, anchor=tk.NW)

        # show subprocess' stdout in GUI
        self.text = tk.Text(frame1, height=4, width = 30)
        self.text.pack(side=tk.LEFT,fill=tk.BOTH, expand=True)
        
        s = ttk.Scrollbar(frame1, orient=tk.VERTICAL, command=self.text.yview)
        s.pack(side=tk.RIGHT,fill=tk.Y)
        self.text['yscrollcommand'] = s.set

        if autostart:
            self.clickStartStop()

    def clickStartStop(self):

        if self.status.get() == 'idle':
            self.start(self.command.split(' '))

        elif self.status.get() == 'running':
            self.quit()
        else:
            pass

    def start(self, command):
        self.status.set('running')
        # start dummy subprocess to generate some output
        self.process = Popen(command, stdout=PIPE, stderr=STDOUT)

        # launch thread to read the subprocess output
        #   (put the subprocess output into the queue in a background thread,
        #    get output from the queue in the GUI thread.
        #    Output chain: process.readline -> queue -> label)
        q = Queue(maxsize=1024)  # limit output buffering (may stall subprocess)
        t = Thread(target=self.reader_thread, args=[q])
        t.daemon = True # close pipe if GUI process exits
        t.start()

        self.update(q) # start update loop


    def reader_thread(self, q):
        """Read subprocess output and put it into the queue."""
        try:
            with self.process.stdout as pipe:
                for line in iter(pipe.readline, b''):
                    q.put(line)
        finally:
            q.put(None)

    def update(self, q):
        """Update GUI with items from the queue."""
        for line in iter_except(q.get_nowait, Empty): # display all content
            if line is None:
                self.quit()
                return
            else:
                # self.label['text'] = line # update GUI
                self.text.insert(tk.END, line)
                self.text.see("end")
                # break # display no more than one line per 40 milliseconds
        self.mainframe.after(40, self.update, q) # schedule next update

    def quit(self):
        try:
            self.process.terminate()
        except AttributeError:
            pass
        self.status.set('idle')

    def kill(self):
        try:
            self.process.kill() # exit subprocess if GUI is closed (zombie!)
        except AttributeError:
            pass
        self.root.destroy()

settings = deepcopy(DEFAULTSETTINGS)
settings.update(read_settings())

#reset geometery if broken
if settings['geometry'].startswith('1x1'):
    settings['geometry'] = DEFAULTSETTINGS['geometry'] 


root = tk.Tk()

root.title("VISSS data acquisition")
root.bind("<Configure>",save_settings)
root.geometry(settings['geometry'])

# mainframe = tttk.Frame(root, padding="3 3 12 12")
# mainframe.grid(column=0, row=0, sticky=(tk.N, tk.W, tk.E, tk.S))


mainframe = ttk.Frame(root)
mainframe.pack(fill=tk.BOTH, expand=1)

root.columnconfigure(0, weight=1)
root.rowconfigure(0, weight=1)

config = ttk.Frame(mainframe)
config.pack(side=tk.TOP, fill=tk.X)
button = ttk.Button(config, text="Dummy")
button.pack(side = tk.LEFT, anchor=tk.NW)
status = ttk.Label(config, text="Text")
status.pack(side = tk.LEFT)



#load settings
IP='192.168.100.2'
MAC='00:01:0D:C3:0F:34'
INTERFACE='enp35s0f0'
MAXMTU='9216'
SITE='LIM'

PRESET='ultrafast'
QUALITY=17

CAMERACONFIG='visss_trigger.config'

OUTDIR='/data/lim'



camera1Command = (f'{ROOTPATH}/launch_visss_data_acquisition.sh '
    f'--IP={IP} --MAC={MAC} --INTERFACE={INTERFACE} --MAXMTU={MAXMTU} ' 
    f'--PRESET={PRESET} --QUALITY={QUALITY} --CAMERACONFIG={CAMERACONFIG} '
    f'--ROOTPATH={ROOTPATH} --OUTDIR={OUTDIR} --SITE={SITE}  ')


IP='192.168.200.2'
MAC='00:01:0D:C3:04:9F'
INTERFACE='enp35s0f1'
CAMERACONFIG='visss_follower.config'

camera2Command = (f'{ROOTPATH}/launch_visss_data_acquisition.sh '
    f'--IP={IP} --MAC={MAC} --INTERFACE={INTERFACE} --MAXMTU={MAXMTU} ' 
    f'--PRESET={PRESET} --QUALITY={QUALITY} --CAMERACONFIG={CAMERACONFIG} '
    f'--ROOTPATH={ROOTPATH} --OUTDIR={OUTDIR} --SITE={SITE}  ')


print(camera1Command)
print(camera2Command)

app1 = DisplaySubprocessOutputDemo(root, mainframe, camera1Command, True)
app2 = DisplaySubprocessOutputDemo(root, mainframe, camera2Command, True)

root.protocol("WM_DELETE_WINDOW", app1.kill)
root.protocol("WM_DELETE_WINDOW", app2.kill)

root.mainloop() 

#####


# from tkinter import *
# import subprocess as sub
# p = sub.Popen('ls -l'.split(' '),stdout=sub.PIPE,stderr=sub.PIPE)
# output, errors = p.communicate()

# root = Tk()
# text = Text(root)
# text.pack()
# text.insert(END, output)
# root.mainloop()


#####

# from tkinter import *
# from tkinter import ttk


# import yaml
# from pathlib import Path

# home = str(Path.home())
# SETTINGSFILE = "%s/.visss.yaml"%home
# DEFAULTSETTINGS = {
#         'geometry' : "%dx%d" % (300,300),
#         'environmentFile' : None,
#         'camera1File' : None,
#         'camera2File' : None,
#     }

# def save_settings(event):
#     #gather setings
#     settings['geometry']  = root.geometry()

#     #write settings
#     with open(SETTINGSFILE, "w+") as stream:
#         yaml.dump(settings, stream, default_flow_style=False, allow_unicode=True)


# def read_settings():
#     try:
#         with open(SETTINGSFILE, 'r') as stream:
#             return yaml.safe_load(stream)
#     except (FileNotFoundError, yaml.YAMLError) as e:
#         print(e)
#         return {}


# def calculate(*args):
#     try:
#         value = float(feet.get())
#         meters.set(int(0.3048 * value * 10000.0 + 0.5)/10000.0)
#     except ValueError:
#         pass

# settings = DEFAULTSETTINGS
# settings.update(read_settings())

# root = Tk()
# root.title("VISSS data acquisition")
# root.bind("<Configure>",save_settings)
# root.geometry(settings['geometry'])

# mainframe = tttk.Frame(root, padding="3 3 12 12")
# mainframe.grid(column=0, row=0, sticky=(N, W, E, S))
# root.columnconfigure(0, weight=1)
# root.rowconfigure(0, weight=1)

# feet = StringVar()
# feet_entry = ttk.Entry(mainframe, width=7, textvariable=feet)
# feet_entry.grid(column=2, row=1, sticky=(W, E))

# meters = StringVar()
# ttk.Label(mainframe, textvariable=meters).grid(column=2, row=2, sticky=(W, E))

# ttk.Button(mainframe, text="Calculate", command=calculate).grid(column=3, row=3, sticky=W)

# ttk.Label(mainframe, text="feet").grid(column=3, row=1, sticky=W)
# ttk.Label(mainframe, text="is equivalent to").grid(column=1, row=2, sticky=E)
# ttk.Label(mainframe, text="meters").grid(column=3, row=2, sticky=W)

# for child in mainframe.winfo_children(): 
#     child.grid_configure(padx=5, pady=5)

# feet_entry.focus()
# root.bind("<Return>", calculate)

# root.mainloop()



# # mainloop() 