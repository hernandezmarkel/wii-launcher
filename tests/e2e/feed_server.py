"""Feed de staging local para probar el updater sin GitHub (solo 127.0.0.1).

Sirve los archivos de una carpeta con la misma estructura que usa el launcher con WL_UPDATE_FEED:
  latest/update.txt, download/v<versión>/<asset>, releases.json
y además reglas de fallo en <carpeta>/rules.json, p. ej.:
  {"/download/v1.0.1/wii-launcher.exe": {"status": 404}}
  {"/latest/update.txt": {"status": 429, "headers": {"Retry-After": "60"}}}
  {"/download/v1.0.1/wii-launcher.exe": {"redirect": "https://evil.example/x"}}
  {"/download/v1.0.1/wii-launcher.exe": {"truncate": 1000}}   (anuncia el tamaño real y corta antes)
  {"/download/v1.0.1/wii-launcher.exe": {"delay_per_chunk": 0.05}}   (descarga lenta)
Uso: python feed_server.py <carpeta> <puerto>
"""
import http.server
import json
import os
import sys
import time

ROOT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2])


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        with open(os.path.join(ROOT, "server.log"), "a", encoding="utf-8") as f:
            f.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def do_GET(self):
        path = self.path.split("?")[0]
        rules = {}
        rules_file = os.path.join(ROOT, "rules.json")
        if os.path.exists(rules_file):
            with open(rules_file, encoding="utf-8") as f:
                rules = json.load(f)
        rule = rules.get(path, {})
        if "redirect" in rule:
            self.send_response(302)
            self.send_header("Location", rule["redirect"])
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if "status" in rule:
            self.send_response(rule["status"])
            for k, v in rule.get("headers", {}).items():
                self.send_header(k, v)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        local = os.path.normpath(os.path.join(ROOT, path.lstrip("/")))
        if not local.startswith(ROOT) or not os.path.isfile(local):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        with open(local, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        limit = rule.get("truncate", len(data))
        delay = rule.get("delay_per_chunk", 0)
        sent = 0
        while sent < limit:
            chunk = data[sent:min(limit, sent + 65536)]
            self.wfile.write(chunk)
            sent += len(chunk)
            if delay:
                time.sleep(delay)
        if limit < len(data):
            self.close_connection = True


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
