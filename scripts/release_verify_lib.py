from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def source_archive_name(arch: str, commit: str) -> str:
    return f"zerokv-src-{arch}-{commit}.tar.gz"


def render_example_commands(commit: str, out_dir: str = "out") -> list[dict[str, object]]:
    out_root = Path(out_dir) / "release-verify" / commit / "arm" / "examples"
    entries: list[dict[str, object]] = [
        {
            "name": "ping_pong",
            "source": "build_tree",
            "build_targets": ["ping_pong"],
            "log_dir": str(out_root / "ping_pong"),
        },
        {
            "name": "rdma_put_get",
            "source": "build_tree",
            "build_targets": ["rdma_put_get"],
            "log_dir": str(out_root / "rdma_put_get"),
        },
        {
            "name": "kv_demo",
            "source": "build_tree",
            "build_targets": ["kv_demo"],
            "log_dir": str(out_root / "kv_demo"),
        },
        {
            "name": "kv_wait_fetch",
            "source": "build_tree",
            "build_targets": ["kv_wait_fetch", "kv_demo"],
            "log_dir": str(out_root / "kv_wait_fetch"),
        },
        {
            "name": "message_kv_demo",
            "source": "build_tree",
            "build_targets": ["message_kv_demo"],
            "log_dir": str(out_root / "message_kv_demo"),
        },
        {
            "name": "alps_kv_bench",
            "source": "package",
            "build_targets": [],
            "log_dir": str(out_root / "alps_kv_bench"),
        },
    ]
    return entries


def _split_host_port(vm1: str) -> tuple[str, str]:
    if ":" in vm1:
        host, port = vm1.rsplit(":", 1)
        if host and port:
            return host, port
    return vm1, "22"


def render_arm_remote_build(
    *,
    commit: str,
    vm1: str,
    vm_user: str,
    vm_pass: str,
    out_dir: str,
) -> dict:
    del vm_pass
    host, port = _split_host_port(vm1)
    out_root = Path(out_dir)
    release_root = out_root / "release-verify" / commit / "arm"
    return {
        "commit": commit,
        "arch": "aarch64",
        "ssh_target": f"{vm_user}@{host}:{port}",
        "ssh_host": host,
        "ssh_port": port,
        "remote_pkg_name": f"alps_kv_wrap_pkg-aarch64-{commit}",
        "remote_src_archive": f"/tmp/{source_archive_name('aarch64', commit)}",
        "remote_ucx_tarball": "/tmp/ucx-v1.20.0.tar.gz",
        "remote_pkg_tarball": f"/tmp/alps_kv_wrap_pkg-aarch64-{commit}.tar.gz",
        "remote_package_txt": f"/tmp/alps_kv_wrap_pkg-aarch64-{commit}.package.txt",
        "local_src_archive": str(out_root / "src" / source_archive_name("aarch64", commit)),
        "local_tarball": str(out_root / "packages" / f"alps_kv_wrap_pkg-aarch64-{commit}.tar.gz"),
        "build_log": str(release_root / "build.log"),
        "package_txt": str(release_root / "package.txt"),
    }


def resolve_target_commit(
    head_sha: str,
    explicit_commit: str | None,
    worktree_dirty: bool,
) -> str:
    if explicit_commit is not None:
        commit = explicit_commit.strip()
        if not commit:
            raise ValueError("explicit commit must not be blank")
        return commit
    if worktree_dirty:
        raise RuntimeError("dirty worktree requires explicit commit")
    return head_sha


def new_summary(commit: str) -> dict:
    return {
        "commit": commit,
        "started_at": utc_now(),
        "finished_at": None,
        "packages": {},
        "steps": [],
    }


def record_step(
    summary: dict,
    *,
    name: str,
    status: str,
    duration_ms: int,
    log_path: str | None = None,
    artifact_path: str | None = None,
    reason: str | None = None,
) -> None:
    summary["steps"].append(
        {
            "name": name,
            "status": status,
            "duration_ms": duration_ms,
            "log_path": log_path,
            "artifact_path": artifact_path,
            "reason": reason,
        }
    )
