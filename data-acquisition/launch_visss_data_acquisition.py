#!/usr/bin/env python3
"""
VISSS GUI


"""
import argparse
import collections
import datetime
import json
import logging
import logging.handlers
import operator
import os
import pathlib
import queue
import shlex
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
from socket import gethostname, timeout
from subprocess import PIPE, STDOUT, Popen
from textwrap import dedent
from threading import Thread
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText
from urllib.error import HTTPError, URLError

import numpy as np
import serial
import yaml
from filelock import FileLock, Timeout
from PIL import Image
from pysolar import solar
from yaml.constructor import SafeConstructor

SETTINGSFILE = "%s/.visss.yaml" % str(Path.home())
DEFAULTGUI = {
    "geometry": "%dx%d" % (300, 300),
    "configFile": None,
    "autopilot": False,
}
DEFAULTGENERAL = {
    "rotateimage": False,
    "noptp": False,
}
DEFAULTCAMERA = {
    "cpunic": -1,
    "cpuserver": -1,
    "cpustream": -1,
    "cpustorage": ["-1", "-1"],
    "cpuother": -1,
    "cpuffmpeg": ["-1", "-1"],
}


LOGFORMAT = "%(asctime)s: %(levelname)s: %(name)s:%(message)s"
TRIGGERINTERVALLFACTOR = 2  # data can be factor 2 older than interval


def add_bool(self, node):
    """
    Add boolean constructor for YAML parsing.

    Parameters
    ----------
    self : SafeConstructor
        The YAML constructor instance.
    node : yaml.Node
        The YAML node to construct.

    Returns
    -------
    bool
        The constructed boolean value.
    """
    return self.construct_scalar(node)


SafeConstructor.add_constructor("tag:yaml.org,2002:bool", add_bool)


def iter_except(function, exception):
    """
    Iterate over a function until an exception occurs.

    This function works like the built-in 2-argument `iter()`, but stops
    when the specified exception is raised.

    Parameters
    ----------
    function : callable
        The function to call repeatedly.
    exception : Exception
        The exception type to catch and stop iteration on.

    Yields
    ------
    Any
        Results from calling the function.
    """
    try:
        while True:
            yield function()
    except exception:
        return


def writeHTML(file, status, color):
    """
    Write HTML status page.

    Parameters
    ----------
    file : str
        Path to the HTML file to write.
    status : str
        Status message to include in the HTML.
    color : str
        Background color for the HTML page.
    """
    html = f"""
    <html>
    <meta http-equiv='refresh' content='60' />
    <body style='background-color:{color}'>
    <pre>{status}</pre>
    </body>
    </html>
    """
    with open(file, "w") as f:
        f.write(html)


def convert2bool(var):
    """
    Convert various input types to boolean.

    Parameters
    ----------
    var : str, int, bool
        Value to convert to boolean.

    Returns
    -------
    bool
        Converted boolean value.

    Raises
    ------
    ValueError
        If the input cannot be converted to a boolean.
    """
    if var in ["true", "True", 1]:
        var = True
    elif var in ["false", "False", 0]:
        var = False
    else:
        raise ValueError("convert2bool: %s" % var)
    return var


class QueueHandler(logging.Handler):
    """
    A logging handler that sends records to a queue.

    This handler can be used from different threads. The ConsoleUi class
    polls this queue to display records in a ScrolledText widget.

    Attributes
    ----------
    log_queue : queue.Queue
        The queue to send log records to.
    """

    def __init__(self, log_queue):
        """
        Initialize the QueueHandler.

        Parameters
        ----------
        log_queue : queue.Queue
            The queue to send log records to.
        """
        super().__init__()
        self.log_queue = log_queue

    def emit(self, record):
        """
        Emit a log record to the queue.

        Parameters
        ----------
        record : logging.LogRecord
            The log record to emit.
        """
        self.log_queue.put(record)


