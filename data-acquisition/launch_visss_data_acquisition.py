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
import collections
import json
import logging
import logging.handlers
import operator
import os
import queue
import signal
import string
import sys
import time
import tkinter as tk
import urllib.request
from copy import deepcopy
from itertools import islice
from pathlib import Path
from queue import Empty, Queue
from socket import timeout, gethostname
from subprocess import PIPE, STDOUT, Popen
from textwrap import dedent
from threading import Thread
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from urllib.error import HTTPError, URLError

import numpy as np
import yaml
from yaml.constructor import SafeConstructor

import argparse
import logging


ROOTPATH = os.path.dirname(os.path.abspath(__file__))
SETTINGSFILE = "%s/.visss.yaml" % str(Path.home())
DEFAULTSETTINGS = {
    'geometry': "%dx%d" % (300, 300),
    'configFile': None,
    'autopilot': False,
}
LOGFORMAT = '%(asctime)s: %(levelname)s: %(name)s:%(message)s'
TRIGGERINTERVALLFACTOR = 2  # data can be factor 2 older than interval

def add_bool(self, node):
    return self.construct_scalar(node)


def iter_except(function, exception):
    """Works like builtin 2-argument `iter()`, but stops on `exception`."""
    try:
        while True:
            yield function()
    except exception:
        return


def getSerialNumbers():
    loggerRoot.debug('getting serial numbers')

    serialNumbers = {}
    p = Popen('lsgev -v', shell=True, stdout=PIPE, stderr=STDOUT)
    for line in p.stdout.readlines():
        line = line.decode()
        ip = line.split(']')[1].split('[')[1]
        serial = line.split(']')[-2].split(':')[-1]
        serialNumbers[ip] = serial
    retval = p.wait()
    loggerRoot.info('got serial numbers: %s' % serialNumbers)
    return serialNumbers

def click_autopilot():
    save_settings(None)
    loggerRoot.info('Autopilot set to %s' % bool(autopilot.get()))


def save_settings(event):
    # it is called to often event hough window size is not changing
    if event is not None:
        if settings['geometry'] == root.geometry():
            return
    # gather setings
    settings['geometry'] = root.geometry()
    settings['autopilot'] = bool(autopilot.get())

    # write settings
    with open(SETTINGSFILE, "w+") as stream:
        yaml.dump(settings, stream, default_flow_style=False,
                  allow_unicode=True)
    loggerRoot.debug('save_settings: %s' % settings)
    return


def read_settings(fname):
    loggerRoot.info('read_settings: %s' % fname)
    if fname is None:
        return {}

    try:
        with open(fname, 'r') as stream:
            settings = yaml.safe_load(stream)
    except (FileNotFoundError, yaml.YAMLError) as e:
        loggerRoot.error(e, exc_info=True)
        settings = {}
    else:
        for k in ['storeallframes', 'autopilot']:
            if k in settings.keys():
                if settings[k] in ['true', 'True', 1]:
                    settings[k] = True
                elif settings[k] in ['false', 'False', 0]:
                    settings[k] = False
                else:
                    raise ValueError('%s: %s' % (k, settings[k]))

    loggerRoot.info('read_settings: %s' % settings)
    return settings


def killall():
    loggerRoot.info('Closing GUI')
    for app in apps:
        app.quit()
    root.destroy()


def askopenfile():
    file = filedialog.askopenfilename(filetypes=[("YAML files", ".yaml")])
    if file is not None:
        settings['configFile'] = file
        configuration = read_settings(file)
        save_settings(None)
        messagebox.showwarning(title=None, message='Restart to apply settings')
    else:
        messagebox.showerror(title=None, message='File not found')


