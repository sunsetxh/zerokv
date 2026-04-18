from __future__ import annotations

from datetime import datetime, timezone


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


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
