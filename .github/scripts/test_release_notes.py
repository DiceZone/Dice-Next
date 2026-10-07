"""Offline regression fixtures; never publish a release or mutate real Git refs."""
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("release_notes", Path(__file__).with_name("release-notes.py"))
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)
A, B, C, D, E = (character * 40 for character in "abcde")
PREVIOUS = "v3.0.0-beta.926"
CURRENT = "v3.0.0-beta.927"


def commit(subject, body="", sha=B, parents=(A,)):
    return notes.Commit(sha, parents, subject, body)


def release(tag=PREVIOUS, draft=False):
    assets = [{"id": 1, "name": "update-manifest.json", "size": 100, "state": "uploaded"}]
    rows = []
    for index, (os, arch) in enumerate(sorted(notes.PLATFORMS), 10):
        name = f"DiceNext-{os}-{arch}.zip"
        assets.append({"id": index, "name": name, "size": 1000, "state": "uploaded"})
        rows.append({"os": os, "arch": arch, "name": name, "size": 1000, "sha256": "f" * 64})
    return ({"tag_name": tag, "draft": draft, "published_at": "2026-10-05T06:20:54Z", "assets": assets},
            {"schema": 1, "repository": notes.REPOSITORIES["core"], "tag": tag, "assets": rows})


def sources(tag=PREVIOUS, core=A, webui=C):
    return {"schema": 1, "tag": tag, "sources": {
        "core": {"repository": notes.REPOSITORIES["core"], "sha": core},
        "webui": {"repository": notes.REPOSITORIES["webui"], "sha": webui}}}


def checkout_log(sha=C):
    return (f"2026-10-05T06:07:44Z Runner image commit: {E}\n"
            "2026-10-05T06:07:45Z Syncing repository: DiceZone/Dice-Next-WebUI\n"
            "2026-10-05T06:07:46Z [command]/usr/bin/git log -1 --format=%H\n"
            f"2026-10-05T06:07:46Z {sha}\n"
            "2026-10-05T06:07:47Z ##[group]Run actions/setup-node@v4\n")


class FakeGit:
    def __init__(self, tags=None, rejected=None, history=None):
        self.tags = tags or {"refs/tags/" + PREVIOUS: A}
        self.rejected = rejected or set()
        self.history = history or []
        self.ranges = []

    def resolve(self, revision):
        if revision in self.tags:
            return self.tags[revision]
        if notes.SHA.fullmatch(revision):
            return revision
        raise notes.NotesError("missing tag")

    def is_ancestor(self, base, head):
        return (base, head) not in self.rejected

    def commits(self, base, head):
        self.ranges.append((base, head))
        return self.history


class FakeGitHub:
    def __init__(self, with_record=True):
        previous, manifest = release()
        if with_record:
            previous["assets"].append({"id": 2, "name": notes.SOURCE_ASSET, "size": 100, "state": "uploaded"})
        self.releases = [previous]
        self.assets = {1: manifest, 2: sources()}
        self.runs = [{"id": 30, "head_sha": A, "conclusion": "success", "run_started_at": "2026-10-05T06:07:00Z"}]
        self.jobs = [{"id": 31, "name": "WebUI", "conclusion": "success",
                      "started_at": "2026-10-05T06:07:00Z", "completed_at": "2026-10-05T06:08:20Z"}]
        self.logs = {31: checkout_log()}
        self.calls = []

    def pages(self, endpoint, key=None):
        self.calls.append((endpoint, key))
        return iter(self.runs if key == "workflow_runs" else self.jobs if key == "jobs" else self.releases)

    def asset_json(self, asset):
        self.calls.append(("asset", asset["id"]))
        result = self.assets[asset["id"]]
        if isinstance(result, Exception):
            raise result
        return result

    def job_log(self, job_id):
        self.calls.append(("log", job_id))
        result = self.logs[job_id]
        if isinstance(result, Exception):
            raise result
        return result


