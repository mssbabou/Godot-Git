"""A stand-in for an LFS server's file locking API (https://github.com/git-lfs/git-lfs/blob/main/docs/api/locking.md),
for test_lfs_locks.gd: git lfs lock / unlock / locks talk to it as they would to GitHub's.
Every lock made through it belongs to "you"; one lock, by "alice", exists from the start.

    python fake_lfs_lock_server.py <port file>

It listens on a free port the system picks and writes the number to <port file> once it does: a
port the test picked could be taken or reserved (Windows runners reserve ranges for Hyper-V).
"""

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

locks = {"1": {"id": "1", "path": "music.ogg", "owner": {"name": "alice"}, "locked_at": "2026-10-03T10:00:00Z"}}
next_id = [2]


class Handler(BaseHTTPRequestHandler):
    def _send(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/vnd.git-lfs+json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(length) or b"{}")

    def do_GET(self):
        url = urlparse(self.path)
        if url.path.endswith("/locks"):
            path = parse_qs(url.query).get("path", [None])[0]
            found = [lock for lock in locks.values() if path is None or lock["path"] == path]
            self._send(200, {"locks": found})
        else:
            self._send(404, {"message": "Not Found"})

    def do_POST(self):
        url = urlparse(self.path)
        body = self._body()
        if url.path.endswith("/locks/verify"):
            ours = [lock for lock in locks.values() if lock["owner"]["name"] == "you"]
            theirs = [lock for lock in locks.values() if lock["owner"]["name"] != "you"]
            self._send(200, {"ours": ours, "theirs": theirs})
        elif url.path.endswith("/locks"):
            for lock in locks.values():
                if lock["path"] == body.get("path"):
                    self._send(409, {"lock": lock, "message": "already created lock"})
                    return
            lock = {"id": str(next_id[0]), "path": body.get("path"), "owner": {"name": "you"}, "locked_at": "2026-10-03T11:00:00Z"}
            next_id[0] += 1
            locks[lock["id"]] = lock
            self._send(201, {"lock": lock})
        elif url.path.endswith("/unlock"):
            lock_id = url.path.split("/")[-2]
            lock = locks.get(lock_id)
            if lock is None:
                self._send(404, {"message": "Not Found"})
            elif lock["owner"]["name"] != "you" and not body.get("force"):
                self._send(403, {"message": "You must have admin access to force delete a lock"})
            else:
                del locks[lock_id]
                self._send(200, {"lock": lock})
        else:
            self._send(404, {"message": "Not Found"})

    def log_message(self, *args):
        pass


server = HTTPServer(("127.0.0.1", 0), Handler)
# Written whole and then renamed, so the test never reads half a number.
with open(sys.argv[1] + ".tmp", "w") as port_file:
    port_file.write(str(server.server_address[1]))
os.replace(sys.argv[1] + ".tmp", sys.argv[1])
server.serve_forever()
