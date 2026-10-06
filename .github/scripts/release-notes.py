"""Read-only release comparison; only the explicitly named output files are written.

GitHub requests are GETs, Git commands never fetch, checkout or execute commit
text, and both range ends come from the builds/published source records.
"""
import argparse
import html
import json
import re
import subprocess
import time
from datetime import datetime
from pathlib import Path
from typing import NamedTuple

REPOSITORIES = {"core": "DiceZone/Dice-Next", "webui": "DiceZone/Dice-Next-WebUI"}
LABELS = {"core": "主程序", "webui": "WebUI"}
SOURCE_ASSET = "release-sources.json"
PLATFORMS = {("windows", "amd64"), ("windows", "arm64"),
             ("linux", "amd64"), ("linux", "arm64"), ("macos", "arm64")}
SHA = re.compile(r"[0-9a-f]{40}\Z")
TAG = re.compile(r"v(\d+)\.(\d+)\.(\d+)(?:-beta\.(\d+))?\Z")
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
CATEGORIES = {"breaking": "兼容性变化", "feat": "新增功能", "fix": "问题修复",
              "perf": "性能优化", "other": "其他改进"}


class NotesError(RuntimeError):
    pass


class Commit(NamedTuple):
    sha: str
    parents: tuple
    subject: str
    body: str


class Baseline(NamedTuple):
    tag: str | None
    core: str | None
    webui: str | None
    webui_origin: str
    warnings: list


def full_sha(value):
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise NotesError("Expected a full, lowercase Git commit SHA")
    return value


def tag_order(value):
    match = TAG.fullmatch(value) if isinstance(value, str) else None
    if not match:
        raise NotesError("Unsupported Dice!Next release tag")
    major, minor, patch, beta = match.groups()
    return int(major), int(minor), int(patch), 1 if beta is None else 0, int(beta or 0)


def positive_int(value):
    return type(value) is int and value > 0


