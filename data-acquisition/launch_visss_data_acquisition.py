#!/usr/bin/env python3
"""
VISSS GUI


"""
import collections
import datetime
import json
import logging
import logging.handlers
import operator
import os
import pathlib
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
SafeConstructor.add_constructor(u'tag:yaml.org,2002:bool', add_bool)


def iter_except(function, exception):
    """Works like builtin 2-argument `iter()`, but stops on `exception`."""
    try:
        while True:
            yield function()
    except exception:
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
    def __init__(self, parent, cameraConfig):

        self.parent = parent
        self.cameraConfig = cameraConfig

        self.configuration = parent.configuration
        self.settings = parent.settings
        self.hostname = parent.hostname
        self.rootpath = parent.rootpath
        
        self.name = cameraConfig['name']
        self.logger = logging.getLogger('Python:runCpp:%s' % self.name)
        self.loggerCpp = logging.getLogger('C++:%s' % self.name)

        self.logDir = (f"{self.configuration['outdir']}/{self.hostname}_"
                       f"{self.cameraConfig['name']}_"
                       f"{self.cameraConfig['serialnumber']}/logs")
        try:
            pathlib.Path(self.logDir).mkdir( parents=True, exist_ok=True )
        except FileExistsError:
            pass
        except PermissionError:
            messagebox.showerror(title=None, message='Cannot create %s'%self.logDir)
            raise PermissionError
        self.statusDir = (f"{self.configuration['outdir']}/{self.hostname}_"
                          f"{self.cameraConfig['name']}_"
                          f"{self.cameraConfig['serialnumber']}"
                          "/data")

        # Create a logging handler using a queue
        self.log_queue = queue.Queue()
        self.queue_handler = QueueHandler(self.log_queue)
        formatter = logging.Formatter(LOGFORMAT)
        self.queue_handler.setFormatter(formatter)
        self.log_handler = logging.handlers.TimedRotatingFileHandler(
            '%s/log_%s_%s' % (self.logDir, self.name,
                              self.cameraConfig['serialnumber']),
            when='D', interval=1,
            backupCount=0)
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

        if self.cameraConfig['follower'] in [True, 'true', 'True', 1]:
            self.cameraConfig['follower'] = 1
        elif self.cameraConfig['follower'] in [False, 'false', 'False', 0]:
            self.cameraConfig['follower'] = 0
        else:
            raise ValueError("cameraConfig['follower'] must be True or False,"
                " got %s"%cameraConfig['follower'])

        self.command = (
            f"/usr/bin/env bash"
            f" {self.rootpath}/launch_visss_data_acquisition.sh"
            f" --IP={self.cameraConfig['ip']}"
            f" --MAC={self.cameraConfig['mac']}"
            f" --FOLLOWERMODE={self.cameraConfig['follower']}"
            f" --INTERFACE={self.cameraConfig['interface']}"
            f" --MAXMTU={self.configuration['maxmtu']}"
            f" --LIVERATIO={self.configuration['liveratio']}"
            f" --PRESET={self.configuration['preset']}"
            f" --QUALITY={self.configuration['quality']}"
            f" --CAMERACONFIG={self.configFName}"
            f" --ROOTPATH={self.rootpath}"
            f" --OUTDIR={self.configuration['outdir']}"
            f" --SITE={self.configuration['site']}"
            f" --NAME={self.name}"
            f" --FPS={self.configuration['fps']}"
            f" --NTHREADS={self.configuration['storagethreads']}"
            f" --NEWFILEINTERVAL={self.configuration['newfileinterval']}"
            f" --STOREALLFRAMES={int(self.configuration['storeallframes'])}"
        )

        frame1 = ttk.Frame(self.parent.mainframe)
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

        s = ttk.Scrollbar(frame1, orient=tk.VERTICAL,
                          command=self.scrolled_text.yview)
        s.pack(side=tk.RIGHT, fill=tk.Y, pady=6, padx=(0, 6))
        self.scrolled_text['yscrollcommand'] = s.set
        self.scrolled_text.bind("<Key>", lambda e: "break")

        self.scrolled_text.configure(font=('TkFixedFont', 8))
        self.scrolled_text.tag_config('INFO', foreground='black')
        self.scrolled_text.tag_config('DEBUG', foreground='gray')
        self.scrolled_text.tag_config('WARNING', foreground='orange')
        self.scrolled_text.tag_config('ERROR', foreground='red')
        self.scrolled_text.tag_config(
            'CRITICAL', foreground='red', underline=1)
        # Start polling messages from the queue
        self.parent.mainframe.after(100, self.poll_log_queue)

        if self.settings['autopilot'] in [True, 'true', 'True', 1]:
            self.clickStartStop(autopilot=True)
            self.startStopButton.state(["disabled"])

    def writeToStatusFile(self, status):
        now = time.time()
        nowD = datetime.datetime.utcfromtimestamp(now)
        statusDir = (f'{self.statusDir}/{nowD.year}/'
                     f'{nowD.month:02}/{nowD.day:02}/')

        statusFile = f'{statusDir}/{self.hostname}_{self.cameraConfig["name"]}_'
        statusFile += f'{self.cameraConfig["serialnumber"]}_{nowD.year}'
        statusFile += f'{nowD.month:02}{nowD.day:02}_status.txt'

        try:
            pathlib.Path(statusDir).mkdir( parents=True, exist_ok=True )
        except FileExistsError:
            pass

        if not os.path.isfile(statusFile):
            self.logger.info('Creating status file %s' % statusFile)

        status = f'{nowD}, {int(now*1000)}, {status}\n'
        self.logger.info(status)
        try:
            with open(statusFile, 'a') as sf:
                sf.write(status)
        except Exception as e:
            self.logger.error(e, exc_info=True)
        return

    def witeParamFile(self):

        file = open(self.configFName, 'w')
        self.logger.debug("witeParamFile: opening %s" % self.configFName)
        for k, v in self.cameraConfig['teledyneparameters'].items():
            if k == 'IO':
                for ii in range(len(
                        self.cameraConfig['teledyneparameters'][k])):
                    for k1, v1 in self.cameraConfig[
                            'teledyneparameters'][k][ii].items():
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
                self.writeToStatusFile('launch, autopilot')
            else:
                self.logger.info('User starts camera')
                self.writeToStatusFile('start, user')
            self.start(self.command.split(' '))
        elif self.running.get().startswith('Running'):
            self.logger.info('User stops camera')
            self.writeToStatusFile('stop, user')
            self.quit()
        else:
            pass

    def statusWatcher(self):

        if np.any(self.parent.externalTriggerStatus):
            if self.running.get().startswith('Idle'):
                self.logger.info('External trigger starts camera')
                self.writeToStatusFile('start, trigger')
                self.start(self.command.split(' '))
                # line = 'EXTERNAL TRIGGER START: %s \n' % list(
                # map(list, self.parent.externalTriggerStatus))
                # self.text.insert(tk.END, line)
            else:
                self.writeToStatusFile('continue, trigger')
        elif (~np.any(self.parent.externalTriggerStatus)):
            if self.running.get().startswith('Running'):
                self.logger.info('External trigger stops camera')
                self.writeToStatusFile('stop, trigger')
                self.quit()
                # line = 'EXTERNAL TRIGGER STOP: %s \n' % list(
                # map(list, self.parent.externalTriggerStatus))
                # self.text.insert(tk.END, line)
                # self.text.see("end")
            else:
                self.writeToStatusFile('sleep, trigger')
        else:
            raise ValueError('do not understand %s' %
                             list(map(list, self.parent.externalTriggerStatus)))
            self.startStopButton.state(["disabled"])

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
                    elif ((line.startswith(b'DEBUG') or
                           line.startswith(b'OPENCV'))):
                        thisLogger = self.loggerCpp.debug
                        # if logging.root.level <= logging.DEBUG:
                        # self.text.insert(tk.END, line)
                        # self.text.see("end")
                    else:
                        thisLogger = self.loggerCpp.info
                        # self.text.insert(tk.END, line)
                        # self.text.see("end")
                    line4Logger = line.decode().rstrip()
                    if not ((line4Logger.startswith('***') or
                             (line4Logger == ""))):
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

        self.parent.mainframe.after(100, self.update, q)  # schedule next update

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
        self.parent.mainframe.after(100, self.poll_log_queue)

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



