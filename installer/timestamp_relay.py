"""Forward SignTool's RFC3161 request when its WinHTTP path cannot reach the TSA.

Only the network transport changes: SignTool embeds the authority's original,
cryptographically signed response. No proxy or certificate-store settings change.
"""

import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import secrets
import subprocess
import sys
import threading
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--signtool', required=True)
    parser.add_argument('file')
    args = parser.parse_args()
    endpoint = '/' + secrets.token_hex(16)
    failures = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path != endpoint:
                self.send_error(404)
                return
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 65536:
                    raise ValueError('invalid timestamp request size')
                request = urllib.request.Request(
                    'http://timestamp.digicert.com',
                    data=self.rfile.read(size),
                    headers={'Content-Type': 'application/timestamp-query'},
                )
                with urllib.request.urlopen(request, timeout=30) as reply:
                    if reply.headers.get_content_type() != 'application/timestamp-reply':
                        raise ValueError('unexpected TSA content type')
                    data = reply.read(1048577)
                    if not 0 < len(data) <= 1048576:
                        raise ValueError('invalid timestamp response size')
            except (OSError, ValueError) as error:
                failures.append(str(error))
                print(f'Online timestamp request failed: {error}', file=sys.stderr)
                self.send_error(502)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'application/timestamp-reply')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, format, *args):
            pass  # HTTP access logs add no signature-verification evidence.

    with HTTPServer(('127.0.0.1', 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = subprocess.run([
                args.signtool, 'timestamp', '/tr',
                f'http://127.0.0.1:{server.server_port}{endpoint}',
                '/td', 'sha256', args.file,
            ], timeout=90)
        finally:
            server.shutdown()
            thread.join()
    return result.returncode or int(bool(failures))


if __name__ == '__main__':
    sys.exit(main())