class ClassificationTests(unittest.TestCase):
    def test_body_omits_duplicate_release_title_for_initial_and_subsequent_releases(self):
        for baseline, introduction in (
                (notes.Baseline(PREVIOUS, A, C, "release-sources", []), "相较于 [" + PREVIOUS + "]"),
                (notes.Baseline(None, None, None, "first-release", []), "首次发布，")):
            with self.subTest(previous_tag=baseline.tag):
                result = notes.render_notes(CURRENT, B, D, baseline, {
                    "core": [commit("feat: 一条功能说明")]})
                self.assertTrue(result.startswith(introduction))
                self.assertNotRegex(result, r"(?m)^# ")
                self.assertNotIn("# Dice!Next " + CURRENT, result)
                self.assertIn("## 新增功能\n", result)
                self.assertIn("## 源码与完整比较\n", result)

    def test_feature_fix_performance_and_untyped_changes(self):
        for subject, category, title in [("feat(core): 定时更新", "feat", "定时更新"),
                                         ("fix(webui): 修复弹窗", "fix", "修复弹窗"),
                                         ("perf: 提速", "perf", "提速"),
                                         ("refactor(core): 简化规则", "other", "简化规则"),
                                         ("修正每日概览", "fix", "修正每日概览"),
                                         ("新增未分类功能", "other", "新增未分类功能")]:
            self.assertEqual(notes.highlights(commit(subject)), [(category, title)])

    def test_internal_changes_and_merge_boilerplate_stay_out_of_highlights(self):
        for kind in ("chore", "docs", "test", "ci", "build"):
            self.assertEqual(notes.highlights(commit(f"{kind}: record successful build 926")), [])
        self.assertEqual(notes.highlights(commit("Merge branch 'feature'", parents=(A, C))), [])

    def test_explicit_block_wins_without_publishing_tests_or_email_trailers(self):
        body = ("内部说明\n\nRelease-Notes:\n- 新增：凌晨四点更新\n- fix: 恢复会话\n"
                "End-Release-Notes\n\nTested: 999 assertions\nCo-authored-by: fixture@example.invalid")
        self.assertEqual(notes.highlights(commit("feat: broad title", body)),
                         [("feat", "凌晨四点更新"), ("fix", "恢复会话")])
        self.assertEqual(notes.highlights(commit("ci: packaging", "Release-Notes: 修复：安装包缺失")),
                         [("fix", "安装包缺失")])

    def test_bullet_block_continuations_stop_at_unrelated_text_and_ignore_fences(self):
        body = "```text\nRelease-Notes: 不应发布\n```\nRelease-Notes:\n- 一条说明\n  补充细节\n\nTested: ok\n- 测试输出"
        self.assertEqual(notes.explicit_notes(body), ["一条说明 补充细节"])
        self.assertEqual(notes.explicit_notes("发布说明：单行摘要\nFixes #48"), ["单行摘要"])

    def test_breaking_change_is_visibly_classified(self):
        self.assertEqual(notes.highlights(commit("feat(core)!: 新配置格式")), [("breaking", "新配置格式")])
        self.assertEqual(notes.highlights(commit("fix: 新设置", "BREAKING CHANGE: 移除旧字段")), [("breaking", "新设置")])
        self.assertEqual(notes.highlights(commit("feat!: 新设置", "Release-Notes: feat: 改变旧默认值")),
                         [("breaking", "改变旧默认值")])

    def test_summary_duplicates_merge_links_but_full_records_keep_every_commit(self):
        baseline = notes.Baseline(PREVIOUS, A, C, "release-sources", [])
        result = notes.render_notes(CURRENT, B, D, baseline, {
            "core": [commit("feat: 一致的功能"), commit("chore: record successful build 926", sha=E)],
            "webui": [commit("feat: 一致的功能", sha=D)]})
        self.assertEqual(result.count("- 一致的功能（"), 1)
        self.assertIn("主程序 · bbbbbbb", result)
        self.assertIn("WebUI · ddddddd", result)
        self.assertIn("record successful build 926", result.split("<details>")[1])
        self.assertNotIn("record successful build 926", result.split("<details>")[0])

    def test_commit_text_cannot_inject_html_links_or_workflow_commands(self):
        baseline = notes.Baseline(PREVIOUS, A, C, "release-sources", [])
        result = notes.render_notes(CURRENT, B, D, baseline, {"core": [commit("feat: <img src=x> [click](evil)\n::error::test")]})
        self.assertNotIn("<img", result)
        self.assertIn("&lt;img", result)
        self.assertIn(r"\[click\]\(evil\)", result)
        self.assertNotIn("\n::error::", result)

    def test_large_notes_are_bounded_and_link_to_unabridged_ranges(self):
        histories = {name: [commit("feat: " + str(index) + "界" * 1000, sha=f"{index + 1:040x}")
                            for index in range(100)] for name in ("core", "webui")}
        result = notes.render_notes(CURRENT, B, D, notes.Baseline(PREVIOUS, A, C, "release-sources", []), histories)
        self.assertLess(len(result), 120000)
        self.assertIn("另有 40 项摘要", result)
        self.assertIn("更早的 25 条提交", result)
        self.assertTrue(result.endswith("</details>\n"))


