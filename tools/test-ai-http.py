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
group_requests = []
errors = []
lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def respond(self, payload):
        data = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            if body['model'] == 'group-review':
                system, user = [message['content'] for message in body['messages']]
                turn = json.loads(user.partition('\n本轮回复对象：')[0])
                if '随后群聊' in turn:
                    followup, = turn['随后群聊']
                    assert turn['拟回复'] == '今晚八点。'
                    answered = followup['内容'] in ('今晚八点', '八点集合')
                    assert followup['引用原问题'] == (followup['内容'] == '今晚八点')
                    assert followup['@原提问者'] == (followup['内容'] == '八点集合')
                    response = {'status': 'answered' if answered else 'keep', 'evidence': 1 if answered else 0}
                elif '同期旁听发言（不作为回复对象）' in turn:
                    answer, = turn['同期旁听发言（不作为回复对象）']
                    assert turn['当前发言'] == '今晚几点开团？' and answer['内容'] == '八点集合'
                    assert answer['@的发言对象'] == ['m1'] and '引用编号' not in answer
                    response = {'audience': 'bot', 'action': 'skip'}
                elif turn['当前发言'] == '走左边。':
                    history = turn['此前对话（从早到晚）']
                    assert history[0]['本账号发言'] and history[0]['编号'] == 'h4'
                    if '本轮回复对象：' in user:
                        assert len(history) == 1 and user.endswith('本轮回复对象：甲')
                        response = '那就走左边的小路。'
                    else:
                        assert history[1]['发言人'] == turn['当前发言人']
                        assert not history[1]['与当前发言人同一账号'] and '当前发言直接@本账号' not in turn
                        response = {'audience': 'bot', 'action': 'reply', 'target': 1, 'thread': ['h4']}
                elif '本轮回复对象：' in user:
                    assert turn['当前发言人'] == '乙' and user.endswith('本轮回复对象：乙')
                    assert turn['当前附件的识图观察（可能有误）'] == '蓝图'
                    assert turn['当前发言引用的原消息']['当时的识图观察（可能有误）'] == '红图'
                    response = '乙，蓝图画的是港口。'
                elif turn['当前发言'] == '乙，你家网络也断了吗？':
                    response = {'audience': 'others', 'action': 'reply'}
                elif turn['当前发言'] == '这是我的地图':
                    assert turn['当前发言之后的补充（从早到晚）'][0]['发言人'] == '乙'
                    response = {'audience': 'bot', 'action': 'reply', 'target': 2}
                else:
                    assert '只在讨论团务时主动接话' in system
                    assert turn['当前发言'] == '今晚几点开团？'
                    response = {'audience': 'bot', 'action': 'reply', 'target': 1}
                with lock:
                    group_requests.append(response.get('status', 'plan') if isinstance(response, dict) else 'reply')
                content = json.dumps(response) if isinstance(response, dict) else response
                self.respond({'choices': [{'finish_reason': 'stop', 'message': {'content': content}}]})
                return
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
            self.respond(payload)
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
        assert sorted(group_requests) == sorted((['answered', 'keep'] + ['plan'] * 5 + ['reply'] * 2) * 2), group_requests
        print('PASS: unmentioned follow-up with concurrent topics and same-name speakers, late KP answer during wait')
        print('PASS: persona, peer conversations, selected speaker and quoted images, resolved and unrelated follow-ups')
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