def queryExternalTrigger(
    nn,
    triggerWidget,
    root,
    name,
    address,
    interval,
    threshold,
    minMax,
    stopOnTimeout,
    nBuffer,
):

    global externalTriggerStatus

    root.after(interval*1000, lambda: queryExternalTrigger(
        nn,
        triggerWidget,
        root,
        name,
        address,
        interval,
        threshold,
        minMax,
        stopOnTimeout,
        nBuffer,
    ))  # schedule next update

    if not bool(autopilot.get()):
        triggerWidget.config(background="gray")
        trigger.set('external trigger disabled')
        return

    # default values
    data = {
        'unit': '',
        'measurement': np.nan,
        'timestamp': np.datetime64('now'),
    }

    if minMax == 'min':
        oper = operator.ge
    elif minMax == 'min':
        oper = operator.le
    else:
        raise ValueError('minMax must be min or max')

    if stopOnTimeout in ['true', 'True', 1]:
        stopOnTimeout = True
    elif stopOnTimeout in ['false', 'False', 0]:
        stopOnTimeout = False
    else:
        raise ValueError('stopOnTimeout: %s' % stopOnTimeout)

    now = np.datetime64('now')
    try:
        response = urllib.request.urlopen(address, timeout=10).read(
        ).decode('utf-8')
    except (HTTPError, URLError) as error:
        loggerRoot.error(
            'queryExternalTrigger: Data not retrieved because %s URL: %s', error, address)
        if stopOnTimeout:
            continueMeasurement = False
        else:
            continueMeasurement = True
        measurement = '-'
        unit = ''
    else:
        data = json.loads(response)[name]
        loggerRoot.info('queryExternalTrigger: response %s' % str(data))

        timeCond = (now - np.datetime64(data['timestamp']) <
                    np.timedelta64(TRIGGERINTERVALLFACTOR * int(interval), 's'))
        if timeCond:
            continueMeasurement = oper(float(data['measurement']), threshold)
        else:
            loggerRoot.error('queryExternalTrigger: Data too old %s' %
                             (np.datetime64(data['timestamp'])))
            if stopOnTimeout:
                continueMeasurement = False
            else:
                continueMeasurement = True

    loggerRoot.info('queryExternalTrigger: continue Measurement %r' % continueMeasurement)

    measurement = '%g' % data['measurement']
    unit = data['unit']

    externalTriggerStatus[nn].append(continueMeasurement)

    if np.any(externalTriggerStatus[nn]):
        triggerWidget.config(background="green")
    else:
        triggerWidget.config(background="red")
    trigger.set('%s: %s %s %i/%i at %s' %
                (name, measurement, unit, np.sum(externalTriggerStatus[nn]),
                    nBuffer, data['timestamp']))
    return


class QueueHandler(logging.Handler):
    """Class to send logging records to a queue

    It can be used from different threads
    The ConsoleUi class polls this queue to display records in a ScrolledText widget
    https://github.com/beenje/tkinter-logging-text-widget
    """

    def __init__(self, log_queue):
        super().__init__()
        self.log_queue = log_queue

    def emit(self, record):
        self.log_queue.put(record)


