import json
from http.server import BaseHTTPRequestHandler, HTTPServer

SAMPLE = {
    "is_available": True,
    "balance_infos": [
        {"currency": "CNY", "total_balance": "44.10", "granted_balance": "0.00", "topped_up_balance": "44.10"},
        {"currency": "USD", "total_balance": "2.24", "granted_balance": "0.00", "topped_up_balance": "2.24"},
    ],
}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/user/balance"):
            body = json.dumps(SAMPLE).encode()
            self.send_response(200)
        elif self.path.startswith("/unauth"):
            body = b'{"error":"bad key"}'
            self.send_response(401)
        elif self.path.startswith("/boom"):
            body = b'{"error":"server"}'
            self.send_response(500)
        else:
            body = b'not found'
            self.send_response(404)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print("mock:", fmt % args, flush=True)


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", 8765), Handler).serve_forever()