class SourceBoundaryTests(unittest.TestCase):
    def test_tag_validation_and_beta_order_do_not_depend_on_latest_stable_api(self):
        self.assertLess(notes.tag_order(PREVIOUS), notes.tag_order(CURRENT))
        self.assertLess(notes.tag_order(CURRENT), notes.tag_order("v3.0.0"))
        for tag in ("main", "v3.0.0-beta.927; echo bad", "v3.0.0-beta.927\n", None):
            with self.assertRaises(notes.NotesError):
                notes.tag_order(tag)
        for sha in ("main", "a" * 39, "A" * 40, "a" * 40 + "\n"):
            with self.assertRaises(notes.NotesError):
                notes.full_sha(sha)

    def test_source_record_requires_exact_tag_core_repository_and_full_shas(self):
        self.assertEqual(notes.recorded_sources(sources(), PREVIOUS, A), C)
        for mutation in (lambda row: row.update(schema=True), lambda row: row.update(tag=CURRENT),
                         lambda row: row["sources"]["core"].update(sha=B),
                         lambda row: row["sources"]["webui"].update(repository="unexpected/repository"),
                         lambda row: row["sources"]["webui"].update(sha="main")):
            row = sources()
            mutation(row)
            with self.assertRaises(notes.NotesError):
                notes.recorded_sources(row, PREVIOUS, A)

    def test_complete_release_requires_all_five_uploaded_packages_and_exact_sizes(self):
        published, manifest = release()
        self.assertTrue(notes.complete_release(published, manifest))
        published["assets"].pop()
        self.assertFalse(notes.complete_release(published, manifest))
        published, manifest = release()
        published["assets"][1]["state"] = "starter"
        self.assertFalse(notes.complete_release(published, manifest))
        published, manifest = release()
        manifest["assets"][0]["size"] = 1
        self.assertFalse(notes.complete_release(published, manifest))
        published, manifest = release()
        manifest["assets"][1] = dict(manifest["assets"][0])
        self.assertFalse(notes.complete_release(published, manifest))
        published, manifest = release(draft=True)
        self.assertFalse(notes.complete_release(published, manifest))
        published, manifest = release()
        manifest["assets"][0]["os"] = []
        self.assertFalse(notes.complete_release(published, manifest))

    def test_records_are_preferred_and_current_release_drafts_and_future_tags_are_ignored(self):
        api = FakeGitHub()
        api.releases.extend([release(CURRENT)[0], release("v3.0.0-beta.928")[0], release("v3.0.0-beta.999", True)[0]])
        baseline = notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D)
        self.assertEqual(baseline, notes.Baseline(PREVIOUS, A, C, "release-sources", []))
        self.assertFalse(any(key == "workflow_runs" for _, key in api.calls))

    def test_first_adoption_restores_actual_checkout_not_an_unrelated_log_sha(self):
        api = FakeGitHub(with_record=False)
        baseline = notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D)
        self.assertEqual(baseline.webui, C)
        self.assertEqual(baseline.webui_origin, "workflow-checkout-log")
        self.assertEqual(notes.checkout_webui_sha(checkout_log().replace("log -1", "log -2")), None)
        self.assertEqual(notes.checkout_webui_sha(checkout_log().replace("DiceZone/Dice-Next-WebUI", "other/repo")), None)
        self.assertEqual(notes.checkout_webui_sha(checkout_log().replace(C, "not a SHA")), None)
        spoof = "Syncing repository: DiceZone/Dice-Next-WebUI\n##[group]Run npm build\n[command]/usr/bin/git log -1 --format=%H\n" + E
        self.assertEqual(notes.checkout_webui_sha(spoof), None)

    def test_failed_or_later_rerun_cannot_be_used_as_the_previous_webui_build(self):
        api = FakeGitHub(with_record=False)
        api.runs[0].update(run_started_at="2026-10-06T06:07:00Z")
        self.assertIsNone(notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D).webui)
        api.runs[0].update(run_started_at="2026-10-05T06:07:00Z", conclusion="failure")
        self.assertIsNone(notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D).webui)
        # A rerun can retain the run's original/queued timestamp. The actual
        # WebUI job must have finished before the release was published too.
        api.runs[0]["conclusion"] = "success"
        api.jobs[0].update(started_at="2026-10-06T06:07:00Z", completed_at="2026-10-06T06:08:20Z")
        self.assertIsNone(notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D).webui)

    def test_expired_legacy_logs_report_unknown_without_dumping_all_webui_history(self):
        api = FakeGitHub(with_record=False)
        api.logs[31] = notes.NotesError("expired")
        core = FakeGit(history=[commit("feat: 主程序更新")])
        web = FakeGit(history=[commit("feat: 不应猜测旧功能", sha=D)])
        description, record = notes.generate(api, core, web, CURRENT, B, D)
        self.assertIn("未能确认", description)
        self.assertNotIn("不应猜测旧功能", description)
        self.assertEqual(web.ranges, [])
        self.assertEqual(record["sources"]["webui"]["sha"], D)
        self.assertEqual(record["comparison"]["webui_base_origin"], "unavailable")

    def test_forked_frontend_history_is_not_compared_as_new_changes(self):
        baseline = notes.choose_baseline(FakeGitHub(), FakeGit(), FakeGit(rejected={(C, D)}), CURRENT, B, D)
        self.assertIsNone(baseline.webui)
        self.assertTrue(baseline.warnings)

    def test_manifest_access_errors_do_not_silently_choose_a_wrong_older_baseline(self):
        api = FakeGitHub()
        api.assets[1] = notes.NotesError("HTTP 503")
        with self.assertRaises(notes.NotesError):
            notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D)

    def test_invalid_authoritative_source_record_fails_instead_of_using_legacy_logs(self):
        api = FakeGitHub()
        api.assets[2] = sources(core=E)
        with self.assertRaises(notes.NotesError):
            notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D)
        self.assertFalse(any(key == "workflow_runs" for _, key in api.calls))

    def test_rerunning_the_same_tag_replaces_with_identical_content_not_appended_notes(self):
        api = FakeGitHub()
        api.releases.append(release(CURRENT)[0])
        core = FakeGit(history=[commit("fix: 修复问题")])
        web = FakeGit(history=[commit("feat: 新面板", sha=D)])
        first = notes.generate(api, core, web, CURRENT, B, D)
        second = notes.generate(api, core, web, CURRENT, B, D)
        self.assertEqual(first, second)
        self.assertEqual(first[0].count("- 修复问题（"), 1)
        self.assertEqual(first[1]["comparison"]["previous_tag"], PREVIOUS)

    def test_true_first_release_and_unusable_old_releases_are_distinguished(self):
        api = FakeGitHub()
        api.releases = []
        result = notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D)
        self.assertIsNone(result.tag)
        self.assertEqual(result.webui_origin, "first-release")
        api = FakeGitHub()
        api.releases[0]["assets"].pop(1)
        with self.assertRaises(notes.NotesError):
            notes.choose_baseline(api, FakeGit(), FakeGit(), CURRENT, B, D)

    def test_api_client_only_uses_get_and_pagination_does_not_lose_a_second_page(self):
        api = notes.GitHub()
        responses = [list(range(100)), [100]]
        with patch.object(api, "get", side_effect=responses) as getter:
            self.assertEqual(list(api.pages("repos/DiceZone/Dice-Next/releases")), list(range(101)))
            self.assertIn("page=2", getter.call_args_list[1].args[0])
        with patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"[]", b"")) as runner:
            self.assertEqual(api.get("repos/DiceZone/Dice-Next/releases"), [])
            command = runner.call_args.args[0]
            self.assertEqual(command[command.index("--method") + 1], "GET")
            self.assertNotIn("POST", command)

    def test_log_escape_retry_stays_captured_and_metadata_downloads_are_bounded(self):
        api = notes.GitHub()
        blocked = subprocess.CompletedProcess([], 1, b"", b"the response contains terminal escape sequences")
        raw = subprocess.CompletedProcess([], 0, b"\x1b[31m" + checkout_log().encode(), b"")
        with patch.object(subprocess, "run", side_effect=[blocked, raw]) as runner:
            self.assertEqual(notes.checkout_webui_sha(api.job_log(31)), C)
            self.assertIn("--allow-escape-sequences", runner.call_args.args[0])
            self.assertTrue(runner.call_args.kwargs["capture_output"])
        with self.assertRaises(notes.NotesError):
            api.asset_json({"id": 1, "size": 131073})

    def test_transient_github_reads_retry_without_changing_method_or_hiding_auth_errors(self):
        api = notes.GitHub()
        unavailable = subprocess.CompletedProcess([], 1, b"", b"gh: service unavailable (HTTP 503)")
        success = subprocess.CompletedProcess([], 0, b"[]", b"")
        with patch.object(subprocess, "run", side_effect=[unavailable, success]) as runner, patch.object(notes.time, "sleep") as sleeper:
            self.assertEqual(api.get("repos/DiceZone/Dice-Next/releases"), [])
            self.assertEqual(runner.call_count, 2)
            sleeper.assert_called_once_with(1)
            self.assertTrue(all(call.args[0][call.args[0].index("--method") + 1] == "GET" for call in runner.call_args_list))
        denied = subprocess.CompletedProcess([], 1, b"", b"gh: forbidden (HTTP 403)")
        with patch.object(subprocess, "run", return_value=denied) as runner:
            with self.assertRaises(notes.NotesError):
                api.get("repos/DiceZone/Dice-Next/releases")
            self.assertEqual(runner.call_count, 1)


