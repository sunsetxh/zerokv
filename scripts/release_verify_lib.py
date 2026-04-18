from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Sequence


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_VM1 = "192.168.3.9:2222"
DEFAULT_VM2 = "192.168.3.9:2223"
DEFAULT_VM_USER = "axon"
DEFAULT_VM_PASS = "axon"
VM1_RDMA_IP = "10.0.0.1"
VM2_RDMA_IP = "10.0.0.2"
SOFTROCE_ENV = {
    "UCX_PROTO_ENABLE": "n",
    "UCX_NET_DEVICES": "rxe0:1",
    "UCX_TLS": "rc,sm,self",
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def source_archive_name(arch: str, commit: str) -> str:
    return f"zerokv-src-{arch}-{commit}.tar.gz"


def package_tag(commit: str) -> str:
    return commit[:7]


def package_dir_name(arch: str, commit: str) -> str:
    return f"zerokv-{arch}-{package_tag(commit)}"


def package_tarball_name(arch: str, commit: str) -> str:
    return f"{package_dir_name(arch, commit)}.tar.gz"


def render_example_commands(commit: str, out_dir: str = "out") -> list[dict[str, object]]:
    out_root = Path(out_dir) / "release-verify" / commit / "arm" / "examples"
    return [
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


def _split_host_port(target: str) -> tuple[str, str]:
    if ":" in target:
        host, port = target.rsplit(":", 1)
        if host and port:
            return host, port
    return target, "22"


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
        "remote_pkg_name": package_dir_name("aarch64", commit),
        "remote_src_archive": f"/tmp/{source_archive_name('aarch64', commit)}",
        "remote_ucx_tarball": "/tmp/ucx-v1.20.0.tar.gz",
        "remote_pkg_tarball": f"/tmp/{package_tarball_name('aarch64', commit)}",
        "remote_package_txt": f"/tmp/{package_dir_name('aarch64', commit)}.package.txt",
        "local_src_archive": str(out_root / "src" / source_archive_name("aarch64", commit)),
        "local_tarball": str(out_root / "packages" / package_tarball_name("aarch64", commit)),
        "build_log": str(release_root / "build.log"),
        "package_txt": str(release_root / "package.txt"),
    }


def resolve_target_commit(head_sha: str, explicit_commit: str | None, worktree_dirty: bool) -> str:
    if explicit_commit is not None:
        commit = explicit_commit.strip()
        if not commit:
            raise ValueError("explicit commit must not be blank")
        return commit
    if worktree_dirty:
        raise RuntimeError("dirty worktree requires explicit commit")
    return head_sha


def default_step_plan(
    *,
    skip_x86: bool,
    skip_arm: bool,
    skip_examples: bool,
    skip_perf: bool,
) -> list[str]:
    plan: list[str] = []
    if not skip_x86:
        plan.extend(["x86_package", "x86_runtime_smoke"])
    if not skip_arm:
        plan.extend(["arm_package", "arm_softroce_ready", "arm_runtime_smoke", "arm_alps_e2e"])
        if not skip_examples:
            plan.append("arm_examples")
        if not skip_perf:
            plan.append("arm_perf")
    return plan


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


def record_step_result(summary: dict, **kwargs: object) -> bool:
    record_step(summary, **kwargs)
    return kwargs["status"] != "fail"


def _shell_join(parts: Sequence[str]) -> str:
    return " ".join(shlex.quote(part) for part in parts)


def render_release_perf_command(
    commit: str,
    *,
    out_dir: str = "out",
    vm1: str = DEFAULT_VM1,
    vm2: str = DEFAULT_VM2,
    vm_user: str = DEFAULT_VM_USER,
    vm_pass: str = DEFAULT_VM_PASS,
) -> str:
    vm1_host, vm1_port = _split_host_port(vm1)
    vm2_host, vm2_port = _split_host_port(vm2)
    perf_out = Path(out_dir) / "release-verify" / commit / "arm" / "perf"
    remote_root = f"/tmp/{package_dir_name('aarch64', commit)}"
    command = [
        "./scripts/perf_experiments.py",
        "run-alps-matrix",
        "--server-target",
        f"{vm_user}@{vm1_host}",
        "--client-target",
        f"{vm_user}@{vm2_host}",
        "--server-ssh-port",
        vm1_port,
        "--client-ssh-port",
        vm2_port,
        "--alps-binary",
        "./bin/alps_kv_bench",
        "--server-workdir",
        remote_root,
        "--client-workdir",
        remote_root,
        "--server-rdma-ip",
        VM1_RDMA_IP,
        "--rdma-device",
        SOFTROCE_ENV["UCX_NET_DEVICES"],
        "--ucx-tls",
        SOFTROCE_ENV["UCX_TLS"],
        "--proto-modes",
        "n",
        "--rma-rails",
        "1",
        "--sizes",
        "1K,1M,32M",
        "--out-dir",
        str(perf_out),
        "--extra-env",
        f"UCX_TLS={SOFTROCE_ENV['UCX_TLS']}",
        "--extra-env",
        f"UCX_NET_DEVICES={SOFTROCE_ENV['UCX_NET_DEVICES']}",
        "--extra-env",
        f"LD_LIBRARY_PATH={remote_root}/lib",
    ]
    del vm_pass
    return _shell_join(command)


def _git_output(*args: str) -> str:
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def _run_logged(
    command: Sequence[str],
    *,
    log_path: Path,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    append: bool = False,
) -> subprocess.CompletedProcess[str]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    mode = "a" if append else "w"
    with log_path.open(mode, encoding="utf-8") as handle:
        return subprocess.run(
            list(command),
            cwd=str(cwd or ROOT),
            env=env,
            text=True,
            stdin=subprocess.DEVNULL,
            stdout=handle,
            stderr=subprocess.STDOUT,
            check=False,
        )


def _with_password(command: Sequence[str], *, password: str, env: dict[str, str] | None = None) -> tuple[list[str], dict[str, str] | None, str | None]:
    if shutil.which("sshpass"):
        return ["sshpass", "-p", password, *command], env, None
    askpass_fd, askpass_path = tempfile.mkstemp(prefix="codex-askpass-", text=True)
    os.close(askpass_fd)
    Path(askpass_path).write_text(
        "#!/usr/bin/env bash\n" f"printf '%s\\n' {shlex.quote(password)}\n",
        encoding="utf-8",
    )
    os.chmod(askpass_path, 0o700)
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    merged_env.update(
        {
            "SSH_ASKPASS": askpass_path,
            "SSH_ASKPASS_REQUIRE": "force",
            "DISPLAY": "codex",
        }
    )
    return ["setsid", "-w", *command], merged_env, askpass_path


def _run_password_logged(
    command: Sequence[str],
    *,
    password: str,
    log_path: Path,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    append: bool = False,
) -> subprocess.CompletedProcess[str]:
    wrapped_cmd, wrapped_env, askpass_path = _with_password(command, password=password, env=env)
    try:
        return _run_logged(wrapped_cmd, log_path=log_path, cwd=cwd, env=wrapped_env, append=append)
    finally:
        if askpass_path:
            Path(askpass_path).unlink(missing_ok=True)


def _remote_command(
    *,
    host: str,
    port: str,
    user: str,
    password: str,
    remote_cmd: str,
) -> list[str]:
    return [
        "ssh",
        "-p",
        port,
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "LogLevel=ERROR",
        f"{user}@{host}",
        remote_cmd,
    ]


def _env_prefix(extra: dict[str, str] | None = None) -> str:
    merged = dict(SOFTROCE_ENV)
    if extra:
        merged.update(extra)
    return "env " + " ".join(f"{key}={shlex.quote(value)}" for key, value in merged.items())


def _extract_package(tarball: Path, destination: Path) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tarball, "r:gz") as archive:
        try:
            archive.extractall(destination, filter="data")
        except TypeError:
            archive.extractall(destination)
        names = [member.name.split("/", 1)[0] for member in archive.getmembers() if member.name]
    roots = [name for name in names if name and name != "."]
    return destination / roots[0]