class runCpp:
    def __init__(self, mainframe, cameraConfig, configuration):

        self.mainframe = mainframe
        self.cameraConfig = cameraConfig
        self.configuration = configuration
        self.name = cameraConfig['name']
        self.logger = logging.getLogger('Python:runCpp:%s' % self.name)
        self.loggerCpp = logging.getLogger('C++:%s' % self.name)

        logDir =  f"{configuration['outdir']}/{hostname}_{cameraConfig['name']}_{cameraConfig['serialnumber']}/logs"
        try:
            os.mkdir(logDir)
        except  FileExistsError:
            pass

        # Create a logging handler using a queue
        self.log_queue = queue.Queue()
        self.queue_handler = QueueHandler(self.log_queue)
        formatter = logging.Formatter(LOGFORMAT)
        self.queue_handler.setFormatter(formatter)
        self.log_handler = logging.handlers.TimedRotatingFileHandler('%s/log_%s_%s'%(logDir, self.name, self.cameraConfig['serialnumber']), when='D', interval=1, backupCount=0)
        self.log_handler.setFormatter(formatter)

        self.logger.addHandler(self.queue_handler)
        self.loggerCpp.addHandler(self.queue_handler)
        self.logger.addHandler(self.log_handler)
        self.loggerCpp.addHandler(self.log_handler)

        self.running = tk.StringVar()
        self.running.set('Idle: %s' % self.name)

        self.status = tk.StringVar()
        self.status.set('-')

        self.configFName = '/tmp/%s.config' % self.name 

        self.command = (f"/usr/bin/env bash {ROOTPATH}/launch_visss_data_acquisition.sh "
                        f"--IP={cameraConfig['ip']} --MAC={cameraConfig['mac']} --INTERFACE={cameraConfig['interface']} --MAXMTU={configuration['maxmtu']} "
                        f"--PRESET={configuration['preset']} --QUALITY={configuration['quality']} --CAMERACONFIG={self.configFName} "
                        f"--ROOTPATH={ROOTPATH} --OUTDIR={configuration['outdir']} --SITE={configuration['site']} --NAME={self.name} "
                        f"--FPS={configuration['fps']} --NTHREADS={configuration['storagethreads']} --STOREALLFRAMES={int(configuration['storeallframes'])}"
                        )

        frame1 = ttk.Frame(mainframe)
        frame1.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.statusWidget = ttk.Label(frame1, textvariable=self.status)
        self.statusWidget.pack(side=tk.BOTTOM, fill=tk.X, pady=6, padx=(6, 6))

        config = ttk.Frame(frame1)
        config.pack(side=tk.TOP, fill=tk.X, pady=6, padx=(6, 6))
        self.startStopButton = ttk.Button(
            config, text="Start/Stop", command=self.clickStartStop)
        self.startStopButton.pack(side=tk.LEFT, anchor=tk.NW)

        self.runningWidget = ttk.Label(config, textvariable=self.running)
        self.runningWidget.pack(
            side=tk.LEFT, anchor=tk.NW, fill=tk.Y, expand=True)


        # show subprocess' stdout in GUI
        self.scrolled_text = tk.Text(frame1, height=4, width=30)
        self.scrolled_text.pack(side=tk.LEFT, fill=tk.BOTH,
                       expand=True, pady=6, padx=(6, 0))

        s = ttk.Scrollbar(frame1, orient=tk.VERTICAL, command=self.scrolled_text.yview)
        s.pack(side=tk.RIGHT, fill=tk.Y, pady=6, padx=(0, 6))
        self.scrolled_text['yscrollcommand'] = s.set
        self.scrolled_text.bind("<Key>", lambda e: "break")


        # Create a ScrolledText wdiget
        # F_font =  ('bold', 30)
        # self.scrolled_text = ScrolledText(frame1, state='disabled', height=12, font=F_font)
        # self.scrolled_text.pack(side=tk.LEFT, fill=tk.BOTH,
        #                expand=True, pady=6, padx=(6, 0))
        self.scrolled_text.configure(font=('TkFixedFont', 8))
        self.scrolled_text.tag_config('INFO', foreground='black')
        self.scrolled_text.tag_config('DEBUG', foreground='gray')
        self.scrolled_text.tag_config('WARNING', foreground='orange')
        self.scrolled_text.tag_config('ERROR', foreground='red')
        self.scrolled_text.tag_config('CRITICAL', foreground='red', underline=1)
        # Start polling messages from the queue
        self.mainframe.after(100, self.poll_log_queue)


        if settings['autopilot'] in [True, 'true', 'True', 1]:
            self.clickStartStop(autopilot=True)
        self.statusWatcher()

    def witeParamFile(self):

        file = open(self.configFName, 'w')
        self.logger.debug("witeParamFile: opening %s" % self.configFName)
        for k, v in self.cameraConfig['teledyneparameters'].items():
            if k == 'IO':
                for ii in range(len(self.cameraConfig['teledyneparameters'][k])):
                    for k1, v1 in self.cameraConfig['teledyneparameters'][k][ii].items():
                        self.logger.debug(
                            "witeParamFile: writing: %s %s" % (k1, v1))
                        file.write("%s %s\n" % (k1, v1))
            else:
                self.logger.debug("witeParamFile: writing %s %s" % (k, v))
                file.write("%s %s\n" % (k, v))
        return

    def clickStartStop(self, autopilot=False):
        if self.running.get().startswith('Idle'):
            if autopilot:
                self.logger.info('Autopilot starts camera')
            else:
                self.logger.info('User starts camera')
            self.start(self.command.split(' '))
        elif self.running.get().startswith('Running'):
            self.logger.info('User stops camera')
            self.quit()
        else:
            pass

    def statusWatcher(self):

        if autopilot.get():

            if np.any(externalTriggerStatus):
                if self.running.get().startswith('Idle'):
                    self.logger.info('External trigger starts camera')
                    self.start(self.command.split(' '))
                    # line = 'EXTERNAL TRIGGER START: %s \n' % list(
                        # map(list, externalTriggerStatus))
                    # self.text.insert(tk.END, line)
                else:
                    pass
            elif (~np.any(externalTriggerStatus)):
                if self.running.get().startswith('Running'):
                    self.logger.info('External trigger stops camera')
                    self.quit()
                    # line = 'EXTERNAL TRIGGER STOP: %s \n' % list(
                        # map(list, externalTriggerStatus))
                    # self.text.insert(tk.END, line)
                    # self.text.see("end")
                else:
                    pass
            else:
                raise ValueError('do not understand %s' %
                                 list(map(list, externalTriggerStatus)))
            self.startStopButton.state(["disabled"])
        else:
            self.startStopButton.state(["!disabled"])

        self.mainframe.after(100, self.statusWatcher)  # schedule next update

    def start(self, command):
        self.running.set('Running: %s' % self.name)
        self.logger.info('Start camera with %s' % ' '.join(command))

        self.witeParamFile()

        # start dummy subprocess to generate some output
        self.process = Popen(command, stdout=PIPE,
                             stderr=STDOUT, preexec_fn=os.setsid)

        # launch thread to read the subprocess output
        #   (put the subprocess output into the queue in a background thread,
        #    get output from the queue in the GUI thread.
        #    Output chain: process.readline -> queue -> label)
        # limit output buffering (may stall subprocess)
        q = Queue(maxsize=1024)
        t = Thread(target=self.reader_thread, args=[q])
        t.daemon = True  # close pipe if GUI process exits
        t.start()

        self.update(q)  # start update loop

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

        for line in iter_except(q.get_nowait, Empty):  # display all content
            if line is None:
                self.quit()
                return
            else:
                # if self.carReturn:
                #     self.text.delete("insert linestart", "insert lineend")

                # self.label['text'] = line # update GUI
                # if line.endswith(b'\r'):
                line = line.replace(b'\r', b'\n')
                #     self.carReturn = True
                # else:
                #     self.carReturn = False

                if line.startswith(b'STATUS'):
                    self.status.set(line.decode().rstrip())
                    self.statusWidget.config(background="green")
                else:
                    if line.startswith(b'ERROR') or line.startswith(b'FATAL'):
                        self.status.set(line.decode().rstrip())
                        self.statusWidget.config(background="red")
                        thisLogger = self.loggerCpp.error
                        # self.text.insert(tk.END, line)
                        # self.text.see("end")
                    elif (line.startswith(b'DEBUG') or line.startswith(b'OPENCV')):
                        thisLogger = self.loggerCpp.debug
                        # if logging.root.level <= logging.DEBUG:
                            # self.text.insert(tk.END, line)
                            # self.text.see("end")
                    else:
                        thisLogger = self.loggerCpp.info
                        # self.text.insert(tk.END, line)
                        # self.text.see("end")
                    line4Logger = line.decode().rstrip()
                    if not (line4Logger.startswith('***') or (line4Logger == "")):
                        threadN = line4Logger.split('|')[0]
                        if line4Logger.startswith('BASH'):
                            pass
                        elif len(threadN.split('-')) > 1:
                            try:
                                threadN = int(threadN.split('-')[1])
                                line4Logger = 'StorageThread%i: %s' % (
                                    threadN, line4Logger.split('|')[-1])
                            except ValueError:
                                line4Logger = line4Logger.split('|')[-1]
                        else:
                            line4Logger = line4Logger.split('|')[-1]
                        thisLogger(line4Logger)
                        # print(line4Logger)
                # break # display no more than one line per 40 milliseconds

        self.mainframe.after(100, self.update, q)  # schedule next update

    def display(self, record):


        msg = self.queue_handler.format(record)

        # cut very long text
        text = self.scrolled_text.get("1.0", tk.END)
        if len(text) > 500000:
            self.scrolled_text.delete("1.0", tk.END)
            self.scrolled_text.insert(tk.END, text[-50000:])
            self.scrolled_text.see("end")


        self.scrolled_text.insert(tk.END, msg + '\n', record.levelname)
        # Autoscroll to the bottom
        self.scrolled_text.yview(tk.END)

    def poll_log_queue(self):
        # Check every 100ms if there is a new message in the queue to display
        while True:
            try:
                record = self.log_queue.get(block=False)
            except queue.Empty:
                break
            else:
                self.display(record)
        self.mainframe.after(100, self.poll_log_queue)

    def quit(self):
        try:
            # self.process.terminate()
            os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
            self.logger.debug('quitting')
        except AttributeError:
            self.logger.error('tried quitting')
            pass
        self.running.set('Idle: %s' % self.name)
        self.status.set('NOT RUNNING (YET)')
        self.statusWidget.config(background="yellow")

    def kill(self):
        try:
            # self.process.kill() # exit subprocess if GUI is closed (zombie!)
            os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
            self.logger.debug('killing')
        except AttributeError:
            self.logger.error('tried killing')
            pass
        self.running.set('Idle: %s' % self.name)
        self.status.set('NOT RUNNING (YET)')
        self.statusWidget.config(background="yellow")