class RealGitTests(unittest.TestCase):
    def test_git_range_uses_build_sha_not_current_head_and_preserves_unicode_bodies(self):
        with tempfile.TemporaryDirectory(prefix="dicenext-release-notes-") as tmp:
            root = Path(tmp)
            # Synthetic commits exist only in this new disposable repository;
            # no global identity, signing, hooks or real workspace refs change.
            def git(*arguments):
                return subprocess.run(["git", "-C", str(root), "-c", "user.name=Release fixture",
                                       "-c", "user.email=release-fixture@example.invalid", "-c", "commit.gpgSign=false",
                                       "-c", "core.hooksPath=" + str(root / "no-hooks"), *arguments], check=True,
                                      capture_output=True, encoding="utf-8", errors="replace").stdout.strip()
            git("init", "--quiet")
            git("commit", "--quiet", "--allow-empty", "-m", "feat: 旧功能")
            base = git("rev-parse", "HEAD")
            git("commit", "--quiet", "--allow-empty", "-m", "feat(core): 中文更新", "-m",
                "Release-Notes:\n- 新增：支持嵌套 {sample:甲|乙}\nEnd-Release-Notes")
            built = git("rev-parse", "HEAD")
            git("commit", "--quiet", "--allow-empty", "-m", "fix: 构建期间后来提交的改动")
            repository = notes.Git(root)
            collected = repository.commits(base, built)
            self.assertEqual(len(collected), 1)
            self.assertEqual(collected[0].sha, built)
            self.assertEqual(notes.highlights(collected[0]), [("feat", "支持嵌套 {sample:甲|乙}")])
            self.assertEqual(repository.commits(built, built), [])
            self.assertEqual(len(repository.commits(None, built)), 2)
            with self.assertRaises(notes.NotesError):
                repository.commits(built, base)