def _write_summary_files(summary: dict, release_root: Path) -> None:
    summary["finished_at"] = utc_now()
    lines: list[str] = []
    for step in summary["steps"]:
        status = str(step["status"]).upper()
        line = f"{status} {step['name']}"
        if step.get("log_path"):
            line += f" log={step['log_path']}"
        if step.get("artifact_path"):
            line += f" artifact={step['artifact_path']}"
        if step.get("reason"):
            line += f" reason={step['reason']}"
        lines.append(line)
    if summary["packages"]:
        lines.append("")
        for name, artifact in summary["packages"].items():
            lines.append(f"{name} package: {artifact}")
    (release_root / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (release_root / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _record_and_maybe_stop(
    summary: dict,
    *,
    name: str,
    status: str,
    start_time: float,
    log_path: Path | None = None,
    artifact_path: Path | None = None,
    reason: str | None = None,
) -> bool:
    return record_step_result(
        summary,
        name=name,
        status=status,
        duration_ms=max(0, int((time_now() - start_time) * 1000)),
        log_path=str(log_path) if log_path else None,
        artifact_path=str(artifact_path) if artifact_path else None,
        reason=reason,
    )


def time_now() -> float:
    import time

    return time.monotonic()


def _local_smoke_x86(commit: str, out_dir: Path, summary: dict) -> bool:
    tarball = out_dir / "packages" / package_tarball_name("x86_64", commit)
    log_dir = out_dir / "release-verify" / commit / "x86" / "runtime"
    log_dir.mkdir(parents=True, exist_ok=True)
    stage_root = _extract_package(tarball, log_dir / "unpacked")
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(stage_root / "lib")

    step_start = time_now()
    ucx_log = log_dir / "ucx_info.log"
    result = _run_logged([str(stage_root / "bin" / "ucx_info"), "-d"], log_path=ucx_log, env=env)
    if result.returncode != 0:
        return _record_and_maybe_stop(summary, name="x86_runtime_smoke", status="fail", start_time=step_start, log_path=ucx_log, reason="ucx_info failed")

    server_log = log_dir / "alps_server.log"
    server = _run_logged(
        [
            "timeout",
            "5",
            str(stage_root / "bin" / "alps_kv_bench"),
            "--mode",
            "server",
            "--port",
            "17721",
            "--sizes",
            "1K",
            "--iters",
            "1",
            "--warmup",
            "0",
            "--threads",
            "1",
        ],
        log_path=server_log,
        env=env,
    )
    server_text = server_log.read_text(encoding="utf-8", errors="replace")
    ok = server.returncode in (0, 124) and "ALPS_KV_LISTEN address=" in server_text
    return _record_and_maybe_stop(
        summary,
        name="x86_runtime_smoke",
        status="pass" if ok else "fail",
        start_time=step_start,
        log_path=server_log,
        reason=None if ok else "alps_kv_bench server smoke failed",
    )


def _stage_arm_package(commit: str, out_dir: Path, vm: str, vm_user: str, vm_pass: str, log_path: Path) -> tuple[bool, str]:
    host, port = _split_host_port(vm)
    tarball = out_dir / "packages" / package_tarball_name("aarch64", commit)
    remote_tarball = f"/tmp/{package_tarball_name('aarch64', commit)}"
    remote_root = f"/tmp/{package_dir_name('aarch64', commit)}"
    copy_result = _run_password_logged(
        [
            "scp",
            "-P",
            port,
            "-o",
            "StrictHostKeyChecking=no",
            "-o",
            "UserKnownHostsFile=/dev/null",
            "-o",
            "LogLevel=ERROR",
            str(tarball),
            f"{vm_user}@{host}:{remote_tarball}",
        ],
        password=vm_pass,
        log_path=log_path,
    )
    if copy_result.returncode != 0:
        return False, remote_root
    stage = _run_password_logged(
        _remote_command(
            host=host,
            port=port,
            user=vm_user,
            password=vm_pass,
            remote_cmd=f"set -euo pipefail; rm -rf {shlex.quote(remote_root)}; tar xzf {shlex.quote(remote_tarball)} -C /tmp",
        ),
        password=vm_pass,
        log_path=log_path,
        append=True,
    )
    return stage.returncode == 0, remote_root


def _remote_softroce_ready(commit: str, out_dir: Path, vm1: str, vm2: str, vm_user: str, vm_pass: str, summary: dict) -> bool:
    release_root = out_dir / "release-verify" / commit / "arm"
    vm1_log = release_root / "softroce-vm1.log"
    vm2_log = release_root / "softroce-vm2.log"
    step_start = time_now()

    def collect(vm: str, log_path: Path) -> tuple[int, str]:
        host, port = _split_host_port(vm)
        cmd = "set -euo pipefail; ibv_devices; echo '---'; ucx_info -d; echo '---'; rdma link show; echo '---'; ip -o addr show"
        result = _run_password_logged(
            _remote_command(host=host, port=port, user=vm_user, password=vm_pass, remote_cmd=cmd),
            password=vm_pass,
            log_path=log_path,
        )
        return result.returncode, log_path.read_text(encoding="utf-8", errors="replace")

    code1, text1 = collect(vm1, vm1_log)
    code2, text2 = collect(vm2, vm2_log)
    netdev1 = _extract_rxe_netdev(text1)
    netdev2 = _extract_rxe_netdev(text2)
    vm1_ok = code1 == 0 and "rxe0" in text1 and netdev1 is not None and _ip_belongs_to_netdev(text1, VM1_RDMA_IP, netdev1)
    vm2_ok = code2 == 0 and "rxe0" in text2 and netdev2 is not None and _ip_belongs_to_netdev(text2, VM2_RDMA_IP, netdev2)
    ok = vm1_ok and vm2_ok
    failure_log = vm1_log if not vm1_ok else vm2_log
    return _record_and_maybe_stop(
        summary,
        name="arm_softroce_ready",
        status="pass" if ok else "fail",
        start_time=step_start,
        log_path=failure_log if not ok else vm2_log,
        reason=None if ok else "softroce readiness check failed",
    )


def _extract_rxe_netdev(text: str) -> str | None:
    for line in text.splitlines():
        if "rxe0" not in line or "netdev" not in line:
            continue
        tail = line.split("netdev", 1)[1].strip()
        if tail:
            return tail.split()[0]
    return None


def _ip_belongs_to_netdev(text: str, ip_addr: str, netdev: str) -> bool:
    token = f"{ip_addr}/"
    for line in text.splitlines():
        if netdev in line and token in line:
            return True
    return False


def _arm_runtime_smoke(commit: str, out_dir: Path, vm1: str, vm2: str, vm_user: str, vm_pass: str, summary: dict) -> bool:
    release_root = out_dir / "release-verify" / commit / "arm"
    setup_log = release_root / "runtime-smoke-stage.log"
    step_start = time_now()
    ok1, remote_root_vm1 = _stage_arm_package(commit, out_dir, vm1, vm_user, vm_pass, setup_log)
    ok2, remote_root_vm2 = _stage_arm_package(commit, out_dir, vm2, vm_user, vm_pass, setup_log)
    if not (ok1 and ok2):
        return _record_and_maybe_stop(summary, name="arm_runtime_smoke", status="fail", start_time=step_start, log_path=setup_log, reason="failed to stage arm package")

    host1, port1 = _split_host_port(vm1)
    host2, port2 = _split_host_port(vm2)
    vm1_ucx_log = release_root / "vm1-ucx_info.log"
    vm2_ucx_log = release_root / "vm2-ucx_info.log"
    vm1_server_log = release_root / "vm1-alps_server.log"
    ld_env = {"LD_LIBRARY_PATH": f"{remote_root_vm1}/lib"}
    ucx_cmd_vm1 = f"set -euo pipefail; cd {shlex.quote(remote_root_vm1)}; {_env_prefix(ld_env)} ./bin/ucx_info -d"
    ucx_cmd_vm2 = f"set -euo pipefail; cd {shlex.quote(remote_root_vm2)}; {_env_prefix({'LD_LIBRARY_PATH': f'{remote_root_vm2}/lib'})} ./bin/ucx_info -d"
    server_cmd = (
        f"set -euo pipefail; cd {shlex.quote(remote_root_vm1)}; "
        f"{_env_prefix({'LD_LIBRARY_PATH': f'{remote_root_vm1}/lib'})} "
        "timeout 5 ./bin/alps_kv_bench --mode server --port 17721 --sizes 1K --iters 1 --warmup 0 --threads 1"
    )
    rc_vm1 = _run_password_logged(_remote_command(host=host1, port=port1, user=vm_user, password=vm_pass, remote_cmd=ucx_cmd_vm1), password=vm_pass, log_path=vm1_ucx_log).returncode
    rc_vm2 = _run_password_logged(_remote_command(host=host2, port=port2, user=vm_user, password=vm_pass, remote_cmd=ucx_cmd_vm2), password=vm_pass, log_path=vm2_ucx_log).returncode
    server_rc = _run_password_logged(_remote_command(host=host1, port=port1, user=vm_user, password=vm_pass, remote_cmd=server_cmd), password=vm_pass, log_path=vm1_server_log).returncode
    server_text = vm1_server_log.read_text(encoding="utf-8", errors="replace")
    ok = rc_vm1 == 0 and rc_vm2 == 0 and server_rc in (0, 124) and "ALPS_KV_LISTEN address=" in server_text
    failure_log = vm1_ucx_log
    if rc_vm1 == 0 and rc_vm2 != 0:
        failure_log = vm2_ucx_log
    elif rc_vm1 == 0 and rc_vm2 == 0:
        failure_log = vm1_server_log
    return _record_and_maybe_stop(
        summary,
        name="arm_runtime_smoke",
        status="pass" if ok else "fail",
        start_time=step_start,
        log_path=failure_log,
        reason=None if ok else "arm runtime smoke failed",
    )


def _arm_alps_e2e(commit: str, out_dir: Path, vm1: str, vm2: str, vm_user: str, vm_pass: str, summary: dict) -> bool:
    release_root = out_dir / "release-verify" / commit / "arm" / "e2e"
    release_root.mkdir(parents=True, exist_ok=True)
    step_start = time_now()
    setup_log = release_root / "stage.log"
    ok1, remote_root_vm1 = _stage_arm_package(commit, out_dir, vm1, vm_user, vm_pass, setup_log)
    ok2, remote_root_vm2 = _stage_arm_package(commit, out_dir, vm2, vm_user, vm_pass, setup_log)
    if not (ok1 and ok2):
        return _record_and_maybe_stop(summary, name="arm_alps_e2e", status="fail", start_time=step_start, log_path=setup_log, reason="failed to stage arm package")

    host1, port1 = _split_host_port(vm1)
    host2, port2 = _split_host_port(vm2)
    server_log = release_root / "server.log"
    client_log = release_root / "client.log"
    server_cmd = (
        f"cd {shlex.quote(remote_root_vm1)} && "
        f"{_env_prefix({'LD_LIBRARY_PATH': f'{remote_root_vm1}/lib'})} "
        "./bin/alps_kv_bench --mode server --port 16000 --sizes 256K --iters 1 --warmup 0 --threads 1"
    )
    client_cmd = (
        f"cd {shlex.quote(remote_root_vm2)} && "
        f"{_env_prefix({'LD_LIBRARY_PATH': f'{remote_root_vm2}/lib'})} "
        f"./bin/alps_kv_bench --mode client --host {VM1_RDMA_IP} --port 16000 --sizes 256K --iters 1 --warmup 0 --threads 1"
    )
    server_handle = server_log.open("w", encoding="utf-8")
    server_proc_cmd, server_proc_env, server_askpass_path = _with_password(
        _remote_command(host=host1, port=port1, user=vm_user, password=vm_pass, remote_cmd=server_cmd),
        password=vm_pass,
    )
    server_proc = subprocess.Popen(
        server_proc_cmd,
        stdout=server_handle,
        stderr=subprocess.STDOUT,
        text=True,
        env=server_proc_env,
        stdin=subprocess.DEVNULL,
    )
    client_rc = -1
    cleanup_error: str | None = None
    try:
        import time

        for _ in range(50):
            if server_log.exists() and "ALPS_KV_LISTEN address=" in server_log.read_text(encoding="utf-8", errors="replace"):
                break
            if server_proc.poll() is not None:
                break
            time.sleep(0.2)
        client_rc = _run_logged(
            _remote_command(host=host2, port=port2, user=vm_user, password=vm_pass, remote_cmd=client_cmd),
            log_path=client_log,
        ).returncode
    finally:
        try:
            if server_proc.poll() is None:
                server_proc.terminate()
            server_proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            cleanup_error = "server cleanup timed out"
            server_proc.kill()
            server_proc.wait(timeout=10)
        finally:
            server_handle.close()
            if server_askpass_path:
                Path(server_askpass_path).unlink(missing_ok=True)

    server_text = server_log.read_text(encoding="utf-8", errors="replace")
    client_text = client_log.read_text(encoding="utf-8", errors="replace")
    ok = cleanup_error is None and "ALPS_KV_LISTEN address=" in server_text and "ALPS_KV_ROUND role=client" in client_text and "avg_control_request_grant_us=" in client_text and client_rc == 0
    return _record_and_maybe_stop(
        summary,
        name="arm_alps_e2e",
        status="pass" if ok else "fail",
        start_time=step_start,
        log_path=client_log,
        reason=None if ok else cleanup_error or "arm alps e2e failed",
    )


def _run_step_command(name: str, command: Sequence[str], summary: dict, log_path: Path, artifact_path: Path | None = None, env: dict[str, str] | None = None) -> bool:
    step_start = time_now()
    result = _run_logged(command, log_path=log_path, env=env)
    ok = result.returncode == 0
    return _record_and_maybe_stop(
        summary,
        name=name,
        status="pass" if ok else "fail",
        start_time=step_start,
        log_path=log_path,
        artifact_path=artifact_path,
        reason=None if ok else f"{name} failed",
    )


def run_release_verify(args: argparse.Namespace) -> int:
    head_sha = _git_output("rev-parse", "HEAD")
    dirty = bool(_git_output("status", "--porcelain"))
    commit_ref = resolve_target_commit(head_sha=head_sha, explicit_commit=args.commit, worktree_dirty=dirty)
    commit = _git_output("rev-parse", commit_ref)
    out_dir = Path(args.out_dir)
    release_root = out_dir / "release-verify" / commit
    release_root.mkdir(parents=True, exist_ok=True)
    summary = new_summary(commit)
    success = True

    try:
        for step in default_step_plan(
            skip_x86=args.skip_x86,
            skip_arm=args.skip_arm,
            skip_examples=args.skip_examples,
            skip_perf=args.skip_perf,
        ):
            if step == "x86_package":
                x86_log = release_root / "x86" / "build.log"
                env = os.environ.copy()
                env["TARGET_COMMIT"] = commit
                ok = _run_step_command(
                    "x86_package",
                    [str(ROOT / "scripts" / "build_pkg_x86_compile.sh")],
                    summary,
                    x86_log,
                    artifact_path=out_dir / "packages" / package_tarball_name("x86_64", commit),
                    env=env,
                )
                if ok:
                    summary["packages"]["x86"] = str(out_dir / "packages" / package_tarball_name("x86_64", commit))
                else:
                    success = False
                    break
            elif step == "x86_runtime_smoke":
                if not _local_smoke_x86(commit, out_dir, summary):
                    success = False
                    break
            elif step == "arm_package":
                arm_log = release_root / "arm" / "build.log"
                ok = _run_step_command(
                    "arm_package",
                    [
                        str(ROOT / "scripts" / "build_pkg_arm_remote.sh"),
                        "--commit",
                        commit,
                        "--vm1",
                        args.vm1,
                        "--vm-user",
                        args.vm_user,
                        "--vm-pass",
                        args.vm_pass,
                        "--out-dir",
                        str(out_dir),
                    ],
                    summary,
                    arm_log,
                    artifact_path=out_dir / "packages" / package_tarball_name("aarch64", commit),
                )
                if ok:
                    summary["packages"]["arm"] = str(out_dir / "packages" / package_tarball_name("aarch64", commit))
                else:
                    success = False
                    break
            elif step == "arm_softroce_ready":
                if not _remote_softroce_ready(commit, out_dir, args.vm1, args.vm2, args.vm_user, args.vm_pass, summary):
                    success = False
                    break
            elif step == "arm_runtime_smoke":
                if not _arm_runtime_smoke(commit, out_dir, args.vm1, args.vm2, args.vm_user, args.vm_pass, summary):
                    success = False
                    break
            elif step == "arm_alps_e2e":
                if not _arm_alps_e2e(commit, out_dir, args.vm1, args.vm2, args.vm_user, args.vm_pass, summary):
                    success = False
                    break
            elif step == "arm_examples":
                examples_log = release_root / "arm" / "examples.log"
                if not _run_step_command(
                    "arm_examples",
                    [
                        str(ROOT / "scripts" / "release_verify_examples.sh"),
                        "--commit",
                        commit,
                        "--vm1",
                        args.vm1,
                        "--vm2",
                        args.vm2,
                        "--vm-user",
                        args.vm_user,
                        "--vm-pass",
                        args.vm_pass,
                        "--out-dir",
                        str(out_dir),
                    ],
                    summary,
                    examples_log,
                ):
                    success = False
                    break
            elif step == "arm_perf":
                perf_log = release_root / "arm" / "perf.log"
                if not _run_step_command(
                    "arm_perf",
                    shlex.split(render_release_perf_command(commit, out_dir=str(out_dir), vm1=args.vm1, vm2=args.vm2, vm_user=args.vm_user, vm_pass=args.vm_pass)),
                    summary,
                    perf_log,
                ):
                    success = False
                    break
    finally:
        _write_summary_files(summary, release_root)
    return 0 if success else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Build and verify pinned release artifacts.")
    parser.add_argument("--commit")
    parser.add_argument("--skip-x86", action="store_true")
    parser.add_argument("--skip-arm", action="store_true")
    parser.add_argument("--skip-perf", action="store_true")
    parser.add_argument("--skip-examples", action="store_true")
    parser.add_argument("--vm1", default=DEFAULT_VM1)
    parser.add_argument("--vm2", default=DEFAULT_VM2)
    parser.add_argument("--vm-user", default=DEFAULT_VM_USER)
    parser.add_argument("--vm-pass", default=DEFAULT_VM_PASS)
    parser.add_argument("--out-dir", default=str(ROOT / "out"))
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return run_release_verify(args)


if __name__ == "__main__":
    raise SystemExit(main())
