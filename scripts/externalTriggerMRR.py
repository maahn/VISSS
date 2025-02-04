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
        try:
            # open MRR file
            file1 = open("C:\MRR_data\ActData\AveData.ave", "r")
            Lines = file1.readlines()
            for line in Lines:
                if line[0] == "Z":
                    Z_profile = line

            try:
                timestamp = np.datetime64("now")
                measurements = np.fromstring(Z_profile[1:], dtype=float, sep=" ")[
                    1:6
                ]  # last measurement from the MRR of level 1 to 5
                measurement = np.mean(measurements)  # mean ≈
                print(timestamp, measurements, "mean", measurement)

            except Exception as e:
                print(e)

                timestamp = np.datetime64("now")
                measurement = 9999.0

            dat = {
                instrument: {
                    "timestamp": timestamp,
                    "unit": "dBz",
                    "measurement": measurement,
                }
            }

            # send data to client
            self._set_headers()
            self.wfile.write(json.dumps(dat, default=str).encode("utf-8"))

        except Exception as e:
            print("MAJOR ERROR")
            print(e)


def run(server_class=HTTPServer, handler_class=S, addr="localhost", port=8000):
    server_address = (addr, port)
    httpd = server_class(server_address, handler_class)

    print(f"Starting httpd server on {addr}:{port}")
    httpd.serve_forever()


if __name__ == "__main__":
    run(addr="", port=port)
