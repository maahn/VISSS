#!/usr/bin/env python3
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
import signal
from itertools import islice
from subprocess import Popen, PIPE, STDOUT
from textwrap import dedent
from threading import Thread
import yaml
from pathlib import Path
from copy import deepcopy
import time
import random
import string

import tkinter as tk # Python 3
from tkinter import ttk, filedialog, messagebox


from queue import Queue, Empty # Python 3

#make sure on stays on and off stays off
from yaml.constructor import SafeConstructor
def add_bool(self, node):
    return self.construct_scalar(node)
SafeConstructor.add_constructor(u'tag:yaml.org,2002:bool', add_bool)


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


def read_settings(fname):
    if fname is None:
        return {}

    try:
        with open(fname, 'r') as stream:
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
        'configFile' : None,
    }


class DisplaySubprocessOutputDemo:
    def __init__(self, root, mainframe, cameraConfig, configuration):
        self.root = root
        self.mainframe = mainframe
        self.cameraConfig = cameraConfig
        self.configuration = configuration

        self.running = tk.StringVar()
        self.running.set('idle: %s'%cameraConfig['name'])

        self.status = tk.StringVar()
        self.status.set('-')

        self.configFName = '/tmp/visss_%s.config'%(''.join(random.choice(string.ascii_lowercase) for i in range(16)))

        self.command = (f"/usr/bin/env bash {ROOTPATH}/launch_visss_data_acquisition.sh "
            f"--IP={cameraConfig['ip']} --MAC={cameraConfig['mac']} --INTERFACE={cameraConfig['interface']} --MAXMTU={configuration['maxmtu']} "
            f"--PRESET={configuration['preset']} --QUALITY={configuration['quality']} --CAMERACONFIG={self.configFName} "
            f"--ROOTPATH={ROOTPATH} --OUTDIR={configuration['outdir']} --SITE={configuration['site']} --NAME={cameraConfig['name']} " 
            f"--FPS={configuration['fps']} --NTHREADS={configuration['storagethreads']} --STOREALLFRAMES={configuration['storeallframes']}" 

            )

        print(self.command)

        frame1=ttk.Frame(mainframe)
        frame1.pack(side=tk.LEFT,fill=tk.BOTH, expand=True)

        self.statusWidget = ttk.Label(frame1, textvariable=self.status)
        self.statusWidget.pack(side=tk.BOTTOM,fill=tk.X,pady=6, padx=(6,6))

        config = ttk.Frame(frame1)
        config.pack(side=tk.TOP, fill=tk.X,pady=6, padx=(6,6))
        self.startStopButton = ttk.Button(config, text="Start/Stop", command=self.clickStartStop)
        self.startStopButton.pack(side = tk.LEFT, anchor=tk.NW)


        self.runningWidget = ttk.Label(config, textvariable=self.running)
        self.runningWidget.pack(side = tk.LEFT, anchor=tk.NW,fill=tk.Y, expand=True)

        # show subprocess' stdout in GUI
        self.text = tk.Text(frame1, height=4, width = 30)
        self.text.pack(side=tk.LEFT,fill=tk.BOTH, expand=True, pady=6, padx=(6,0))
        
        s = ttk.Scrollbar(frame1, orient=tk.VERTICAL, command=self.text.yview)
        s.pack(side=tk.RIGHT,fill=tk.Y, pady=6, padx=(0,6))
        self.text['yscrollcommand'] = s.set

        

        self.carReturn = False
        if configuration['autostart'] in ['true', 'True', 1]:
            self.clickStartStop()


    def witeParamFile(self):
        
        file = open(self.configFName, 'w')
        for k, v in self.cameraConfig['teledyneparameters'].items():
            if k == 'IO':
                for ii in range(len(self.cameraConfig['teledyneparameters'][k])):
                    for k1, v1 in self.cameraConfig['teledyneparameters'][k][ii].items():
                        print("%s %s"%(k1,v1))
                        file.write("%s %s\n"%(k1,v1))
            else:
                print("%s %s"%(k,v))
                file.write("%s %s\n"%(k,v))
        return 

    def clickStartStop(self):

        if self.running.get().startswith('idle'):
            self.start(self.command.split(' '))

        elif self.running.get().startswith('running'):
            self.quit()
        else:
            pass

    def start(self, command):
        self.running.set('running: %s'%cameraConfig['name'])

        self.witeParamFile()


        # start dummy subprocess to generate some output
        self.process = Popen(command, stdout=PIPE, stderr=STDOUT, preexec_fn=os.setsid)

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
                # if self.carReturn:
                #     self.text.delete("insert linestart", "insert lineend")

                # self.label['text'] = line # update GUI
                # if line.endswith(b'\r'):
                line =  line.replace(b'\r',b'\n')
                #     self.carReturn = True
                # else:
                #     self.carReturn = False

                if line.startswith(b'STATUS'):
                    self.status.set(line[:-2])
                    self.statusWidget.config(background="green")
                else:
                    if line.startswith(b'ERROR') or line.startswith(b'FATAL'):
                        self.status.set(line[:-2])
                        self.statusWidget.config(background="red")
                    self.text.insert(tk.END, line)
                    self.text.see("end")
                # break # display no more than one line per 40 milliseconds
        self.mainframe.after(40, self.update, q) # schedule next update

    def quit(self):
        try:
            # self.process.terminate()
            os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
            print('quit')
        except AttributeError:
            print('tried quit')
            pass
        self.running.set('idle: %s'%cameraConfig['name'])
        self.status.set('NOT RUNNING (YET)')
        self.statusWidget.config(background="yellow")
        try:
            os.remove(self.configFName)
        except FileNotFoundError:
            pass

    def kill(self):
        try:
            #self.process.kill() # exit subprocess if GUI is closed (zombie!)
            os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
            print('kill')
        except AttributeError:
            print('tried to kill')
            pass
        self.running.set('idle: %s'%cameraConfig['name'])
        self.status.set('NOT RUNNING (YET)')
        self.statusWidget.config(background="yellow")

        try:
            os.remove(self.configFName)
        except FileNotFoundError:
            pass

