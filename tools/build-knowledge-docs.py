"""把 Dice-Next-Doc 文档站的 Markdown 切成知识库段落，写入 server/i18n/knowledge/docs.json。

用法：python tools/build-knowledge-docs.py <Dice-Next-Doc 仓库路径>

每段带「页面 > 小节」标题路径与页面地址，正文去掉 Markdown 标记、保留表格与代码的文字。
文档更新后重新运行一次并提交生成的文件；程序启动时读取，不需要联网。
"""
import json
import os
import re
import sys

MAX_CHARS = 700          # 单段上限（字符），超出按段落再切
MIN_CHARS = 40           # 太短的小节并入上一段

# 更新日志与开发计划记录的是版本变化，不是用法说明，检索时只会干扰回答。
SKIP_PAGES = {'changelog.md', 'roadmap.md'}

SECTION_NAMES = {'guide': '入门', 'use': '使用指南', 'manage': '管理面板', 'develop': '开发', 'other': '其他'}


def clean(text: str) -> str:
    text = re.sub(r'<!--.*?-->', '', text, flags=re.S)
    text = re.sub(r'^:::\s*\w+\s*(.*)$', r'\1', text, flags=re.M)     # ::: tip 标题 → 标题
    text = re.sub(r'^:::\s*$', '', text, flags=re.M)
    text = re.sub(r'<[^>\n]+>', '', text)                               # HTML / Vue 组件标签
    text = re.sub(r'!\[[^\]]*\]\([^)]*\)', '', text)                    # 图片
    text = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', text)                # 链接 → 文字
    text = re.sub(r'^```.*$', '', text, flags=re.M)                     # 代码围栏
    text = re.sub(r'\*\*([^*]+)\*\*', r'\1', text)
    text = re.sub(r'`([^`]+)`', r'\1', text)
    text = re.sub(r'^\|?\s*:?-{2,}.*$', '', text, flags=re.M)           # 表格分隔行
    text = re.sub(r'\{#[^}]*\}', '', text)                              # 自定义锚点
    text = re.sub(r'[ \t]+\n', '\n', text)
    text = re.sub(r'\n{3,}', '\n\n', text)
    return text.strip()


def split_long(text: str):
    if len(text) <= MAX_CHARS:
        return [text]
    parts, cur = [], ''
    for para in re.split(r'\n\s*\n', text):
        if cur and len(cur) + len(para) + 2 > MAX_CHARS:
            parts.append(cur)
            cur = ''
        if len(para) > MAX_CHARS:                       # 单段过长：按行再切
            for line in para.split('\n'):
                if cur and len(cur) + len(line) + 1 > MAX_CHARS:
                    parts.append(cur)
                    cur = ''
                cur = (cur + '\n' + line) if cur else line
        else:
            cur = (cur + '\n\n' + para) if cur else para
    if cur:
        parts.append(cur)
    return parts


def chunk_page(rel: str, md: str):
    md = re.sub(r'^---\n.*?\n---\n', '', md, flags=re.S)  # frontmatter
    page_title, h2, h3 = '', '', ''
    sections, buf = [], []

    def flush():
        body = clean('\n'.join(buf))
        buf.clear()
        if not body:
            return
        path = ' > '.join(x for x in (page_title, h2, h3) if x)
        sections.append([path, body])

    in_code = False
    for line in md.split('\n'):
        if line.startswith('```'):
            in_code = not in_code
        m = None if in_code else re.match(r'^(#{1,3})\s+(.*?)\s*(\{#.*\})?\s*$', line)
        if m:
            flush()
            level, title = len(m.group(1)), clean(m.group(2))
            if level == 1:
                page_title, h2, h3 = title, '', ''
            elif level == 2:
                h2, h3 = title, ''
            else:
                h3 = title
            continue
        buf.append(line)
    flush()
    # 太短的小节并入上一段（同页）
    merged = []
    for path, body in sections:
        if merged and len(body) < MIN_CHARS:
            merged[-1][1] += '\n' + body
        else:
            merged.append([path, body])
    url = '/' + rel[:-3].replace(os.sep, '/')
    if url.endswith('/index'):
        url = url[:-5]
    out = []
    for path, body in merged:
        for part in split_long(body):
            out.append({'title': path or page_title or url, 'url': url, 'text': part})
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = os.path.join(sys.argv[1], 'src')
    chunks = []
    for root, dirs, files in os.walk(src):
        dirs[:] = sorted(d for d in dirs if d not in ('public', 'config', 'node_modules') and not d.startswith('.'))
        for name in sorted(files):
            if not name.endswith('.md') or name in SKIP_PAGES:
                continue
            full = os.path.join(root, name)
            rel = os.path.relpath(full, src)
            section = SECTION_NAMES.get(rel.split(os.sep)[0], '')
            with open(full, encoding='utf-8-sig') as f:
                for c in chunk_page(rel, f.read()):
                    if section:
                        c['section'] = section
                    chunks.append(c)
    for i, c in enumerate(chunks):
        c['id'] = 'doc' + str(i + 1)
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, '..', 'server', 'i18n', 'knowledge', 'docs.json')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'version': 1, 'source': 'Dice-Next-Doc', 'chunks': chunks}, f, ensure_ascii=False, indent=0)
        f.write('\n')
    print(f'{len(chunks)} chunks -> {os.path.normpath(out)}')


if __name__ == '__main__':
    main()
