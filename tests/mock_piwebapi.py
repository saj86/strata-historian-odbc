#!/usr/bin/env python3
"""Loopback mock of the PI Web API routes the driver uses, for exercising WinHTTP end to end.

SYNTHETIC. This is not PI, says nothing about a real server, and must never be cited as Gate B
evidence. Run: python tests/mock_piwebapi.py 8765
DSN: BaseUrl=http://127.0.0.1:8765/piwebapi;DataServer=SYN;Backend=piwebapi;Auth=Anonymous
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

FLAGS = {"Good": True, "Questionable": False, "Substituted": False, "Annotated": False}
EVENTS = [
    {"Timestamp": "2026-01-15T08:00:00Z", "Value": 101.5, "UnitsAbbreviation": "kPa", **FLAGS},
    {"Timestamp": "2026-01-15T08:00:10Z", "Value": 0, "UnitsAbbreviation": "kPa", **FLAGS},
    {"Timestamp": "2026-01-15T08:00:10.0000153Z", "Value": 102.25, "UnitsAbbreviation": "kPa", **FLAGS},
    {"Timestamp": "2026-01-15T08:00:10.0000153Z", "Value": 102.5, "UnitsAbbreviation": "kPa", **FLAGS},
    {"Timestamp": "2026-01-15T08:05:00Z", "Value": {"Name": "Bad Input", "Value": 307, "IsSystem": True}, "UnitsAbbreviation": "", **FLAGS, "Good": False},
]
POINT = {"WebId": "P1MOCK", "Name": "SYN.PRESSURE.PV", "Descriptor": "mock", "PointClass": "classic", "PointType": "Float64", "EngineeringUnits": "kPa", "DigitalSetName": "", "Step": False}


class Handler(BaseHTTPRequestHandler):
    requests_seen = []

    def send_json(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        path = unquote(url.path)
        print("REQUEST", self.path, flush=True)
        if path == "/piwebapi/dataservers":
            return self.send_json(200, {"WebId": "F1DSMOCK", "Name": q.get("name", ["?"])[0]})
        if path == "/piwebapi/dataservers/F1DSMOCK/points":
            start = int(q.get("startIndex", ["0"])[0])
            return self.send_json(200, {"Items": [POINT] if start == 0 else []})
        if path == "/piwebapi/points/P1MOCK":
            return self.send_json(200, POINT)
        if path == "/piwebapi/points/P1DENIED" or path == "/piwebapi/streams/P1DENIED/recorded":
            return self.send_json(403, {"Errors": ["denied"]})
        if path == "/piwebapi/streams/P1MOCK/recorded":
            start, end = q["startTime"][0], q["endTime"][0]
            limit = int(q["maxCount"][0])
            norm = lambda s: s.rstrip("Z").ljust(27, "0") if "." in s else s.rstrip("Z") + ".0000000"
            items = [e for e in EVENTS if norm(start) <= norm(e["Timestamp"]) <= norm(end)]
            return self.send_json(200, {"Items": items[:limit]})
        return self.send_json(404, {"Errors": ["not found"]})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
