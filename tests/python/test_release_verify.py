import importlib.util
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT_PATH = ROOT / "scripts" / "release_verify_lib.py"


def load_module():
    spec = importlib.util.spec_from_file_location("release_verify_lib", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ReleaseVerifyLibTests(unittest.TestCase):
    def test_resolve_target_commit_returns_explicit_commit_override(self):
        module = load_module()

        resolved = module.resolve_target_commit(
            head_sha="abc123",
            explicit_commit="def456",
            worktree_dirty=True,
        )

        self.assertEqual(resolved, "def456")

    def test_resolve_target_commit_returns_head_sha_for_clean_tree(self):
        module = load_module()

        resolved = module.resolve_target_commit(
            head_sha="abc123",
            explicit_commit=None,
            worktree_dirty=False,
        )

        self.assertEqual(resolved, "abc123")

    def test_resolve_target_commit_rejects_blank_explicit_commit(self):
        module = load_module()

        with self.assertRaisesRegex(ValueError, "explicit commit"):
            module.resolve_target_commit(
                head_sha="abc123",
                explicit_commit="   ",
                worktree_dirty=False,
            )

    def test_new_summary_and_record_step_schema_basics(self):
        module = load_module()

        summary = module.new_summary("abc123")
        module.record_step(
            summary,
            name="x86 package build",
            status="pass",
            duration_ms=12,
            log_path="out/release-verify/abc123/x86/build.log",
            artifact_path="out/packages/alps_kv_wrap_pkg-x86_64-abc123.tar.gz",
        )

        self.assertEqual(summary["commit"], "abc123")
        self.assertIn("started_at", summary)
        self.assertIn("finished_at", summary)
        self.assertIn("steps", summary)
        self.assertEqual(summary["steps"][0]["name"], "x86 package build")
        self.assertEqual(summary["steps"][0]["status"], "pass")

    def test_resolve_target_commit_rejects_dirty_tree_without_explicit_commit(self):
        module = load_module()

        with self.assertRaisesRegex(RuntimeError, "dirty"):
            module.resolve_target_commit(
                head_sha="abc123",
                explicit_commit=None,
                worktree_dirty=True,
            )


if __name__ == "__main__":
    unittest.main()