class runCpp:
    """
    Class to manage a C++ data acquisition process.

    This class handles starting/stopping the C++ process, monitoring its
    output, and managing configuration files.

    Attributes
    ----------
    parent : GUI
        Reference to the parent GUI object.
    cameraConfig : dict
        Configuration dictionary for the camera.
    configuration : dict
        Global configuration dictionary.
    settings : dict
        Application settings.
    hostname : str
        Hostname of the machine.
    rootpath : str
        Root path of the application.
    name : str
        Name of the camera.
    logger : logging.Logger
        Logger for Python components.
    loggerCpp : logging.Logger
        Logger for C++ components.
    logDir : str
        Directory for log files.
    lastImage : str
        Path to the latest image file.
    statusDir : str
        Directory for status files.
    statusHtmlFile : str
        Path to the status HTML file.
    log_queue : queue.Queue
        Queue for log messages.
    queue_handler : QueueHandler
        Handler for queue-based logging.
    log_handler : logging.handlers.TimedRotatingFileHandler
        File handler for logging.
    running : tk.StringVar
        String variable for running status.
    status : tk.StringVar
        String variable for status message.
    configFName : str
        Path to the configuration file.
    command : list
        Command to execute the C++ program.
    statusWidget : ttk.Label
        Widget displaying status.
    startStopButton : ttk.Button
        Button to start/stop the process.
    CleanButton : ttk.Button
        Button to clean the camera.
    runningWidget : ttk.Label
        Widget displaying running status.
    scrolled_text : tk.Text
        Text widget for displaying process output.
    serialPortWiper : str
        Serial port for wiper device.
    cleanThread : threading.Thread
        Thread for checking cleanliness.
    process : subprocess.Popen
        Process object for the C++ program.
    """

    def __init__(self, parent, cameraConfig):
        """
        Initialize the runCpp class.

        Parameters
        ----------
        parent : GUI
            Reference to the parent GUI object.
        cameraConfig : dict
            Configuration dictionary for the camera.
        """
        self.parent = parent
        self.cameraConfig = cameraConfig

        self.configuration = parent.configuration
        self.settings = parent.settings
        self.hostname = parent.hostname
        self.rootpath = parent.rootpath

        self.name = cameraConfig["name"]
        self.logger = logging.getLogger("Python:runCpp:%s" % self.name)
        self.loggerCpp = logging.getLogger("C++:%s" % self.name)

        self.logDir = (
            f"{self.configuration['outdir']}/{self.hostname}_"
            f"{self.cameraConfig['name']}_"
            f"{self.cameraConfig['serialnumber']}/logs"
        )
        self.lastImage = (
            f"{self.configuration['outdir']}/"
            f"{self.cameraConfig['name']}_latest_0.jpg"
        )
        try:
            pathlib.Path(self.logDir).mkdir(parents=True, exist_ok=True)
        except FileExistsError:
            pass
        except PermissionError:
            messagebox.showerror(title=None, message="Cannot create %s" % self.logDir)
            raise PermissionError
        self.statusDir = (
            f"{self.configuration['outdir']}/{self.hostname}_"
            f"{self.cameraConfig['name']}_"
            f"{self.cameraConfig['serialnumber']}"
            "/data"
        )
        self.statusHtmlFile = (
            f"{self.configuration['outdir']}/{self.hostname}_"
            f"{self.cameraConfig['name']}_"
            f"{self.cameraConfig['serialnumber']}"
            "/status.html"
        )

        # Create a logging handler using a queue
        self.log_queue = queue.Queue()
        self.queue_handler = QueueHandler(self.log_queue)
        formatter = logging.Formatter(LOGFORMAT)
        self.queue_handler.setFormatter(formatter)
        self.log_handler = logging.handlers.TimedRotatingFileHandler(
            "%s/log_%s_%s"
            % (self.logDir, self.name, self.cameraConfig["serialnumber"]),
            when="D",
            interval=1,
            backupCount=0,
        )
        self.log_handler.setFormatter(formatter)

        self.logger.addHandler(self.queue_handler)
        self.loggerCpp.addHandler(self.queue_handler)
        self.logger.addHandler(self.log_handler)
        self.loggerCpp.addHandler(self.log_handler)

        self.running = tk.StringVar()
        self.running.set("Idle: %s" % self.name)

        self.status = tk.StringVar()
        self.status.set("-")

        self.configFName = "/tmp/%s_%s_%s.config" % (
            self.name,
            os.path.basename(self.parent.settings["configFile"]),
            str(datetime.date.today()),
        )

        if self.cameraConfig["follower"] in [True, "true", "True", 1]:
            self.cameraConfig["follower"] = 1
        elif self.cameraConfig["follower"] in [False, "false", "False", 0]:
            self.cameraConfig["follower"] = 0
        else:
            raise ValueError(
                "cameraConfig['follower'] must be True or False,"
                " got %s" % cameraConfig["follower"]
            )

        self.command = []
        if ("sshForwarding" in self.cameraConfig.keys()) and (
            self.cameraConfig["sshForwarding"] != "None"
        ):
            raise ValueError("sshForwarding not supported any more")
            # self.command += (
            #     f"ssh -o ServerAliveInterval=60 -tt"
            #     f" {self.cameraConfig['sshForwarding']} DISPLAY=:0 "
            #     )

        self.command += [
            #'systemd-run', '--user', '--scope', #'--property=CPUQuota=100%',
            f"{self.rootpath}/launch_visss_data_acquisition.sh",
            f"--IP={self.cameraConfig['ip']}",
            f"--MAC={self.cameraConfig['mac']}",
            f"--FOLLOWERMODE={self.cameraConfig['follower']}",
            f"--INTERFACE={self.cameraConfig['interface']}",
            f"--MAXMTU={self.configuration['maxmtu']}",
            f"--LIVERATIO={self.configuration['liveratio']}",
            f"--ENCODING={self.configuration['encoding'].replace(' ','@')}",
            f"--CAMERACONFIG={self.configFName}",
            f"--ROOTPATH={self.rootpath}",
            f"--OUTDIR={self.configuration['outdir']}",
            f"--SITE={self.configuration['site']}",
            f"--NAME={self.name}",
            f"--FPS={self.configuration['fps']}",
            f"--NTHREADS={self.configuration['storagethreads']}",
            f"--NEWFILEINTERVAL={self.configuration['newfileinterval']}",
            f"--STOREALLFRAMES={int(self.configuration['storeallframes'])}",
            f"--NOPTP={int(self.configuration['noptp'])}",
            f"--QUERYGAIN={int(self.configuration['querygain'])}",
            f"--ROTATEIMAGE={int(self.configuration['rotateimage'])}",
            f"--MINBRIGHT={self.configuration['minBrightchange']}",
            f"--CPUNIC={self.cameraConfig['cpunic']}",
            f"--CPUSERVER={self.cameraConfig['cpuserver']}",
            f"--CPUSTREAM={self.cameraConfig['cpustream']}",
            f"--CPUSTORAGE={'@'.join(map(str, self.cameraConfig['cpustorage']))}",
            f"--CPUOTHER={self.cameraConfig['cpuother']}",
            f"--CPUFFMPEG={'@'.join(self.cameraConfig['cpuffmpeg'])}",
        ]
        print(self.command)
        frame1 = ttk.Frame(self.parent.mainframe)
        frame1.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.statusWidget = ttk.Label(frame1, textvariable=self.status)
        self.statusWidget.pack(side=tk.BOTTOM, fill=tk.X, pady=6, padx=(6, 6))

        config = ttk.Frame(frame1)
        config.pack(side=tk.TOP, fill=tk.X, pady=6, padx=(6, 6))
        self.startStopButton = ttk.Button(
            config, text="Start/Stop", command=self.clickStartStop
        )

        self.startStopButton.pack(side=tk.RIGHT, anchor=tk.NW)

        self.CleanButton = ttk.Button(config, text="Clean", command=self.clean)
        self.CleanButton.pack(side=tk.RIGHT, anchor=tk.NW, pady=0, padx=(6, 0))
        self.CleanButton.state(["disabled"])
        if "wiper" in self.cameraConfig.keys():
            self.serialPortWiper = self.cameraConfig["wiper"]
        else:
            self.serialPortWiper = None

        self.runningWidget = ttk.Label(config, textvariable=self.running)
        self.runningWidget.pack(side=tk.LEFT, anchor=tk.NW, fill=tk.Y, expand=True)

        # show subprocess' stdout in GUI
        self.scrolled_text = tk.Text(frame1, height=4, width=30)
        self.scrolled_text.pack(
            side=tk.LEFT, fill=tk.BOTH, expand=True, pady=6, padx=(6, 0)
        )

        s = ttk.Scrollbar(frame1, orient=tk.VERTICAL, command=self.scrolled_text.yview)
        s.pack(side=tk.RIGHT, fill=tk.Y, pady=6, padx=(0, 6))
        self.scrolled_text["yscrollcommand"] = s.set
        self.scrolled_text.bind("<Key>", lambda e: "break")

        # Add context menu for copy/paste functionality
        self.scrolled_text.bind("<Button-3>", self.show_context_menu)
        
        self.scrolled_text.configure(font=("TkFixedFont", 8))
        self.scrolled_text.tag_config("INFO", foreground="black")
        self.scrolled_text.tag_config("DEBUG", foreground="gray")
        self.scrolled_text.tag_config("WARNING", foreground="orange")
        self.scrolled_text.tag_config("ERROR", foreground="red")
        self.scrolled_text.tag_config("CRITICAL", foreground="red", underline=1)
        # Start polling messages from the queue
        self.parent.mainframe.after(100, self.poll_log_queue)

        if self.settings["autopilot"] in [True, "true", "True", 1]:
            self.clickStartStop(autopilot=True)
            self.startStopButton.state(["disabled"])

        if "wiper" in self.cameraConfig.keys():
            self.cleanThread = Thread(
                target=self.checkCleaniness, args=(), kwargs={}, daemon=True
            )
            self.cleanThread.start()

    def show_context_menu(self, event):
        """
        Show context menu for copy/paste functionality.
        
        Parameters
        ----------
        event : tkinter.Event
            The mouse event that triggered the context menu.
        """
        # Create context menu
        context_menu = tk.Menu(self.scrolled_text, tearoff=0)
        context_menu.add_command(label="Copy", command=self.copy_text)
        context_menu.add_command(label="Select All", command=self.select_all_text)
        
        # Display the context menu
        context_menu.post(event.x_root, event.y_root)
    
    def copy_text(self):
        """
        Copy selected text to clipboard.
        """
        try:
            selected_text = self.scrolled_text.get("sel.first", "sel.last")
            self.scrolled_text.clipboard_clear()
            self.scrolled_text.clipboard_append(selected_text)
        except tk.TclError:
            # No selection, do nothing
            pass
    
    def select_all_text(self):
        """
        Select all text in the scrolled_text widget.
        """
        self.scrolled_text.tag_add(tk.SEL, "1.0", tk.END)
        self.scrolled_text.mark_set(tk.INSERT, "1.0")
        self.scrolled_text.see(tk.INSERT)
        
    def writeToStatusFile(self, status):
        """
        Write status information to a file.

        Parameters
        ----------
        status : str
            Status message to write.
        """
        now = time.time()
        nowD = datetime.datetime.utcfromtimestamp(now)
        statusDir = f"{self.statusDir}/{nowD.year}/" f"{nowD.month:02}/{nowD.day:02}/"

        statusFile = f'{statusDir}/{self.hostname}_{self.cameraConfig["name"]}_'
        statusFile += f'{self.cameraConfig["serialnumber"]}_{nowD.year}'
        statusFile += f"{nowD.month:02}{nowD.day:02}_status.txt"

        try:
            pathlib.Path(statusDir).mkdir(parents=True, exist_ok=True)
        except FileExistsError:
            pass

        if not os.path.isfile(statusFile):
            self.logger.info("Creating status file %s" % statusFile)

        status = f"{nowD}, {int(now*1000)}, {status}\n"
        self.logger.info(status)
        try:
            with open(statusFile, "a") as sf:
                sf.write(status)
        except Exception as e:
            self.logger.error(e, exc_info=True)
        return

    def witeParamFile(self):
        """
        Write parameter configuration file.

        Returns
        -------
        bool
            True if successful, False otherwise.
        """
        if ("sshForwarding" in self.cameraConfig.keys()) and (
            self.cameraConfig["sshForwarding"] != "None"
        ):
            fname = "%s4ssh" % self.configFName
        else:
            fname = self.configFName

        file = open(fname, "w")
        self.logger.debug("witeParamFile: opening %s" % fname)
        for k, v in self.cameraConfig["teledyneparameters"].items():
            if k == "IO":
                for ii in range(len(self.cameraConfig["teledyneparameters"][k])):
                    for k1, v1 in self.cameraConfig["teledyneparameters"][k][
                        ii
                    ].items():
                        self.logger.debug("witeParamFile: writing: %s %s" % (k1, v1))
                        file.write("%s %s\n" % (k1, v1))
            else:
                self.logger.debug("witeParamFile: writing %s %s" % (k, v))
                file.write("%s %s\n" % (k, v))
        file.close()

        if fname.endswith("4ssh"):
            scp = f"scp -q {fname} {self.cameraConfig['sshForwarding']}:{self.configFName}"
            self.logger.info(f"SCP {scp}")
            p = Popen(shlex.split(scp), stderr=STDOUT, stdout=PIPE)
            p.wait()
            self.logger.info(f"SCP {p.communicate()[0].decode()}")
            if p.returncode != 0:
                self.logger.error(
                    f"Cannot connect to {self.cameraConfig['sshForwarding']} to copy configuration, got {p.returncode}."
                )
                return False
        return True

    def clickStartStop(self, autopilot=False):
        """
        Handle start/stop button clicks.

        Parameters
        ----------
        autopilot : bool, optional
            Whether the action was triggered by autopilot, by default False
        """
        if self.running.get().startswith("Idle"):
            if autopilot:
                self.logger.info("Autopilot starts camera")
                self.writeToStatusFile("launch, autopilot")
            else:
                self.logger.info("User starts camera")
                self.writeToStatusFile("start, user")
            self.start(self.command)
        elif self.running.get().startswith("Running"):
            self.logger.info("User stops camera")
            self.writeToStatusFile("stop, user")
            self.quit()
        else:
            pass

    def statusWatcher(self):
        """
        Watch external trigger status and respond accordingly.
        """
        if np.any(self.parent.externalTriggerStatus):
            if self.running.get().startswith("Idle"):
                self.logger.info("External trigger starts camera")
                self.writeToStatusFile("start, trigger")
                self.start(self.command)
                # line = 'EXTERNAL TRIGGER START: %s \n' % list(
                # map(list, self.parent.externalTriggerStatus))
                # self.text.insert(tk.END, line)
            else:
                self.writeToStatusFile("continue, trigger")
        elif ~np.any(self.parent.externalTriggerStatus):
            if self.running.get().startswith("Running"):
                self.logger.info("External trigger stops camera")
                self.writeToStatusFile("stop, trigger")
                self.quit()
                # line = 'EXTERNAL TRIGGER STOP: %s \n' % list(
                # map(list, self.parent.externalTriggerStatus))
                # self.text.insert(tk.END, line)
                # self.text.see("end")
            else:
                self.writeToStatusFile("sleep, trigger")
        else:
            raise ValueError(
                "do not understand %s"
                % list(map(list, self.parent.externalTriggerStatus))
            )
            self.startStopButton.state(["disabled"])

    def start(self, command):
        """
        Start the C++ data acquisition process.

        Parameters
        ----------
        command : list
            Command to execute the C++ program.
        """
        self.running.set("Running: %s" % self.name)
        self.logger.info("Start camera with %s" % command)

        # start with a clean
        if self.serialPortWiper is not None:
            self.logger.info("Clean camera using port %s" % self.serialPortWiper)
            y = Thread(target=doClean, args=(self.serialPortWiper,), daemon=True)
            y.start()

        self.witeParamFile()
        # myEnv = os.environ.copy()
        # myEnv["TERM"] = "xterm"
        self.process = Popen(command, stdout=PIPE, stderr=STDOUT, preexec_fn=os.setsid)
        # launch thread to read the subprocess output
        # (put the subprocess output into the queue in a background thread, # get output from the queue in the GUI thread. # Output chain: process.readline -> queue -> label) # limit output buffering (may stall subprocess)
        q = Queue()
        t = Thread(target=self.reader_thread, args=[q])
        t.daemon = True  # close pipe if GUI process exits
        t.start()
        self.update(q)  # start update loop

    def reader_thread(self, q):
        """
        Read subprocess output and put it into the queue.

        Parameters
        ----------
        q : queue.Queue
            Queue to put output lines into.
        """
        try:
            with self.process.stdout as pipe:
                for line in iter(pipe.readline, b""):
                    q.put(line)
        finally:
            q.put(None)

    def update(self, q):
        """
        Update GUI with items from the queue.

        Parameters
        ----------
        q : queue.Queue
            Queue containing log messages.
        """
        for line in iter_except(q.get_nowait, Empty):  # display all content
            if line is None:
                self.quit()
                return
            else:
                # if self.carReturn:
                #     self.text.delete("insert linestart", "insert lineend")

                # self.label['text'] = line # update GUI
                # if line.endswith(b'\r'):
                line = line.replace(b"\r", b"\n")
                #     self.carReturn = True
                # else:
                #     self.carReturn = False
                string = line.decode().rstrip()

                if line.startswith(b"STATUS"):
                    self.status.set(string)
                    self.statusWidget.config(background="green")
                    writeHTML(self.statusHtmlFile, string, "green")
                    if self.serialPortWiper is not None:
                        self.CleanButton.state(["!disabled"])
                else:
                    if line.startswith(b"ERROR") or line.startswith(b"FATAL"):
                        self.status.set(string)
                        self.statusWidget.config(background="red")
                        writeHTML(self.statusHtmlFile, string, "red")

                        thisLogger = self.loggerCpp.error
                        # self.text.insert(tk.END, line)
                        # self.text.see("end")
                    elif line.startswith(b"DEBUG") or line.startswith(b"OPENCV"):
                        thisLogger = self.loggerCpp.debug
                        # if logging.root.level <= logging.DEBUG:
                        # self.text.insert(tk.END, line)
                        # self.text.see("end")
                    else:
                        thisLogger = self.loggerCpp.info
                        # self.text.insert(tk.END, line)
                        # self.text.see("end")
                    line4Logger = line.decode().rstrip()
                    if not ((line4Logger.startswith("***") or (line4Logger == ""))):
                        threadN = line4Logger.split("|")[0]
                        if line4Logger.startswith("BASH"):
                            pass
                        elif len(threadN.split("-")) > 1:
                            try:
                                threadN = int(threadN.split("-")[1])
                                line4Logger = "StorageThread%i: %s" % (
                                    threadN,
                                    line4Logger.split("|")[-1],
                                )
                            except ValueError:
                                line4Logger = line4Logger.split("|")[-1]
                        else:
                            line4Logger = line4Logger.split("|")[-1]
                        thisLogger(line4Logger)
                        # print(line4Logger)
                # break # display no more than one line per 40 milliseconds

        self.parent.mainframe.after(100, self.update, q)  # schedule next update

    def display(self, record):
        """
        Display a log record in the GUI.

        Parameters
        ----------
        record : logging.LogRecord
            The log record to display.
        """
        # capture VISSS restarts
        if "Restarting VISSS" in record.getMessage():
            self.writeToStatusFile("start, cpp")

        msg = self.queue_handler.format(record)

        # cut very long text
        text = self.scrolled_text.get("1.0", tk.END)
        if len(text) > 500000:
            self.scrolled_text.delete("1.0", tk.END)
            self.scrolled_text.insert(tk.END, text[-50000:])
            self.scrolled_text.see("end")

        self.scrolled_text.insert(tk.END, msg + "\n", record.levelname)
        # Autoscroll to the bottom
        self.scrolled_text.yview(tk.END)

    def poll_log_queue(self):
        """
        Poll the log queue for new messages to display.
        """
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
        """
        Gracefully quit the C++ process.
        """
        try:
            # self.process.terminate()
            os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
            self.logger.debug("quitting")
        except AttributeError:
            self.logger.error("tried quitting")
            pass
        self.running.set("Idle: %s" % self.name)
        string = "NOT RUNNING (YET)"
        self.status.set(string)
        self.statusWidget.config(background="yellow")
        writeHTML(self.statusHtmlFile, string, "yellow")
        self.CleanButton.state(["disabled"])

    def kill(self):
        """
        Force kill the C++ process.
        """
        try:
            # self.process.kill() # exit subprocess if GUI is closed (zombie!)
            os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
            self.logger.debug("killing")
        except AttributeError:
            self.logger.error("tried killing")
            pass
        self.running.set("Idle: %s" % self.name)
        string = "NOT RUNNING (YET)"
        self.status.set(string)
        self.statusWidget.config(background="yellow")
        writeHTML(self.statusHtmlFile, string, "yellow")
        self.CleanButton.state(["disabled"])

    def checkCleaniness(self):
        """
        Periodically check if the camera needs cleaning.
        """
        revisitTime = 60
        while True:
            time.sleep(revisitTime)
            if not self.running.get().startswith("Running"):
                continue

            if not os.path.isfile(self.lastImage):
                self.logger.error(f"Did not find {self.lastImage}!")
                continue

            # Get modification time of file
            file_mtime = os.path.getmtime(self.lastImage)
            current_time = time.time()
            age = current_time - file_mtime
            if age > self.configuration["newfileinterval"]:
                self.logger.error(f"Last image too old: {age}s!")
                continue

            img = Image.open(self.lastImage).convert("L")
            height_offset = 64
            brightnessThreshold = 50
            arr = np.array(img)[height_offset:]
            nPixel = arr.shape[0] * arr.shape[1]
            dark_ratio = np.sum(arr < brightnessThreshold) / nPixel
            if dark_ratio < (self.configuration["wiperThreshold"] / 100):
                self.logger.info(f"Image is not blocked: {dark_ratio*100}%")
                continue

            self.logger.info(f"Image is blocked: {dark_ratio*100}%. Cleaning!")
            self.clean()
            # wait longer after cleaning to avoid cleaning too often!
            time.sleep(revisitTime)

        return

    def clean(self):
        """
        Clean the camera using the wiper device.
        """
        self.startStopButton.state(["disabled"])
        self.CleanButton.state(["disabled"])
        self.logger.info("Cleaning camera using port %s" % self.serialPortWiper)

        if self.running.get().startswith("Running"):
            self.quit()
            y = Thread(target=doClean, args=(self.serialPortWiper,), daemon=True)
            y.start()
            self.parent.root.after(5 * 1000, self.endClean)  # schedule restart

    def endClean(self):
        """
        Complete the cleaning process and restart if needed.
        """
        self.logger.info("Done cleaning camera")
        if self.running.get().startswith("Idle"):
            self.start(self.command)
        self.startStopButton.state(["!disabled"])
        self.CleanButton.state(["!disabled"])


