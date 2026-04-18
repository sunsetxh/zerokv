import importlib.util
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import textwrap


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

    def test_source_archive_name_uses_explicit_commit(self):
        module = load_module()

        self.assertEqual(
            module.source_archive_name("x86_64", "deadbee"),
            "zerokv-src-x86_64-deadbee.tar.gz",
        )

    def test_build_pkg_x86_compile_script_handles_target_commit_path(self):
        script_text = (ROOT / "scripts" / "build_pkg_x86_compile.sh").read_text()

        self.assertRegex(
            script_text,
            re.compile(r'if \[\[ -n "\$\{TARGET_COMMIT:-\}" \]\]; then.*?COMMIT_ID="\$\{TARGET_COMMIT\}"', re.S),
        )
        self.assertIn(
            'git -C "${ROOT_DIR}" archive --format=tar.gz -o "${SRC_ARCHIVE}" "${TARGET_COMMIT}"',
            script_text,
        )
        self.assertIn(
            '[[ -z "${TARGET_COMMIT//[[:space:]]/}" ]] || [[ "${TARGET_COMMIT}" =~ [[:space:]] ]]',
            script_text,
        )

    def test_build_pkg_x86_compile_script_targets_explicit_commit_with_fakes(self):
        script_path = ROOT / "scripts" / "build_pkg_x86_compile.sh"
        target_commit = "deadbee"

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            bin_dir = tmp / "bin"
            bin_dir.mkdir()
            log_file = tmp / "calls.log"
            output_tarball = ROOT / "out" / "packages" / f"alps_kv_wrap_pkg-x86_64-{target_commit}.tar.gz"
            source_archive = ROOT / "out" / "src" / f"zerokv-src-x86_64-{target_commit}.tar.gz"
            ucx_tarball = ROOT / "ucx-v1.20.0.tar.gz"
            created_ucx = False

            if not ucx_tarball.exists():
                ucx_tarball.write_bytes(b"fake-ucx")
                created_ucx = True

            fake_git = bin_dir / "git"
            fake_git.write_text(
                textwrap.dedent(
                    f"""\
                    #!/usr/bin/env bash
                    set -euo pipefail
                    printf '%s\\n' "git $*" >> "{log_file}"
                    if [[ "$1" == "-C" && "$3" == "rev-parse" && "$4" == "--git-common-dir" ]]; then
                        printf '.git\\n'
                        exit 0
                    fi
                    if [[ "$1" == "-C" && "$3" == "archive" ]]; then
                        out=""
                        commit=""
                        while [[ $# -gt 0 ]]; do
                            case "$1" in
                                -o)
                                    out="$2"
                                    shift 2
                                    ;;
                                archive)
                                    shift
                                    ;;
                                --format=tar.gz)
                                    shift
                                    ;;
                                -C)
                                    shift 2
                                    ;;
                                *)
                                    commit="$1"
                                    shift
                                    ;;
                            esac
                        done
                        printf '%s\\n' "$commit" >> "{log_file}"
                        : > "$out"
                        exit 0
                    fi
                    if [[ "$1" == "-C" && "$3" == "rev-parse" && "$4" == "--short" && "$5" == "HEAD" ]]; then
                        printf 'unreachable\\n'
                        exit 0
                    fi
                    echo "unexpected git invocation: $*" >&2
                    exit 1
                    """
                )
            )
            fake_git.chmod(0o755)

            fake_docker = bin_dir / "docker"
            fake_docker.write_text(
                textwrap.dedent(
                    f"""\
                    #!/usr/bin/env bash
                    set -euo pipefail
                    printf '%s\\n' "docker $*" >> "{log_file}"
                    case "$1" in
                        image)
                            exit 1
                            ;;
                        container)
                            exit 1
                            ;;
                        pull|create|start|rm)
                            exit 0
                            ;;
                        cp)
                            src="$2"
                            dest="$3"
                            if [[ "$dest" != *:* ]]; then
                                mkdir -p "$(dirname "$dest")"
                                : > "$dest"
                            fi
                            exit 0
                            ;;
                        exec)
                            exit 0
                            ;;
                    esac
                    echo "unexpected docker invocation: $*" >&2
                    exit 1
                    """
                )
            )
            fake_docker.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}:{env['PATH']}"
            env["TARGET_COMMIT"] = target_commit

            try:
                completed = subprocess.run(
                    ["bash", str(script_path)],
                    cwd=ROOT,
                    env=env,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(
                    completed.returncode,
                    0,
                    msg=f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
                )

                log_text = log_file.read_text()
                self.assertIn(f"git -C {ROOT} archive --format=tar.gz -o {source_archive} {target_commit}", log_text)
                self.assertIn(f"docker create --name alps-pkg-x86-glibc228-{target_commit}", log_text)
                self.assertIn(
                    f"docker cp alps-pkg-x86-glibc228-{target_commit}:/tmp/alps_kv_wrap_pkg-x86_64-{target_commit}.tar.gz {output_tarball}",
                    log_text,
                )
                self.assertTrue(source_archive.exists())
                self.assertTrue(output_tarball.exists())
            finally:
                if created_ucx and ucx_tarball.exists():
                    ucx_tarball.unlink()
                if output_tarball.exists():
                    output_tarball.unlink()
                if source_archive.exists():
                    source_archive.unlink()


if __name__ == "__main__":
    unittest.main()
