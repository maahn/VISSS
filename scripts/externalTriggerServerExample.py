#!/usr/bin/env python
"""
Very simple HTTP server to provide last measurement of an external trigger. 
A random geneator is used here instad of real data.

"""

from http.server import HTTPServer, BaseHTTPRequestHandler
import json
import numpy as np

tmpfile = 'tmp.json'
maxCacheAge = 1  # s
instrument = 'random'
port = 8123

class S(BaseHTTPRequestHandler):
    def _set_headers(self):
        self.send_response(200)
        self.send_header("Content-type", "text/json")
        self.end_headers()

    def do_GET(self):

        try:
            with open(tmpfile) as f:
                cachedDat = json.load(f)
                cachedDat[instrument]['timestamp'] = np.datetime64(
                    cachedDat[instrument]['timestamp'])
        except FileNotFoundError:
            cachedDat = None

        # no cache or old cache
        if (
            (cachedDat is None) or
            ((np.datetime64('now') - cachedDat[instrument]['timestamp'])
             > np.timedelta64(maxCacheAge, 's'))):

            ##### add code to get the radar data#####
            try:
                # replace with time from the radar
                timestamp = np.datetime64('now')
                # replace with last measurement from the radar
                measurement = np.random.randint(-10, 10)
            # could not get radar data, use cached data
            except:
                timestamp = cachedDat[instrument]["timestamp"]
                measurement = cachedDat[instrument]["measurement"]

            #######

            dat = {instrument: {
                "timestamp": timestamp,
                "unit": "dBz",
                "measurement": measurement,
            }}

        # cache not very old
        else:
            dat = cachedDat

        #send data to client
        self._set_headers()
        self.wfile.write(json.dumps(dat, default=str).encode('utf-8'))

        #write data to cache file
        with open(tmpfile, 'w') as f:
            cachedDat = json.dump(dat, f, default=str)


def run(server_class=HTTPServer, handler_class=S, addr="localhost", port=8000):
    server_address = (addr, port)
    httpd = server_class(server_address, handler_class)

    print(f"Starting httpd server on {addr}:{port}")
    httpd.serve_forever()


if __name__ == "__main__":

    run(addr="", port=port)