class Git:
    def __init__(self, root):
        self.root = Path(root).resolve()

    def _run(self, *arguments):
        try:
            return subprocess.run(["git", "-C", str(self.root), *arguments],
                                  capture_output=True, encoding="utf-8", errors="replace",
                                  timeout=30, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise NotesError("Cannot read local Git history") from error

    def resolve(self, revision):
        # Callers supply only validated SHA values or canonical refs/tags/v... .
        result = self._run("rev-parse", "--verify", "--end-of-options", revision + "^{commit}")
        if result.returncode:
            raise NotesError("A required source commit is absent from local Git history")
        return full_sha(result.stdout.strip())

    def is_ancestor(self, base, head):
        full_sha(base)
        full_sha(head)
        result = self._run("merge-base", "--is-ancestor", base, head)
        if result.returncode not in (0, 1):
            raise NotesError("Cannot verify the comparison's Git ancestry")
        return result.returncode == 0

    def commits(self, base, head):
        full_sha(head)
        if base is not None:
            full_sha(base)
            if not self.is_ancestor(base, head):
                raise NotesError("Refusing a comparison across unrelated Git histories")
        revision = f"{base}..{head}" if base else head
        result = self._run("log", "--reverse", "--topo-order", "--encoding=UTF-8",
                           "--format=%H%x00%P%x00%s%x00%b%x00", revision, "--")
        if result.returncode:
            raise NotesError("Cannot read the release's commit range")
        fields = result.stdout.split("\0")
        commits = []
        for index in range(0, len(fields) - 1, 4):
            sha, parents, subject, body = fields[index:index + 4]
            commits.append(Commit(full_sha(sha.strip()), tuple(parents.split()), subject, body))
        return commits


class GitHub:
    def _get(self, endpoint, accept="application/vnd.github+json"):
        command = ["gh", "api", "--hostname", "github.com", "--method", "GET",
                   "--header", "Accept:" + accept, endpoint]
        for attempt in range(3):
            try:
                result = subprocess.run(command, capture_output=True, timeout=45, check=False)
                # New gh versions protect terminal output containing ANSI. Log
                # text stays captured, stripped, never evaluated or echoed.
                if result.returncode and b"terminal escape sequences" in result.stderr:
                    command.append("--allow-escape-sequences")
                    result = subprocess.run(command, capture_output=True, timeout=45, check=False)
            except subprocess.TimeoutExpired as error:
                if attempt == 2:
                    raise NotesError("GitHub read-only request timed out after retries") from error
                time.sleep(attempt + 1)
                continue
            except OSError as error:
                raise NotesError("Cannot run the GitHub CLI") from error
            if not result.returncode:
                return result.stdout
            status = re.search(rb"HTTP (\d{3})", result.stderr)
            code = int(status.group(1)) if status else 0
            transient = code in (429, 500, 502, 503, 504) or b"unexpected EOF" in result.stderr
            if transient and attempt < 2:
                time.sleep(attempt + 1)
                continue
            detail = " HTTP " + status.group(1).decode() if status else ""
            raise NotesError("GitHub GET failed:" + detail + " " + endpoint.split("?")[0])

    def get(self, endpoint):
        try:
            return json.loads(self._get(endpoint))
        except (ValueError, UnicodeError) as error:
            raise NotesError("GitHub returned invalid JSON") from error

    def pages(self, endpoint, key=None):
        for page in range(1, 21):
            separator = "&" if "?" in endpoint else "?"
            payload = self.get(f"{endpoint}{separator}per_page=100&page={page}")
            items = payload.get(key) if key and isinstance(payload, dict) else payload
            if not isinstance(items, list):
                raise NotesError("GitHub returned an invalid paginated response")
            yield from items
            if len(items) < 100:
                return
        raise NotesError("GitHub history exceeds the bounded pagination limit")

    def asset_json(self, asset):
        if not positive_int(asset.get("id")) or not positive_int(asset.get("size")) or asset["size"] > 131072:
            raise NotesError("Invalid or oversized release metadata asset")
        raw = self._get(f"repos/{REPOSITORIES['core']}/releases/assets/{asset['id']}",
                        "application/octet-stream")
        if len(raw) > 131072:
            raise NotesError("Release metadata exceeds the size limit")
        try:
            return json.loads(raw)
        except (ValueError, UnicodeError) as error:
            raise NotesError("Release metadata is not valid JSON") from error

    def job_log(self, job_id):
        if not positive_int(job_id):
            raise NotesError("Invalid workflow job ID")
        raw = self._get(f"repos/{REPOSITORIES['core']}/actions/jobs/{job_id}/logs")
        if len(raw) > 4 * 1024 * 1024:
            raise NotesError("Legacy WebUI log exceeds the bounded size limit")
        return ANSI.sub("", raw.decode("utf-8", errors="replace"))


def complete_release(release, manifest):
    """Do not use a draft/partially uploaded release as the changelog boundary."""
    if (not isinstance(release, dict) or release.get("draft")
            or not timestamp(release.get("published_at")) or not isinstance(release.get("assets"), list)):
        return False
    if not isinstance(manifest, dict) or type(manifest.get("schema")) is not int or manifest["schema"] != 1:
        return False
    if manifest.get("repository") != REPOSITORIES["core"] or manifest.get("tag") != release.get("tag_name"):
        return False
    assets = {item.get("name"): item for item in release.get("assets", []) if isinstance(item, dict)}
    rows = manifest.get("assets")
    if not isinstance(rows, list) or len(rows) != len(PLATFORMS):
        return False
    seen = set()
    for item in rows:
        if not isinstance(item, dict):
            return False
        platform = (item.get("os"), item.get("arch"))
        name = item.get("name")
        if (not all(isinstance(value, str) for value in platform)
                or platform not in PLATFORMS or platform in seen or not isinstance(name, str)):
            return False
        uploaded = assets.get(name, {})
        if (not re.fullmatch(r"[A-Za-z0-9._-]+", name) or not positive_int(item.get("size"))
                or item["size"] != uploaded.get("size") or uploaded.get("state") != "uploaded"
                or not re.fullmatch(r"[0-9a-fA-F]{64}", str(item.get("sha256", "")))):
            return False
        seen.add(platform)
    return seen == PLATFORMS


def recorded_sources(record, tag, core):
    if (not isinstance(record, dict) or type(record.get("schema")) is not int
            or record["schema"] != 1 or record.get("tag") != tag):
        raise NotesError("Invalid release source record")
    sources = record.get("sources")
    if not isinstance(sources, dict):
        raise NotesError("Release source record has no sources")
    for name, repository in REPOSITORIES.items():
        item = sources.get(name)
        if not isinstance(item, dict) or item.get("repository") != repository:
            raise NotesError("Release source record references an unexpected repository")
        full_sha(item.get("sha"))
    if sources["core"]["sha"] != core:
        raise NotesError("Release source record disagrees with the published tag")
    return sources["webui"]["sha"]


def checkout_webui_sha(log):
    """Read only the checkout's first log -1 result after its repository marker."""
    synced = False
    awaiting_sha = False
    for line in ANSI.sub("", log).splitlines():
        text = re.sub(r"^\d{4}-\d\d-\d\dT\S+\s+", "", line).strip()
        if text.startswith("Syncing repository:"):
            synced = text == "Syncing repository: " + REPOSITORIES["webui"]
            awaiting_sha = False
        elif synced and re.search(r"\[command\].*\bgit(?:\.exe)? log -1 --format=%H$", text):
            awaiting_sha = True
        elif awaiting_sha:
            return text if SHA.fullmatch(text) else None
        elif synced and text.startswith("##[group]Run "):
            # Checkout finished without a source result; do not trust build output.
            return None
    return None


def timestamp(value):
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        return parsed if parsed.tzinfo is not None else None
    except (AttributeError, TypeError, ValueError):
        return None


def legacy_webui_source(api, release, core):
    published = timestamp(release.get("published_at"))
    endpoint = f"repos/{REPOSITORIES['core']}/actions/workflows/release.yml/runs?status=success&head_sha={core}"
    runs = list(api.pages(endpoint, "workflow_runs"))
    runs.sort(key=lambda run: run.get("run_started_at", ""), reverse=True)
    for run in runs:
        started = timestamp(run.get("run_started_at"))
        if (run.get("head_sha") != core or run.get("conclusion") != "success"
                or not positive_int(run.get("id")) or not started or not published or started > published):
            continue
        jobs = api.pages(f"repos/{REPOSITORIES['core']}/actions/runs/{run['id']}/jobs", "jobs")
        for job in jobs:
            job_started = timestamp(job.get("started_at"))
            job_completed = timestamp(job.get("completed_at"))
            if (job.get("name") == "WebUI" and job.get("conclusion") == "success"
                    and job_started and job_completed and job_started <= job_completed <= published):
                sha = checkout_webui_sha(api.job_log(job.get("id")))
                if sha:
                    return sha
    return None


def choose_baseline(api, core_git, web_git, tag, core_head, web_head):
    candidates = []
    for release in api.pages(f"repos/{REPOSITORIES['core']}/releases"):
        if not isinstance(release, dict) or release.get("draft") or not timestamp(release.get("published_at")):
            continue
        try:
            order = tag_order(release.get("tag_name"))
        except NotesError:
            continue
        if order < tag_order(tag):
            candidates.append((order, release))
    candidates.sort(key=lambda item: item[0], reverse=True)
    for _, release in candidates:
        previous_tag = release["tag_name"]
        try:
            core = core_git.resolve("refs/tags/" + previous_tag)
        except NotesError:
            continue
        if not core_git.is_ancestor(core, core_head):
            continue
        assets = {item.get("name"): item for item in release.get("assets", []) if isinstance(item, dict)}
        if "update-manifest.json" not in assets:
            continue
        # API errors must not silently choose an older release and misreport its changes.
        manifest = api.asset_json(assets["update-manifest.json"])
        if not complete_release(release, manifest):
            continue
        warnings = []
        webui = None
        origin = "unavailable"
        if SOURCE_ASSET in assets:
            # A published source record is authoritative. Do not downgrade a
            # corrupt record or transient download error to a guessed range.
            webui = recorded_sources(api.asset_json(assets[SOURCE_ASSET]), previous_tag, core)
            origin = "release-sources"
        if not webui:
            try:
                webui = legacy_webui_source(api, release, core)
                if webui:
                    origin = "workflow-checkout-log"
            except NotesError:
                pass  # Restricted/expired legacy logs are optional, not a guessed baseline.
        if webui:
            try:
                webui = web_git.resolve(full_sha(webui))
                if not web_git.is_ancestor(webui, web_head):
                    webui = None
            except NotesError:
                webui = None
        if not webui:
            origin = "unavailable"
            warnings.append("未能确认上次实际打包的 WebUI 提交，本次不猜测前端改动范围；已记录本版源码供下次准确比较。")
        return Baseline(previous_tag, core, webui, origin, warnings)
    if candidates:
        # Tags/manifests absent or histories changed: do not call this a first release.
        raise NotesError("No complete, ancestral published release is available as a reliable baseline")
    return Baseline(None, None, None, "first-release", [])


CONVENTIONAL = re.compile(r"^(feat|fix|perf|refactor|docs|test|ci|build|chore)(?:\([^\r\n)]*\))?(!)?:\s*(.+)$", re.I)
NOTE_MARKER = re.compile(r"^(?:Release-Notes|发布说明)\s*[:：]\s*(.*)$", re.I)
NOTE_PREFIX = re.compile(r"^(breaking|feat|fix|perf|other|新增|功能|修复|性能|优化|其他|兼容性变化)\s*[:：]\s*(.+)$", re.I)
NOTE_TYPES = {"breaking": "breaking", "兼容性变化": "breaking", "feat": "feat", "新增": "feat", "功能": "feat",
              "fix": "fix", "修复": "fix", "perf": "perf", "性能": "perf", "优化": "perf", "other": "other", "其他": "other"}


def explicit_notes(body):
    """Only an opt-in block is published, never arbitrary test logs/footers."""
    entries = []
    active = False
    fence = None
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith(("```", "~~~")):
            fence = None if fence == stripped[:3] else stripped[:3]
            if active:
                break
            continue
        if fence:
            continue
        marker = NOTE_MARKER.fullmatch(stripped)
        if marker:
            active = True
            if marker.group(1):
                entries.append(marker.group(1))
                break
            continue
        if not active:
            continue
        if stripped.casefold().rstrip(":") in ("end-release-notes", "release-notes-end"):
            break
        if not stripped:
            continue
        bullet = re.fullmatch(r"[-*]\s+(.+)", stripped)
        if bullet:
            entries.append(bullet.group(1))
        elif entries and line.startswith(("  ", "\t")):
            entries[-1] += " " + stripped
        else:
            break
    return entries


def highlights(commit):
    parsed = CONVENTIONAL.fullmatch(commit.subject)
    category = None
    description = commit.subject
    if parsed:
        kind, breaking, description = parsed.groups()
        category = {"feat": "feat", "fix": "fix", "perf": "perf", "refactor": "other"}.get(kind.casefold())
        if breaking:
            category = "breaking"
    else:
        category = "fix" if re.match(r"^(?:修复|修正|修正了|修復)", description) else "other"
    if re.search(r"(?m)^BREAKING[ -]CHANGE:\s*\S", commit.body):
        category = "breaking"
    notes = explicit_notes(commit.body)
    if notes:
        result = []
        for text in notes:
            prefix = NOTE_PREFIX.fullmatch(text)
            note_category = "breaking" if category == "breaking" else (
                NOTE_TYPES[prefix.group(1).casefold()] if prefix else category or "other")
            result.append((note_category, prefix.group(2) if prefix else text))
        return result
    if len(commit.parents) > 1 or category is None:
        return []
    return [(category, description)]


def markdown_text(value, limit=450):
    text = re.sub(r"[\x00-\x1f\x7f]", " ", value)
    text = " ".join(text.split())
    text = html.escape(text, quote=False)
    text = re.sub(r"([\\`*_[\]()#!|])", r"\\\1", text)
    return text if len(text) <= limit else text[:limit - 1].rstrip("\\") + "…"


def commit_link(name, commit):
    return f"[{LABELS[name]} · {commit.sha[:7]}](https://github.com/{REPOSITORIES[name]}/commit/{commit.sha})"


def compare_link(name, base, head):
    repository = REPOSITORIES[name]
    return f"https://github.com/{repository}/compare/{base}...{head}" if base else f"https://github.com/{repository}/commits/{head}"


def render_notes(tag, core_head, web_head, baseline, histories):
    tag_order(tag)
    full_sha(core_head)
    full_sha(web_head)
    lines = [f"# Dice!Next {tag}", ""]
    if baseline.tag:
        lines.extend([f"相较于 [{baseline.tag}](https://github.com/{REPOSITORIES['core']}/releases/tag/{baseline.tag})，本次包含以下改动。", ""])
    else:
        lines.extend(["首次发布，以下按实际构建的源码记录整理。", ""])
    for warning in baseline.warnings:
        lines.extend(["> " + markdown_text(warning), ""])

    grouped = {key: {} for key in CATEGORIES}
    for name, commits in histories.items():
        for commit in commits:
            for category, description in highlights(commit):
                key = " ".join(description.split()).casefold()
                item = grouped[category].setdefault(key, (description, []))
                reference = commit_link(name, commit)
                if reference not in item[1]:
                    item[1].append(reference)
    remaining = 60
    omitted = 0
    any_highlights = False
    for category, items in grouped.items():
        if not items:
            continue
        displayed = list(items.values())[:remaining]
        omitted += len(items) - len(displayed)
        if not displayed:
            continue
        any_highlights = True
        lines.extend(["## " + CATEGORIES[category], ""])
        for description, references in displayed:
            suffix = "；另见完整提交记录" if len(references) > 3 else ""
            lines.append(f"- {markdown_text(description)}（{'、'.join(references[:3])}{suffix}）")
        lines.append("")
        remaining -= len(displayed)
    if not any_highlights:
        lines.extend(["本次没有新增的功能 / 修复摘要；构建与维护提交见下方完整记录。", ""])
    if omitted:
        lines.extend([f"另有 {omitted} 项摘要，见下方完整比较链接。", ""])

    lines.extend(["## 源码与完整比较", "",
                  f"- 主程序：[`{core_head[:7]}`](https://github.com/{REPOSITORIES['core']}/commit/{core_head})",
                  f"- WebUI：[`{web_head[:7]}`](https://github.com/{REPOSITORIES['webui']}/commit/{web_head})"])
    for name, head, base in (("core", core_head, baseline.core), ("webui", web_head, baseline.webui)):
        if name in histories:
            lines.append(f"- [{LABELS[name]}完整改动]({compare_link(name, base, head)})")
    lines.extend(["", "<details>", "<summary>完整提交记录（含构建与维护）</summary>", ""])
    for name, commits in histories.items():
        lines.extend(["### " + LABELS[name], ""])
        if not commits:
            lines.extend(["本次源码未变化。", ""])
            continue
        for commit in commits[-75:]:
            lines.append(f"- {commit_link(name, commit)} {markdown_text(commit.subject, 250)}")
        if len(commits) > 75:
            lines.append(f"- 更早的 {len(commits) - 75} 条提交见该仓库的完整比较链接。")
        lines.append("")
    lines.extend(["</details>", ""])
    result = "\n".join(lines)
    if len(result) > 120000:
        raise NotesError("Generated description exceeds the bounded release body size")
    return result


def generate(api, core_git, web_git, tag, core_head, web_head):
    tag_order(tag)
    core_head = core_git.resolve(full_sha(core_head))
    web_head = web_git.resolve(full_sha(web_head))
    baseline = choose_baseline(api, core_git, web_git, tag, core_head, web_head)
    histories = {"core": core_git.commits(baseline.core, core_head)}
    if baseline.webui is not None or baseline.tag is None:
        histories["webui"] = web_git.commits(baseline.webui, web_head)
    notes = render_notes(tag, core_head, web_head, baseline, histories)
    sources = {"schema": 1, "tag": tag,
               "sources": {"core": {"repository": REPOSITORIES["core"], "sha": core_head},
                           "webui": {"repository": REPOSITORIES["webui"], "sha": web_head}},
               "comparison": {"previous_tag": baseline.tag, "core_base": baseline.core,
                              "webui_base": baseline.webui, "webui_base_origin": baseline.webui_origin}}
    return notes, sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-path", type=Path, required=True)
    parser.add_argument("--core-sha", required=True)
    parser.add_argument("--webui-path", type=Path, required=True)
    parser.add_argument("--webui-sha", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sources-output", type=Path, required=True)
    args = parser.parse_args()
    try:
        notes, sources = generate(GitHub(), Git(args.core_path), Git(args.webui_path),
                                  args.tag, args.core_sha, args.webui_sha)
        args.output.write_text(notes, encoding="utf-8", newline="\n")
        args.sources_output.write_text(json.dumps(sources, ensure_ascii=False, indent=2) + "\n",
                                       encoding="utf-8", newline="\n")
    except (NotesError, OSError) as error:
        parser.exit(1, "Release notes error: " + str(error) + "\n")
    print("Release notes generated; previous=" + str(sources["comparison"]["previous_tag"])
          + ", webui_baseline=" + sources["comparison"]["webui_base_origin"])


if __name__ == "__main__":
    main()
