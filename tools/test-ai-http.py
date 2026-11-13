"""Local HTTP integration: python tools/test-ai-http.py <dice-next-tests executable>.

No real provider, account, API key, bot listener or external image is used.
Two test processes share the system temp directory to exercise request isolation.
"""
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

requests = []
errors = []
lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            parts = body['messages'][0]['content']
            tag = parts[0]['text']
            assert tag in ('process-one', 'process-two'), tag
            assert body['thinking'] == {'type': 'disabled'}, body
            assert body['max_tokens'] == 4096, body['max_tokens']
            image = parts[1]['image_url']['url']
            assert image.startswith('data:image/png;base64,')
            assert base64.b64decode(image.split(',', 1)[1]) == b'\x89PNG\r\n\x1a\n' + tag.encode()
            model = body['model']
            assert model in ('empty', 'complete')
            with lock:
                requests.append((tag, model))
            time.sleep(0.1)
            empty = model == 'empty'
            payload = {'choices': [{'finish_reason': 'length' if empty else 'stop', 'message': {
                'content': '' if empty else 'image-' + tag, 'reasoning_content': 'unfinished' if empty else ''}}],
                'usage': {'prompt_tokens': 100, 'completion_tokens': 300 if empty else 40}}
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except Exception as exc:
            with lock:
                errors.append(str(exc))
            self.send_error(500)


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    children = []
    try:
        for tag in ('process-one', 'process-two'):
            env = dict(os.environ, DICENEXT_TEST_FILTER='AiHttpIntegration',
                       DICENEXT_AI_TEST_ENDPOINT=f'http://127.0.0.1:{server.server_port}', DICENEXT_AI_TEST_TAG=tag)
            children.append(subprocess.Popen([str(executable)], cwd=executable.parent, env=env,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT))
        for child in children:
            output, _ = child.communicate(timeout=60)
            assert child.returncode == 0, output.decode('utf-8', errors='replace')
        assert not errors, errors
        for tag in ('process-one', 'process-two'):
            assert [model for owner, model in requests if owner == tag] == ['empty', 'complete', 'empty', 'complete'], requests
        print('PASS: concurrent processes, Base64 identity, request options, truncated-output fallback, billed failures, cache reuse/invalidation')
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
                child.wait()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
