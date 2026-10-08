#!/usr/bin/env python3
"""Serve a built Studio browser product with worker isolation headers."""

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

    def do_GET(self):
        request = urlsplit(self.path)
        if request.path == '/':
            self.send_response(302)
            self.send_header('Location', '/chibi-studio.html' + ('?' + request.query if request.query else ''))
            self.end_headers()
            return
        super().do_GET()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--bind', default='127.0.0.1')
    args = parser.parse_args()
    directory = args.directory.resolve()
    for name in ('chibi-studio.html', 'chibi-studio.js', 'chibi-studio.wasm', 'chibi-studio.data'):
        if not (directory / name).is_file():
            parser.error(f'Browser product is missing {name} in {directory}')
    if not 1 <= args.port <= 65535:
        parser.error('Port must be between 1 and 65535')
    with ThreadingHTTPServer((args.bind, args.port), partial(Handler, directory=str(directory))) as server:
        print(f'Serving Studio at http://{args.bind}:{args.port}/chibi-studio.html', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