SafeConstructor.add_constructor(u'tag:yaml.org,2002:bool', add_bool)

parser = argparse.ArgumentParser()
parser.add_argument('-log',
                    '--loglevel',
                    default='info',
                    help='Provide logging level. Example --loglevel debug, default=info')

args = parser.parse_args()


logging.basicConfig(level=args.loglevel.upper(),
                    format=LOGFORMAT)
loggerRoot = logging.getLogger('Python')
loggerRoot.info('Launching GUI')
loggerRoot.debug('Rootpath %s' % ROOTPATH)

hostname = gethostname()
serialNumbers = getSerialNumbers()



settings = deepcopy(DEFAULTSETTINGS)
settings.update(read_settings(SETTINGSFILE))

# reset geometery if broken
if settings['geometry'].startswith('1x1'):
    settings['geometry'] = DEFAULTSETTINGS['geometry']


root = tk.Tk()

root.title("VISSS data acquisition")
root.bind("<Configure>", save_settings)
root.geometry(settings['geometry'])

# mainframe = tttk.Frame(root, padding="3 3 12 12")
# mainframe.grid(column=0, row=0, sticky=(tk.N, tk.W, tk.E, tk.S))


mainframe = ttk.Frame(root)
mainframe.pack(fill=tk.BOTH, expand=1)

