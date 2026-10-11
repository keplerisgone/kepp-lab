import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


SOURCE_BINARY = Path(__file__).with_name("worker")


class WorkerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / "kepp-lab"
        self.bin = self.base / "bin"
        self.log = self.base / "log"
        (self.root / "worker").mkdir(parents=True)
        self.bin.mkdir()
        self.log.mkdir()
        shutil.copy2(SOURCE_BINARY, self.root / "worker" / "worker")
        (self.root / "unchanged.txt").write_text("original\n")
        self._script("gh", r'''#!/bin/sh
printf '%s|%s\n' "$PWD" "$*" >> "$WORKER_LOG/gh-calls"
case "$1 $2" in
  "auth status") printf '%s' "${GH_AUTH_ERROR:-}" >&2; exit "${GH_AUTH_EXIT:-0}" ;;
  "issue list") printf '%s' "$GH_RESPONSE"; exit "${GH_ISSUE_EXIT:-0}" ;;
  "pr list")
    count_file="$WORKER_LOG/pr-list-count"
    count=0
    [ -f "$count_file" ] && count=$(cat "$count_file")
    count=$((count + 1))
    printf '%s' "$count" > "$count_file"
    if [ "$count" -gt 1 ] && [ -n "${GH_PR_LIST_SECOND+x}" ]; then
      printf '%s' "$GH_PR_LIST_SECOND"
    else
      printf '%s' "${GH_PR_LIST:-[]}"
    fi
    exit "${GH_PR_LIST_EXIT:-0}" ;;
  "repo view") printf '%s\n' "${GH_DEFAULT_BRANCH:-main}"; exit "${GH_REPO_EXIT:-0}" ;;
  "pr create") printf '%s\n' "${GH_PR_URL:-https://github.test/pr/1}"; exit "${GH_PR_CREATE_EXIT:-0}" ;;
esac
exit 2
''')
        self._script("git", r'''#!/bin/sh
printf '%s|%s\n' "$PWD" "$*" >> "$WORKER_LOG/git-calls"
case "$1 $2" in
  "remote get-url")
    if [ "$PWD" != "$WORKER_ROOT" ] && [ -n "${GIT_WORKTREE_REMOTE:-}" ]; then
      printf '%s\n' "$GIT_WORKTREE_REMOTE"
    else
      printf '%s\n' "${GIT_REMOTE:-git@github.com:keplerisgone/kepp-lab.git}"
    fi
    exit "${GIT_REMOTE_EXIT:-0}" ;;
  "show-ref --verify") exit "${GIT_BRANCH_EXIT:-1}" ;;
  "ls-remote --exit-code")
    [ -n "${GIT_REMOTE_HEAD:-}" ] && printf '%s\trefs/heads/%s\n' "$GIT_REMOTE_HEAD" "${4#refs/heads/}"
    exit "${GIT_REMOTE_BRANCH_EXIT:-2}" ;;
  "worktree list")
    if [ -n "${GIT_WORKTREE_LIST+x}" ]; then
      printf '%s' "$GIT_WORKTREE_LIST"
    elif [ -f "$WORKER_LOG/worktree-registration" ]; then
      IFS='|' read -r registered_path registered_branch < "$WORKER_LOG/worktree-registration"
      printf 'worktree %s\nHEAD %s\nbranch refs/heads/%s\n\n' "$registered_path" "${GIT_HEAD:-abc123}" "$registered_branch"
    fi
    exit "${GIT_WORKTREE_LIST_EXIT:-0}" ;;
  "worktree add")
    if [ "${GIT_WORKTREE_EXIT:-0}" -eq 0 ]; then
      mkdir -p "$5/worker"
      printf 'test:\n\t@true\n' > "$5/worker/Makefile"
      printf 'test:\n\t@true\n' > "$5/Makefile"
      printf '%s|%s\n' "$5" "$4" > "$WORKER_LOG/worktree-registration"
    fi
    exit "${GIT_WORKTREE_EXIT:-0}" ;;
  "rev-parse HEAD")
    if [ -f "$PWD/.fake-head" ]; then cat "$PWD/.fake-head"; exit 0; fi
    count_file="$WORKER_LOG/rev-count"
    count=0
    [ -f "$count_file" ] && count=$(cat "$count_file")
    count=$((count + 1)); printf '%s' "$count" > "$count_file"
    if [ "$count" -gt 1 ] && [ -n "${GIT_HEAD_AFTER:-}" ]; then printf '%s\n' "$GIT_HEAD_AFTER"; else printf '%s\n' "${GIT_HEAD:-abc123}"; fi
    exit "${GIT_REV_EXIT:-0}" ;;
  "status --porcelain=v1")
    case "${GIT_STATUS_KIND:-none}" in
      changed) printf ' M worker/worker.cpp\0' ;;
      untracked) printf '?? new.txt\0' ;;
      sensitive) printf '?? .env\0' ;;
      rename) printf 'R  new.txt\0old.txt\0' ;;
    esac
    exit "${GIT_STATUS_EXIT:-0}" ;;
  "add --") exit "${GIT_ADD_EXIT:-0}" ;;
  "diff --cached")
    [ "$3" = "--check" ] && exit "${GIT_CHECK_EXIT:-0}"
    [ "$3" = "--quiet" ] && exit "${GIT_STAGED_EXIT:-1}"
    exit 2 ;;
  "commit -m")
    if [ "${GIT_COMMIT_EXIT:-0}" -eq 0 ]; then printf '%s\n' "${GIT_COMMIT_HEAD:-def456}" > "$PWD/.fake-head"; fi
    exit "${GIT_COMMIT_EXIT:-0}" ;;
  "push --set-upstream") exit "${GIT_PUSH_EXIT:-0}" ;;
esac
exit 2
''')
        self._script("codex", r'''#!/bin/sh
printf 'run\n' >> "$WORKER_LOG/codex-calls"
printf '%s\n' "$@" > "$WORKER_LOG/codex-args"
printf '%s' "$PWD" > "$WORKER_LOG/codex-cwd"
cat > "$WORKER_LOG/codex-prompt"
[ -n "${CODEX_SLEEP:-}" ] && sleep "$CODEX_SLEEP"
printf '%s' "${CODEX_OUTPUT:-}"
printf '%s' "${CODEX_ERROR:-}" >&2
if [ "${CODEX_SIGNAL:-0}" -ne 0 ]; then kill -"$CODEX_SIGNAL" $$; fi
exit "${CODEX_EXIT:-0}"
''')
        self._script("make", r'''#!/bin/sh
printf '%s|%s\n' "$PWD" "$*" >> "$WORKER_LOG/make-calls"
printf '%s' "${MAKE_OUTPUT:-}"
printf '%s' "${MAKE_ERROR:-}" >&2
exit "${MAKE_EXIT:-0}"
''')

    def tearDown(self):
        self.temporary.cleanup()

    def _script(self, name, contents):
        path = self.bin / name
        path.write_text(contents)
        path.chmod(0o755)

    def run_worker(self, repository="keplerisgone/kepp-lab", use_path=True, extra_args=None, **values):
        environment = os.environ.copy()
        environment.update({
            "PATH": str(self.bin) + os.pathsep + environment["PATH"],
            "WORKER_LOG": str(self.log),
            "WORKER_ROOT": str(self.root),
            "GH_RESPONSE": "[]",
        })
        environment.update(values)
        arguments = [self.root / "worker" / "worker", "--repo", repository]
        if use_path:
            arguments += ["--path", self.root]
        if extra_args:
            arguments += extra_args
        return subprocess.run(arguments, text=True, capture_output=True, env=environment, check=False)

    def issue(self, number=42):
        return json.dumps([{"number": number, "title": "Fix worker", "body": "Keep it small."}])

    def calls(self, name):
        path = self.log / name
        return path.read_text() if path.exists() else ""

    def successful(self, **values):
        return self.run_worker(GH_RESPONSE=self.issue(), GIT_STATUS_KIND="changed", **values)

    def resume(self, **values):
        return self.run_worker(GH_RESPONSE=self.issue(), GIT_STATUS_KIND="changed",
                               extra_args=["--resume"], **values)

    def test_missing_repository_prints_usage_without_guessing(self):
        result = subprocess.run([self.root / "worker" / "worker"], text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Usage: worker --repo", result.stderr)

    def test_registered_path_supports_repo_only_interface(self):
        result = self.run_worker(use_path=False, KEPP_LAB_PATH=str(self.root))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--repo keplerisgone/kepp-lab", self.calls("gh-calls"))

    def test_no_issue_is_successful_no_work(self):
        result = self.run_worker()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("No approved issues", result.stdout)
        self.assertNotIn("worktree add", self.calls("git-calls"))
        self.assertFalse((self.log / "codex-args").exists())
        self.assertEqual((self.root / "unchanged.txt").read_text(), "original\n")

    def test_success_commits_pushes_and_creates_draft_pr(self):
        result = self.successful()
        self.assertEqual(result.returncode, 0, result.stderr)
        worktree = self.base / "kepp-lab-keplerisgone-kepp-lab-issue-42"
        branch = "agent/keplerisgone-kepp-lab-issue-42"
        git_calls = self.calls("git-calls")
        self.assertIn(f"worktree add -b {branch} {worktree}", git_calls)
        self.assertIn("add -- worker/worker.cpp", git_calls)
        self.assertIn("diff --cached --check", git_calls)
        self.assertIn("commit -m feat: implement issue #42", git_calls)
        self.assertIn(f"push --set-upstream origin {branch}", git_calls)
        gh_calls = self.calls("gh-calls")
        self.assertIn("pr create --repo keplerisgone/kepp-lab --draft --base main", gh_calls)
        self.assertIn(f"--head {branch}", gh_calls)
        self.assertIn("## Verification", gh_calls)
        self.assertEqual((self.log / "codex-cwd").read_text(), str(worktree))
        prompt = (self.log / "codex-prompt").read_text()
        self.assertIn("Issue #42", prompt)
        self.assertIn("Title: Fix worker", prompt)
        self.assertIn("Body:\nKeep it small.", prompt)
        self.assertIn("gpt-5.5", (self.log / "codex-args").read_text())
        self.assertIn("--sandbox\nworkspace-write", (self.log / "codex-args").read_text())
        self.assertEqual(self.calls("make-calls"), f"{worktree / 'worker'}|test\n")
        self.assertEqual((self.root / "unchanged.txt").read_text(), "original\n")

    def test_mini_shell_uses_distinct_names_without_forced_verification(self):
        mini = self.base / "mini-shell"
        mini.mkdir()
        result = self.run_worker(repository="keplerisgone/mini-shell", GH_RESPONSE=self.issue(),
                                 GIT_REMOTE="https://github.com/keplerisgone/mini-shell.git",
                                 GIT_STATUS_KIND="changed", extra_args=None, use_path=False,
                                 MINI_SHELL_PATH=str(mini))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("agent/keplerisgone-mini-shell-issue-42", self.calls("git-calls"))
        self.assertEqual(self.calls("make-calls"), "")
        self.assertIn("no repository-specific automated verification configured", result.stdout)
        self.assertIn("SKIPPED: no repository-specific automated command configured", self.calls("gh-calls"))

    def test_unapproved_repository_is_rejected_before_commands(self):
        result = self.run_worker(repository="someone/other")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Repository is not allowed", result.stderr)
        self.assertEqual(self.calls("gh-calls"), "")

    def test_remote_mismatch_is_rejected(self):
        result = self.run_worker(GIT_REMOTE="git@github.com:keplerisgone/mini-shell.git")
        self.assertEqual(result.returncode, 1)
        self.assertIn("requested keplerisgone/kepp-lab but origin is keplerisgone/mini-shell", result.stderr)
        self.assertEqual(self.calls("gh-calls"), "")

    def test_auth_failure_is_distinct(self):
        result = self.run_worker(GH_AUTH_EXIT="4", GH_AUTH_ERROR="login required\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("GitHub authentication exited with code 4", result.stderr)
        self.assertNotIn("issue list", self.calls("gh-calls"))

    def test_issue_lookup_failure_is_distinct(self):
        result = self.run_worker(GH_ISSUE_EXIT="5")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Issue lookup: gh issue list exited with code 5", result.stderr)

    def test_existing_pr_stops_before_workspace_creation(self):
        result = self.run_worker(GH_RESPONSE=self.issue(), GH_PR_LIST='[{"number":9}]')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("already exists", result.stdout)
        self.assertNotIn("worktree add", self.calls("git-calls"))
        self.assertFalse((self.log / "codex-args").exists())

    def test_existing_branch_is_preserved(self):
        result = self.run_worker(GH_RESPONSE=self.issue(), GIT_BRANCH_EXIT="0")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Existing branch conflict; preserving", result.stderr)
        self.assertNotIn("worktree add", self.calls("git-calls"))

    def test_existing_remote_branch_is_preserved(self):
        result = self.run_worker(GH_RESPONSE=self.issue(), GIT_REMOTE_BRANCH_EXIT="0")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Existing remote branch conflict; preserving", result.stderr)
        self.assertNotIn("worktree add", self.calls("git-calls"))

    def test_existing_worktree_is_preserved(self):
        path = self.base / "kepp-lab-keplerisgone-kepp-lab-issue-42"
        result = self.run_worker(GH_RESPONSE=self.issue(), GIT_WORKTREE_LIST=f"worktree {path}\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Existing worktree conflict; preserving", result.stderr)
        self.assertNotIn("worktree add", self.calls("git-calls"))

    def test_worktree_failure_stops_before_codex(self):
        result = self.run_worker(GH_RESPONSE=self.issue(8), GIT_WORKTREE_EXIT="6")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Worktree creation: git worktree add exited with code 6", result.stderr)
        self.assertFalse((self.log / "codex-args").exists())

    def test_codex_failure_never_submits(self):
        result = self.run_worker(GH_RESPONSE=self.issue(7), CODEX_EXIT="9", CODEX_ERROR="codex failed\n",
                                 GIT_STATUS_KIND="untracked")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Codex exit code: 9", result.stdout)
        self.assertIn("Codex execution exited with code 9", result.stderr)
        self.assert_no_submission()

    def test_codex_signal_is_distinct(self):
        result = self.run_worker(GH_RESPONSE=self.issue(), CODEX_SIGNAL="15", GIT_STATUS_KIND="changed")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Codex execution terminated by signal 15", result.stderr)
        self.assert_no_submission()

    def test_codex_timeout_is_distinct(self):
        result = self.run_worker(GH_RESPONSE=self.issue(), CODEX_SLEEP="3", GIT_STATUS_KIND="changed",
                                 extra_args=["--timeout", "1"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("Codex execution timed out after 1 seconds", result.stderr)
        self.assert_no_submission()

    def test_zero_exit_without_changes_is_failure(self):
        result = self.run_worker(GH_RESPONSE=self.issue())
        self.assertEqual(result.returncode, 1)
        self.assertIn("No Git changes", result.stderr)
        self.assert_no_submission()

    def test_codex_commit_is_rejected_and_preserved(self):
        result = self.run_worker(GH_RESPONSE=self.issue(), GIT_STATUS_KIND="changed", GIT_HEAD_AFTER="def456")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Codex changed HEAD", result.stderr)
        self.assert_no_submission()

    def test_sensitive_change_is_rejected_before_verification(self):
        result = self.run_worker(GH_RESPONSE=self.issue(), GIT_STATUS_KIND="sensitive")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Change validation rejected path: .env", result.stderr)
        self.assertEqual(self.calls("make-calls"), "")
        self.assertNotIn("add --", self.calls("git-calls"))

    def test_verification_failure_blocks_submission(self):
        result = self.successful(MAKE_EXIT="7", MAKE_ERROR="tests failed\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Verification: make test (worker/) exited with code 7", result.stderr)
        self.assert_no_submission()

    def test_existing_worktree_requires_explicit_resume(self):
        self.successful(MAKE_EXIT="7")
        result = self.successful()
        self.assertEqual(result.returncode, 1)
        self.assertIn("--resume", result.stderr)
        self.assertEqual(self.calls("codex-calls"), "run\n")

    def test_resume_retries_verification_without_rerunning_codex(self):
        first = self.successful(MAKE_EXIT="7")
        self.assertEqual(first.returncode, 1)
        result = self.resume()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls("codex-calls"), "run\n")
        self.assertEqual(len(self.calls("make-calls").splitlines()), 2)
        self.assertIn("resuming at verification", result.stdout)

    def test_resume_verification_failure_preserves_changes(self):
        self.successful(MAKE_EXIT="7")
        result = self.resume(MAKE_EXIT="8")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Verification: make test (worker/) exited with code 8", result.stderr)
        self.assertTrue((self.base / "kepp-lab-keplerisgone-kepp-lab-issue-42").exists())
        self.assert_no_submission()

    def test_resume_refuses_incomplete_codex_work(self):
        first = self.successful(CODEX_EXIT="9")
        self.assertEqual(first.returncode, 1)
        result = self.resume()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Codex completion is not proven", result.stderr)
        self.assertEqual(self.calls("codex-calls"), "run\n")

    def test_resume_rejects_worktree_from_other_repository(self):
        self.successful(MAKE_EXIT="7")
        result = self.resume(GIT_WORKTREE_REMOTE="git@github.com:someone/other.git")
        self.assertEqual(result.returncode, 1)
        self.assertIn("worktree repository mismatch", result.stderr)
        self.assertEqual(self.calls("codex-calls"), "run\n")

    def test_resume_detects_changed_issue_content(self):
        self.successful(MAKE_EXIT="7")
        changed = json.dumps([{"number": 42, "title": "Fix worker", "body": "Changed requirements."}])
        result = self.run_worker(GH_RESPONSE=changed, GIT_STATUS_KIND="changed", extra_args=["--resume"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("Issue content changed", result.stderr)
        self.assertEqual(self.calls("codex-calls"), "run\n")

    def test_resume_after_commit_reuses_commit(self):
        first = self.successful(GIT_PUSH_EXIT="13")
        self.assertEqual(first.returncode, 1)
        result = self.resume()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls("codex-calls"), "run\n")
        self.assertEqual(self.calls("git-calls").count("commit -m"), 1)
        self.assertGreaterEqual(self.calls("git-calls").count("push --set-upstream"), 2)

    def test_resume_after_push_creates_pr_without_repush(self):
        first = self.successful(GH_PR_CREATE_EXIT="12")
        self.assertEqual(first.returncode, 1)
        pushes = self.calls("git-calls").count("push --set-upstream")
        result = self.resume(GIT_REMOTE_BRANCH_EXIT="0", GIT_REMOTE_HEAD="def456")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls("git-calls").count("push --set-upstream"), pushes)
        self.assertEqual(self.calls("codex-calls"), "run\n")

    def test_resume_does_not_duplicate_existing_pr(self):
        first = self.successful(GH_PR_CREATE_EXIT="12")
        self.assertEqual(first.returncode, 1)
        result = self.resume(GH_PR_LIST='[{"number":10,"url":"https://github.test/pr/10"}]',
                             GIT_REMOTE_BRANCH_EXIT="0", GIT_REMOTE_HEAD="def456")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls("gh-calls").count("pr create"), 1)

    def test_resume_keeps_default_branch_as_pr_base(self):
        self.successful(GH_PR_CREATE_EXIT="12", GH_DEFAULT_BRANCH="develop")
        result = self.resume(GIT_REMOTE_BRANCH_EXIT="0", GIT_REMOTE_HEAD="def456",
                             GH_DEFAULT_BRANCH="main")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--draft --base develop", self.calls("gh-calls"))

    def test_diff_check_failure_blocks_commit(self):
        result = self.successful(GIT_CHECK_EXIT="3")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Validation: git diff --cached --check exited with code 3", result.stderr)
        self.assert_no_submission()

    def test_push_failure_preserves_commit_and_skips_pr(self):
        result = self.successful(GIT_PUSH_EXIT="13")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Git push exited with code 13", result.stderr)
        self.assertIn("commit -m", self.calls("git-calls"))
        self.assertNotIn("pr create", self.calls("gh-calls"))

    def test_commit_failure_blocks_push_and_pr(self):
        result = self.successful(GIT_COMMIT_EXIT="11")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Git commit exited with code 11", result.stderr)
        self.assertNotIn("push --set-upstream", self.calls("git-calls"))
        self.assertNotIn("pr create", self.calls("gh-calls"))

    def test_pr_creation_failure_preserves_pushed_branch(self):
        result = self.successful(GH_PR_CREATE_EXIT="12")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Draft PR creation: gh pr create exited with code 12", result.stderr)
        self.assertIn("push --set-upstream", self.calls("git-calls"))

    def test_second_pr_check_prevents_duplicate_creation(self):
        result = self.successful(GH_PR_LIST_SECOND='[{"number":10}]')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("already exists", result.stdout)
        self.assertNotIn("pr create", self.calls("gh-calls"))

    def test_repository_lock_rejects_concurrent_worker(self):
        environment = os.environ.copy()
        environment.update({
            "PATH": str(self.bin) + os.pathsep + environment["PATH"],
            "WORKER_LOG": str(self.log),
            "GH_RESPONSE": self.issue(),
            "GIT_STATUS_KIND": "changed",
            "CODEX_SLEEP": "2",
        })
        arguments = [self.root / "worker" / "worker", "--repo", "keplerisgone/kepp-lab",
                     "--path", self.root]
        first = subprocess.Popen(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=environment)
        for _ in range(50):
            if (self.log / "codex-args").exists():
                break
            time.sleep(0.05)
        second = subprocess.run(arguments, text=True, capture_output=True, env=environment, check=False)
        first_stdout, first_stderr = first.communicate(timeout=5)
        self.assertEqual(first.returncode, 0, first_stderr + first_stdout)
        self.assertEqual(second.returncode, 1)
        self.assertIn("Another worker is already running", second.stderr)

    def assert_no_submission(self):
        git_calls = self.calls("git-calls")
        self.assertNotIn("commit -m", git_calls)
        self.assertNotIn("push --set-upstream", git_calls)
        self.assertNotIn("pr create", self.calls("gh-calls"))


if __name__ == "__main__":
    unittest.main()