class GUI(object):
    def __init__(self, loglevel):
            
        self.rootpath  = os.path.dirname(os.path.abspath(__file__))


        logging.basicConfig(level=loglevel,
                            format=LOGFORMAT)
        self.loggerRoot = logging.getLogger('Python')
        self.loggerRoot.info('Launching GUI')
        self.loggerRoot.debug('Rootpath %s' % self.rootpath)

        self.hostname = gethostname()
        #self.getSerialNumbers()
        self.externalTriggerStatus = [[]]

        self.settings = deepcopy(DEFAULTSETTINGS)
        self.settings.update(self.read_settings(SETTINGSFILE))
        # reset geometery if broken
        if self.settings['geometry'].startswith('1x1'):
            self.settings['geometry'] = DEFAULTSETTINGS['geometry']

        self.root = tk.Tk()

        self.root.title("VISSS data acquisition")
        self.root.bind("<Configure>", self.save_settings)
        self.root.geometry(self.settings['geometry'])

        # mainframe = tttk.Frame(self.root, padding="3 3 12 12")
        # mainframe.grid(column=0, row=0, sticky=(tk.N, tk.W, tk.E, tk.S))


        self.mainframe = ttk.Frame(self.root)
        self.mainframe.pack(fill=tk.BOTH, expand=1)

        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        config = ttk.Frame(self.mainframe)
        config.pack(side=tk.TOP, fill=tk.X)
        button = ttk.Button(config, text="Configuration File",
                            command=lambda: self.askopenfile())

        button.pack(side=tk.LEFT, anchor=tk.NW, pady=6, padx=(6, 0))
        try:
            statusStr = self.settings['configFile'].split('/')[-1]
        except AttributeError:
            statusStr = 'None'
        status = ttk.Label(config, text=statusStr)
        status.pack(side=tk.LEFT, pady=6, padx=(6, 0))

        self.configuration = self.read_settings(self.settings['configFile'])



        self.autopilot = tk.IntVar()
        self.apps = []
        if 'camera' in self.configuration.keys():
            for cameraConfig in self.configuration['camera']:
              
                thisCamera = runCpp(self, cameraConfig)
                self.apps.append(thisCamera)

                # add loggers
                self.loggerRoot.addHandler(thisCamera.queue_handler)
                self.loggerRoot.addHandler(thisCamera.log_handler)
                self.loggerRoot.debug('Adding %s camera ' % cameraConfig)

        ChkBttn = ttk.Checkbutton(
            config,
            text='Autopilot',
            command=self.click_autopilot,
            variable=self.autopilot)
        ChkBttn.pack(side=tk.LEFT, pady=6, padx=6)
        if self.settings['autopilot']:
            ChkBttn.invoke()


        if (('externalTrigger' in self.configuration.keys()) and
                (self.configuration['externalTrigger'] is not None)):
            self.externalTriggerStatus = []
            for ee, externalTrigger in enumerate(self.configuration['externalTrigger']):

                self.externalTriggerStatus.append(
                    collections.deque(maxlen=externalTrigger['nBuffer']))

                trigger = tk.StringVar()
                trigger.set('%s: -' % externalTrigger['name'])

                triggerWidget = ttk.Label(config, textvariable=trigger, width=40)
                triggerWidget.pack(side=tk.RIGHT, pady=6, padx=6)
                triggerWidget.config(background="yellow")

                self.loggerRoot.info('STARTING thread for %s trigger' % externalTrigger)

                x = Thread(target=self.queryExternalTrigger, args=(
                    ee, trigger, triggerWidget,), kwargs=externalTrigger, daemon=True)
                x.start()

        return


    def getSerialNumbers(self):
        self.loggerRoot.debug('getting serial numbers')

        self.serialNumbers = {}
        p = Popen('lsgev -v', shell=True, stdout=PIPE, stderr=STDOUT)
        for line in p.stdout.readlines():
            line = line.decode()
            if line.startswith('0 cameras detected'):
                self.serialNumbers = None
                return
            ip = line.split(']')[1].split('[')[1]
            serial = line.split(']')[-2].split(':')[-1]
            self.serialNumbers[ip] = serial
        retval = p.wait()
        self.loggerRoot.info('got serial numbers: %s' % self.serialNumbers)


    def click_autopilot(self):

        self.save_settings(None)
        self.loggerRoot.info('Autopilot set to %s' % bool(self.autopilot.get()))
        for app in self.apps:
            if self.autopilot.get():
                app.startStopButton.state(["disabled"])
            else:
                app.startStopButton.state(["!disabled"])
                self.externalTriggerStatus = [[True]]

    def askopenfile(self):
        file = filedialog.askopenfilename(filetypes=[("YAML files", ".yaml")])
        if file is not None:
            self.settings['configFile'] = file
            self.configuration = self.read_settings(file)
            self.save_settings(None)
            messagebox.showwarning(title=None, message='Restart to apply settings')
        else:
            messagebox.showerror(title=None, message='File not found')

    def read_settings(self, fname):
        self.loggerRoot.info('read_settings: %s' % fname)
        if fname is None:
            return {}

        try:
            with open(fname, 'r') as stream:
                settings = yaml.safe_load(stream)
        except (FileNotFoundError, yaml.YAMLError) as e:
            self.loggerRoot.error(e, exc_info=True)
            messagebox.showerror(title=None, message='File %s not found'%fname)
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

        self.loggerRoot.info('read_settings: %s' % settings)
        return settings

    def save_settings(self, event):
        # it is called to often event hough window size is not changing
        if event is not None:
            if self.settings['geometry'] == self.root.geometry():
                return
        # gather setings
        self.settings['geometry'] = self.root.geometry()
        self.settings['autopilot'] = bool(self.autopilot.get())

        # write settings
        with open(SETTINGSFILE, "w+") as stream:
            yaml.dump(self.settings, stream, default_flow_style=False,
                      allow_unicode=True)
        self.loggerRoot.debug('save_settings: %s' % self.settings)
        return

    def killall(self):
        self.loggerRoot.info('Closing GUI')
        for app in self.apps:
            app.writeToStatusFile('terminate, user')
            app.quit()
        self.root.destroy()




    def queryExternalTrigger(
        self,
        nn,
        trigger,
        triggerWidget,
        name,
        address,
        interval,
        threshold,
        minMax,
        stopOnTimeout,
        nBuffer,
    ):


        if not bool(self.autopilot.get()):
            triggerWidget.config(background="gray")
            trigger.set('external trigger disabled')
            self.root.after(100, lambda: self.queryExternalTrigger(
                nn,
                trigger,
                triggerWidget,
                name,
                address,
                interval,
                threshold,
                minMax,
                stopOnTimeout,
                nBuffer,
            ))  # schedule next update
            return

        self.root.after(interval*1000, lambda: self.queryExternalTrigger(
            nn,
            trigger,
            triggerWidget,
            name,
            address,
            interval,
            threshold,
            minMax,
            stopOnTimeout,
            nBuffer,
        ))  # schedule next update

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
            self.loggerRoot.error(
                'queryExternalTrigger: Data not retrieved because %s URL: %s',
                error, address)
            if stopOnTimeout:
                continueMeasurement = False
            else:
                continueMeasurement = True
            measurement = '-'
            unit = ''
        else:
            data = json.loads(response)[name]
            self.loggerRoot.info('queryExternalTrigger: response %s' % str(data))

            timeCond = (now - np.datetime64(data['timestamp']) <
                        np.timedelta64(TRIGGERINTERVALLFACTOR * int(
                            interval), 's'))
            if timeCond:
                continueMeasurement = oper(float(data['measurement']), threshold)
            else:
                self.loggerRoot.error('queryExternalTrigger: Data too old %s' %
                                 (np.datetime64(data['timestamp'])))
                if stopOnTimeout:
                    continueMeasurement = False
                else:
                    continueMeasurement = True

        self.loggerRoot.info('queryExternalTrigger: continue Measurement %r' %
                        continueMeasurement)

        measurement = '%g' % data['measurement']
        unit = data['unit']

        self.externalTriggerStatus[nn].append(continueMeasurement)

        if np.any(self.externalTriggerStatus[nn]):
            triggerWidget.config(background="green")
        else:
            triggerWidget.config(background="red")
        trigger.set('%s: %s %s %i/%i at %s' %
                    (name, measurement, unit, np.sum(self.externalTriggerStatus[nn]),
                        nBuffer, data['timestamp']))

        for app in self.apps:
            app.statusWatcher()

        return


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-log',
                        '--loglevel',
                        default='info',
                        help=('Provide logging level. Example --loglevel '
                              'debug, default=info'))

    args = parser.parse_args()
    visssgui = GUI(args.loglevel.upper())
    visssgui.root.protocol("WM_DELETE_WINDOW", visssgui.killall)
    visssgui.root.mainloop()