root.columnconfigure(0, weight=1)
root.rowconfigure(0, weight=1)

config = ttk.Frame(mainframe)
config.pack(side=tk.TOP, fill=tk.X)
button = ttk.Button(config, text="Configuration File",
                    command=lambda: askopenfile())

button.pack(side=tk.LEFT, anchor=tk.NW, pady=6, padx=(6, 0))
try:
    statusStr = settings['configFile'].split('/')[-1]
except AttributeError:
    statusStr = 'None'
status = ttk.Label(config, text=statusStr)
status.pack(side=tk.LEFT, pady=6, padx=(6, 0))

configuration = read_settings(settings['configFile'])


autopilot = tk.IntVar()
ChkBttn = ttk.Checkbutton(
    config,
    text='Autopilot',
    command=click_autopilot,
    variable=autopilot)
ChkBttn.pack(side=tk.LEFT, pady=6, padx=6)
if settings['autopilot']:
    ChkBttn.invoke()


if (('externalTrigger' in configuration.keys()) and
        (configuration['externalTrigger'] is not None)):
    externalTriggerStatus = []
    for ee, externalTrigger in enumerate(configuration['externalTrigger']):

        externalTriggerStatus.append(
            collections.deque(maxlen=externalTrigger['nBuffer']))

        trigger = tk.StringVar()
        trigger.set('%s: -' % externalTrigger['name'])

        triggerWidget = ttk.Label(config, textvariable=trigger, width=40)
        triggerWidget.pack(side=tk.RIGHT, pady=6, padx=6)
        triggerWidget.config(background="yellow")

        loggerRoot.info('STARTING thread for %s trigger' % externalTrigger)

        x = Thread(target=queryExternalTrigger, args=(
            ee, triggerWidget, root,), kwargs=externalTrigger, daemon=True)
        x.start()
else:
    externalTriggerStatus = [[True]]

apps = []
if 'camera' in configuration.keys():
    for cameraConfig in configuration['camera']:

        cameraConfig['serialnumber'] = serialNumbers[cameraConfig['ip']]
        thisCamera = runCpp(
            mainframe, cameraConfig, configuration)
        apps.append(thisCamera)

        #add loggers
        loggerRoot.addHandler(thisCamera.queue_handler)
        loggerRoot.addHandler(thisCamera.log_handler)
        loggerRoot.debug('Adding %s camera ' % cameraConfig)


root.protocol("WM_DELETE_WINDOW", killall)

root.mainloop()
