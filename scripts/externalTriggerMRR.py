#!/usr/bin/env python
"""
Very simple HTTP server to provide last measurement of an external trigger. 


"""

import json
from http.server import BaseHTTPRequestHandler, HTTPServer

import numpy as np

instrument = "radar"
port = 8123


class S(BaseHTTPRequestHandler):
    def _set_headers(self):
        self.send_response(200)
        self.send_header("Content-type", "text/json")
        self.end_headers()

    def do_GET(self):
        # Always answer, whatever happens below: the VISSS launcher treats a
        # missing/hanging response as "no data" and (with stopOnTimeout) stops
        # measuring, so failing silently here is worse than reporting the 9999
        # sentinel value.
        timestamp = np.datetime64("now")
        measurement = 9999.0

        try:
            # open MRR file (raw string: the path contains backslash escapes)
            with open(r"C:\MRR_data\ActData\AveData.ave", "r") as file1:
                Lines = file1.readlines()

            Z_profile = None
            for line in Lines:
                if line.startswith("Z"):
                    Z_profile = line

            if Z_profile is None:
                raise ValueError("no Z profile line in AveData.ave")

            measurements = np.fromstring(Z_profile[1:], dtype=float, sep=" ")[
                1:6
            ]  # last measurement from the MRR of level 1 to 5
            measurement = np.mean(measurements)  # mean ≈
            print(timestamp, measurements, "mean", measurement)

        except Exception as e:
            print("could not read MRR data:", e)

        dat = {
            instrument: {
                "timestamp": timestamp,
                "unit": "dBz",
                "measurement": measurement,
            }
        }

        # send data to client
        try:
            self._set_headers()
            self.wfile.write(json.dumps(dat, default=str).encode("utf-8"))
        except Exception as e:
            print("could not send response:", e)


def run(server_class=HTTPServer, handler_class=S, addr="localhost", port=8000):
    server_address = (addr, port)
    httpd = server_class(server_address, handler_class)

    print(f"Starting httpd server on {addr}:{port}")
    httpd.serve_forever()


if __name__ == "__main__":
    run(addr="", port=port)