class WorkflowWiringTests(unittest.TestCase):
    def test_actual_frontend_sha_is_shared_and_notes_are_generated_before_counter_rebase(self):
        workflow = (Path(__file__).resolve().parents[1] / "workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn("source_sha: ${{ steps.source.outputs.sha }}", workflow)
        self.assertIn("ref: ${{ needs.webui.outputs.source_sha }}", workflow)
        self.assertIn("needs: [webui, linux, windows, macos-arm64, backend-tests]", workflow)
        self.assertIn("CORE_SHA: ${{ github.sha }}", workflow)
        self.assertIn('git show "$CORE_SHA":.github/scripts/release-notes.py', workflow)
        self.assertIn("actions: read", workflow)
        self.assertLess(workflow.index("Generate categorized release notes"), workflow.index("Record successful build number"))
        self.assertIn("body_path: release-notes.md", workflow)
        self.assertIn("generate_release_notes: false", workflow)
        self.assertTrue(workflow.rstrip().endswith("release-sources.json"))
        self.assertIn("Test Release automation", workflow)

    def test_release_notes_bash_step_has_valid_syntax(self):
        workflow = (Path(__file__).resolve().parents[1] / "workflows/release.yml").read_text(encoding="utf-8")
        step = workflow.split("      - name: Generate categorized release notes", 1)[1].split("      - name:", 1)[0]
        script = "\n".join(line[10:] for line in step.split("        run: |\n", 1)[1].splitlines())
        git_bash = Path("C:/Program Files/Git/bin/bash.exe")
        bash = str(git_bash) if git_bash.is_file() else shutil.which("bash")
        if not bash:
            self.skipTest("Bash is unavailable on this test host")
        result = subprocess.run([bash, "-n"], input=script, capture_output=True,
                                encoding="utf-8", timeout=15, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
