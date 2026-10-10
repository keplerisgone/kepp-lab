import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
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
        self._script("gh", """#!/bin/sh
printf '%s\\n' "$@" > "$WORKER_LOG/gh-args"
printf '%s' "$GH_RESPONSE"
exit "${GH_EXIT:-0}"
""")
        self._script("git", """#!/bin/sh
printf '%s|%s\\n' "$PWD" "$*" >> "$WORKER_LOG/git-calls"
if [ "$1 $2" = "worktree add" ]; then
  mkdir -p "$5"
  exit "${GIT_WORKTREE_EXIT:-0}"
fi
if [ "$1 $2" = "status --short" ]; then
  printf '%s' "${GIT_STATUS:-}"
  exit "${GIT_STATUS_EXIT:-0}"
fi
exit 2
""")
        self._script("codex", """#!/bin/sh
printf '%s\\n' "$@" > "$WORKER_LOG/codex-args"
printf '%s' "$PWD" > "$WORKER_LOG/codex-cwd"
cat > "$WORKER_LOG/codex-prompt"
printf '%s' "${CODEX_OUTPUT:-}"
printf '%s' "${CODEX_ERROR:-}" >&2
exit "${CODEX_EXIT:-0}"
""")

    def tearDown(self):
        self.temporary.cleanup()

    def _script(self, name, contents):
        path = self.bin / name
        path.write_text(contents)
        path.chmod(0o755)

    def run_worker(self, **values):
        environment = os.environ.copy()
        environment.update({
            "PATH": str(self.bin) + os.pathsep + environment["PATH"],
            "WORKER_LOG": str(self.log),
            "GH_RESPONSE": "[]",
        })
        environment.update(values)
        return subprocess.run(
            [self.root / "worker" / "worker"],
            text=True,
            capture_output=True,
            env=environment,
            check=False,
        )

    def test_no_issue_exits_without_git_or_codex(self):
        result = self.run_worker()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse((self.log / "git-calls").exists())
        self.assertFalse((self.log / "codex-args").exists())
        self.assertEqual((self.root / "unchanged.txt").read_text(), "original\n")
        gh_args = (self.log / "gh-args").read_text().splitlines()
        self.assertIn("--repo", gh_args)
        self.assertIn("keplerisgone/kepp-lab", gh_args)

    def test_issue_creates_worktree_and_runs_sandboxed_codex(self):
        issue = [{"number": 42, "title": "Fix worker", "body": "Keep it small."}]
        result = self.run_worker(
            GH_RESPONSE=json.dumps(issue),
            GIT_STATUS=" M worker/worker.cpp\n",
            CODEX_OUTPUT="done\n",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        worktree = self.base / "kepp-lab-issue-42"
        git_calls = (self.log / "git-calls").read_text()
        self.assertIn(f"worktree add -b agent/issue-42 {worktree}", git_calls)
        self.assertEqual((self.log / "codex-cwd").read_text(), str(worktree))
        self.assertEqual(
            (self.log / "codex-args").read_text().splitlines(),
            ["exec", "--sandbox", "workspace-write", "--cd", str(worktree), "-"],
        )
        prompt = (self.log / "codex-prompt").read_text()
        self.assertIn("Issue #42", prompt)
        self.assertIn("Title: Fix worker", prompt)
        self.assertIn("Body:\nKeep it small.", prompt)
        self.assertIn("Do not commit, push, create a pull request", prompt)
        self.assertIn("Codex exit code: 0", result.stdout)
        self.assertIn("Git changes:\n M worker/worker.cpp", result.stdout)
        self.assertEqual((self.root / "unchanged.txt").read_text(), "original\n")

    def test_codex_failure_is_reported_and_status_is_still_checked(self):
        issue = [{"number": 7, "title": "Fail", "body": "Report failure."}]
        result = self.run_worker(
            GH_RESPONSE=json.dumps(issue),
            CODEX_EXIT="9",
            CODEX_ERROR="codex failed\n",
            GIT_STATUS="?? partial.txt\n",
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("codex failed", result.stderr)
        self.assertIn("Codex exit code: 9", result.stdout)
        self.assertIn("Git changes:\n?? partial.txt", result.stdout)
        self.assertIn("status --short", (self.log / "git-calls").read_text())

    def test_worktree_failure_stops_before_codex(self):
        issue = [{"number": 8, "title": "Blocked", "body": "Do not retry."}]
        result = self.run_worker(
            GH_RESPONSE=json.dumps(issue),
            GIT_WORKTREE_EXIT="6",
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("git worktree add exited with code 6", result.stderr)
        self.assertFalse((self.log / "codex-args").exists())


if __name__ == "__main__":
    unittest.main()