def doClean(com_port):
    """
    Send cleaning command to the wiper device.

    Parameters
    ----------
    com_port : str
        Serial port to communicate with the wiper device.
    """
    sendString = "wipe"
    baudrate = 115200
    sendString = sendString + "\r\n"
    sendString = sendString.encode("utf-8")
    with serial.Serial(
        com_port,
        baudrate,
        timeout=10800,
        rtscts=False,
        dsrdtr=False,
        xonxoff=True,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
    ) as serialPort:
        serialPort.write(sendString)
    return


class GUI(object):
    """
    Main GUI class for VISSS data acquisition.

    This class manages the graphical user interface, handles configuration
    loading, and controls multiple camera processes.

    Attributes
    ----------
    rootpath : str
        Root path of the application.
    loggerRoot : logging.Logger
        Root logger for the application.
    hostname : str
        Hostname of the machine.
    externalTriggerStatus : list
        List of deques tracking external trigger status.
    settings : dict
        Application settings.
    configuration : dict
        Global configuration.
    autopilot : tk.IntVar
        Variable for autopilot state.
    apps : list
        List of runCpp instances.
    root : tkinter.Tk
        Main Tkinter window.
    mainframe : ttk.Frame
        Main frame container.
    """

    def __init__(self, loglevel):
        """
        Initialize the GUI class.

        Parameters
        ----------
        loglevel : str
            Logging level for the application.
        """
        self.rootpath = os.path.dirname(os.path.abspath(__file__))

        logging.basicConfig(level=loglevel, format=LOGFORMAT)
        self.loggerRoot = logging.getLogger("Python")
        self.loggerRoot.info("Launching GUI")
        self.loggerRoot.debug("Rootpath %s" % self.rootpath)

        self.hostname = gethostname()
        # self.getSerialNumbers()
        self.externalTriggerStatus = [[]]

        self.settings = deepcopy(DEFAULTGUI)
        self.settings.update(self.read_settings(SETTINGSFILE))
        # reset geometery if broken

        if self.settings["geometry"].startswith("1x1"):
            self.settings["geometry"] = DEFAULTGUI["geometry"]

        self.sunAltitude = 999
        self.sunOldAltitude = 999

        self.root = tk.Tk()

        self.root.title("VISSS data acquisition")
        self.root.bind("<Configure>", self.save_settings)
        self.root.geometry(self.settings["geometry"])

        # mainframe = tttk.Frame(self.root, padding="3 3 12 12")
        # mainframe.grid(column=0, row=0, sticky=(tk.N, tk.W, tk.E, tk.S))

        self.mainframe = ttk.Frame(self.root)
        self.mainframe.pack(fill=tk.BOTH, expand=1)

        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        config = ttk.Frame(self.mainframe)
        config.pack(side=tk.TOP, fill=tk.X)
        button = ttk.Button(
            config, text="Configuration File", command=lambda: self.askopenfile()
        )

        button.pack(side=tk.LEFT, anchor=tk.NW, pady=6, padx=(6, 0))

        try:
            statusStr = self.settings["configFile"].split("/")[-1]
        except AttributeError:
            statusStr = "None"
        status = ttk.Label(config, text=statusStr)
        status.pack(side=tk.LEFT, pady=6, padx=(6, 0))

        self.configuration = deepcopy(DEFAULTGENERAL)
        self.configuration.update(self.read_settings(self.settings["configFile"]))

        self.autopilot = tk.IntVar()
        self.apps = []

        if "camera" in self.configuration.keys():
            for cameraConfig1 in self.configuration["camera"]:
                cameraConfig = deepcopy(DEFAULTCAMERA)
                cameraConfig.update(cameraConfig1)

                thisCamera = runCpp(self, cameraConfig)
                self.apps.append(thisCamera)

                # add loggers
                self.loggerRoot.addHandler(thisCamera.queue_handler)
                self.loggerRoot.addHandler(thisCamera.log_handler)
                self.loggerRoot.debug("Adding %s camera " % cameraConfig)

        ChkBttn = ttk.Checkbutton(
            config,
            text="Autopilot",
            command=self.click_autopilot,
            variable=self.autopilot,
        )
        ChkBttn.pack(side=tk.LEFT, pady=6, padx=6)
        if self.settings["autopilot"]:
            ChkBttn.invoke()

        if ("externalTrigger" in self.configuration.keys()) and (
            self.configuration["externalTrigger"] is not None
        ):
            self.triggerHtmlFile = (
                f"{self.configuration['outdir']}/externalTrigger.html"
            )

            writeHTML(self.triggerHtmlFile, "no trigger", "gray")

            self.externalTriggerStatus = []
            for ee, externalTrigger in enumerate(self.configuration["externalTrigger"]):
                self.externalTriggerStatus.append(
                    collections.deque(maxlen=externalTrigger["nBuffer"])
                )

                trigger = tk.StringVar()
                string = "%s: -" % externalTrigger["name"]
                trigger.set(string)

                triggerWidget = ttk.Label(config, textvariable=trigger, width=40)
                triggerWidget.pack(side=tk.RIGHT, pady=6, padx=6)
                triggerWidget.config(background="yellow")

                writeHTML(self.triggerHtmlFile, string, "yellow")

                self.loggerRoot.info("STARTING thread for %s trigger" % externalTrigger)

                x = Thread(
                    target=self.queryExternalTrigger,
                    args=(
                        ee,
                        trigger,
                        triggerWidget,
                    ),
                    kwargs=externalTrigger,
                    daemon=True,
                )
                x.start()

        return

    def getSerialNumbers(self):
        """
        Get serial numbers of connected cameras.
        """
        self.loggerRoot.debug("getting serial numbers")

        self.serialNumbers = {}
        p = Popen("lsgev -v", shell=True, stdout=PIPE, stderr=STDOUT)
        for line in p.stdout.readlines():
            line = line.decode()
            if line.startswith("0 cameras detected"):
                self.serialNumbers = None
                return
            ip = line.split("]")[1].split("[")[1]
            serial = line.split("]")[-2].split(":")[-1]
            self.serialNumbers[ip] = serial
        retval = p.wait()
        self.loggerRoot.info("got serial numbers: %s" % self.serialNumbers)

    def click_autopilot(self):
        """
        Handle autopilot checkbox toggle.
        """
        self.save_settings(None)
        self.loggerRoot.info("Autopilot set to %s" % bool(self.autopilot.get()))
        for app in self.apps:
            if self.autopilot.get():
                app.startStopButton.state(["disabled"])
            else:
                app.startStopButton.state(["!disabled"])
                if len(self.externalTriggerStatus) > 0:
                    try:
                        self.externalTriggerStatus[0][0] = True
                    except IndexError:
                        self.externalTriggerStatus[0].append(True)

    def askopenfile(self):
        """
        Open file dialog to select configuration file.
        """
        file = filedialog.askopenfilename(filetypes=[("YAML files", ".yaml")])
        if file is not None:
            self.settings["configFile"] = file
            self.configuration = self.read_settings(file)
            self.save_settings(None)
            messagebox.showwarning(title=None, message="Restart to apply settings")
        else:
            messagebox.showerror(title=None, message="File not found")

    def read_settings(self, fname):
        """
        Read settings from a YAML file.

        Parameters
        ----------
        fname : str
            Path to the settings file.

        Returns
        -------
        dict
            Dictionary containing the settings.
        """
        self.loggerRoot.info("read_settings: %s" % fname)
        if fname is None:
            return {}

        try:
            with open(fname, "r") as stream:
                settings = yaml.safe_load(stream)
        except (FileNotFoundError, yaml.YAMLError) as e:
            self.loggerRoot.error(e, exc_info=True)
            messagebox.showerror(title=None, message="File %s not found" % fname)
            settings = {}
        else:
            for k in [
                "storeallframes",
                "autopilot",
                "querygain",
                "rotateimage",
                "noptp",
            ]:
                if k in settings.keys():
                    settings[k] = convert2bool(settings[k])

        self.loggerRoot.info("read_settings: %s" % settings)
        return settings

    def save_settings(self, event):
        """
        Save current settings to file.

        Parameters
        ----------
        event : tkinter.Event
            Tkinter event that triggered the save.
        """
        # it is called to often event though window size is not changing
        if event is not None:
            if self.settings["geometry"] == self.root.geometry():
                return
        # gather setings
        self.settings["geometry"] = self.root.geometry()
        self.settings["autopilot"] = bool(self.autopilot.get())

        # write settings
        with open(SETTINGSFILE, "w+") as stream:
            yaml.dump(
                self.settings, stream, default_flow_style=False, allow_unicode=True
            )
        self.loggerRoot.info("save_settings: %s" % self.settings)
        return

    def killall(self):
        """
        Kill all running processes and close the GUI.
        """
        self.loggerRoot.info("Closing GUI")
        for app in self.apps:
            app.writeToStatusFile("terminate, user")
            app.quit()
        self.root.destroy()

    def sunIsUp(self):
        """
        Check if the sun is currently above the horizon.

        Returns
        -------
        bool
            True if sun altitude is >= 0, False otherwise.
        """
        now = datetime.datetime.now(datetime.timezone.utc)
        longitude = self.configuration["longitude"]
        latitude = self.configuration["latitude"]
        self.sunAltitude = int(round(solar.get_altitude(latitude, longitude, now)))

        if (self.sunAltitude >= 0) and (self.sunOldAltitude < 0):
            self.loggerRoot.info("sunIsUp: sunrise detected")

        if (self.sunAltitude < 0) and (self.sunOldAltitude >= 0):
            self.loggerRoot.info("sunIsUp: sunset detected")

        self.sunOldAltitude = deepcopy(self.sunAltitude)

        return self.sunAltitude >= 0

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
        nightOnly,
    ):
        """
        Query external trigger status periodically.

        Parameters
        ----------
        nn : int
            Index of the trigger.
        trigger : tk.StringVar
            Variable to store trigger status.
        triggerWidget : ttk.Label
            Widget to display trigger status.
        name : str
            Name of the trigger.
        address : str
            Address to query for trigger data.
        interval : int
            Interval between queries in seconds.
        threshold : float
            Threshold value for trigger condition.
        minMax : str
            Comparison operator ('min' or 'max').
        stopOnTimeout : bool
            Whether to stop measurement on timeout.
        nBuffer : int
            Size of the trigger status buffer.
        nightOnly : bool
            Whether to only trigger during nighttime.
        """
        if not bool(self.autopilot.get()):
            triggerWidget.config(background="gray")
            trigger.set("external trigger disabled")
            writeHTML(self.triggerHtmlFile, "external trigger disabled", "gray")
            self.root.after(
                100,
                lambda: self.queryExternalTrigger(
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
                    nightOnly,
                ),
            )  # schedule next update
            return

        elif nightOnly and self.sunIsUp():
            triggerWidget.config(background="green")
            trigger.set(f"sun is at {self.sunAltitude}° - external trigger disabled")
            writeHTML(
                self.triggerHtmlFile,
                f"sun is at {self.sunAltitude}° - external trigger disabled",
                "gray",
            )
            self.root.after(
                interval * 1000,
                lambda: self.queryExternalTrigger(
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
                    nightOnly,
                ),
            )  # schedule next update

            # start measurements if required:
            self.externalTriggerStatus[nn].append(True)
            for app in self.apps:
                app.statusWatcher()

            return

        self.root.after(
            interval * 1000,
            lambda: self.queryExternalTrigger(
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
                nightOnly,
            ),
        )  # schedule next update

        # default values
        data = {
            "unit": "",
            "measurement": np.nan,
            "timestamp": np.datetime64("now"),
        }

        if minMax == "min":
            oper = operator.ge
        elif minMax == "min":
            oper = operator.le
        else:
            raise ValueError("minMax must be min or max")

        stopOnTimeout = convert2bool(stopOnTimeout)
        nightOnly = convert2bool(nightOnly)

        now = np.datetime64("now")
        try:
            response = (
                urllib.request.urlopen(address, timeout=10).read().decode("utf-8")
            )
        except (HTTPError, URLError) as error:
            self.loggerRoot.error(
                "queryExternalTrigger: Data not retrieved because %s URL: %s",
                error,
                address,
            )
            if stopOnTimeout:
                continueMeasurement = False
            else:
                continueMeasurement = True
            measurement = "NO RESPONSE"
            unit = ""
        else:
            data = json.loads(response)[name]
            self.loggerRoot.info("queryExternalTrigger: response %s" % str(data))

            timeCond = now - np.datetime64(data["timestamp"]) < np.timedelta64(
                TRIGGERINTERVALLFACTOR * int(interval), "s"
            )
            if timeCond:
                continueMeasurement = oper(float(data["measurement"]), threshold)
            else:
                self.loggerRoot.error(
                    "queryExternalTrigger: Data too old %s"
                    % (np.datetime64(data["timestamp"]))
                )
                if stopOnTimeout:
                    continueMeasurement = False
                else:
                    continueMeasurement = True

            measurement = "%g" % data["measurement"]
            unit = data["unit"]

        string = "%s: %s %s %i/%i at %s" % (
            name,
            measurement,
            unit,
            np.sum(self.externalTriggerStatus[nn]),
            nBuffer,
            data["timestamp"],
        )

        self.loggerRoot.info(string)

        self.externalTriggerStatus[nn].append(continueMeasurement)

        if np.any(self.externalTriggerStatus[nn]):
            color = "green"
        else:
            color = "yellow"

        triggerWidget.config(background=color)
        trigger.set(string)
        writeHTML(self.triggerHtmlFile, string, color)
        for app in self.apps:
            app.statusWatcher()

        return


if __name__ == "__main__":
    lockfile = "/tmp/launch_visss_data_acquisition.lock"
    lock = FileLock(lockfile, timeout=1)

    try:
        with lock:
            parser = argparse.ArgumentParser()
            parser.add_argument(
                "-log",
                "--loglevel",
                default="info",
                help=(
                    "Provide logging level. Example --loglevel " "debug, default=info"
                ),
            )

            args = parser.parse_args()
            visssgui = GUI(args.loglevel.upper())
            visssgui.root.protocol("WM_DELETE_WINDOW", visssgui.killall)
            visssgui.root.mainloop()

    except Timeout:
        print("lock file error.")
        messagebox.showerror(
            title=None,
            message=f"VISSS data acqusition GUI alreaady running! Lock file: {lockfile}",
        )