settings = deepcopy(DEFAULTSETTINGS)
settings.update(read_settings(SETTINGSFILE))

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
button = ttk.Button(config, text="Configuration File", command=lambda:askopenfile())

def askopenfile():
    file = filedialog.askopenfilename(filetypes=[("YAML files", ".yaml")])
    if file is not None: 
        settings['configFile'] = file
        configuration = read_settings(file)
        save_settings(None)
        messagebox.showwarning(title=None, message='Restart to apply settings')
    else:
        messagebox.showerror(title=None, message='File not found')

button.pack(side = tk.LEFT, anchor=tk.NW, pady=6, padx=(6,0))
status = ttk.Label(config, text=settings['configFile'])
status.pack(side = tk.LEFT, pady=6, padx=(6,0))

configuration = read_settings(settings['configFile'])
print(111, configuration['storeallframes'])
if configuration['storeallframes'] in ['true', 'True', 1]:
    configuration['storeallframes'] = 1
elif configuration['storeallframes'] in ['false', 'False', 0]:
    configuration['storeallframes'] = 0
else:
    sys.exit('storeallframes: %s'%configuration['storeallframes'])




# configuration = {
#     'maxmtu':'9216',
#     'site':'LIM',
#     'preset':'ultrafast',
#     'quality':17,
#     'outdir':'/data/lim',
#     'autostart':True,
#     'camera' : [
#         {
#         'ip':'192.168.100.2',
#         'mac':'00:01:0D:C3:0F:34',
#         'interface':'enp35s0f0',
#         'name':'visss_trigger.config',
#         }, 
#         {
#         'ip':'192.168.200.2',
#         'mac':'00:01:0D:C3:04:9F',
#         'interface':'enp35s0f1',
#         'name':'visss_follower.config',
#         },
#     ],
# }



#load settings




apps = []
if 'camera' in configuration.keys():
    for cameraConfig in configuration['camera']:
        apps.append(DisplaySubprocessOutputDemo(root, mainframe, cameraConfig, configuration))


# apps.append(DisplaySubprocessOutputDemo(root, mainframe, camera2Command, autostart))

def killall():
    for app in apps:
        app.quit()
    root.destroy()

root.protocol("WM_DELETE_WINDOW", killall)

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