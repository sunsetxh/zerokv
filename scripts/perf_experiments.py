#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import os
import pathlib
import queue
import shlex
import socket
import subprocess
import sys
import threading
import time
from typing import Dict, Iterable, List, Optional


ROOT = pathlib.Path(__file__).resolve().parents[1]


@dataclasses.dataclass(frozen=True)
class ExperimentCase:
    name: str
    env: Dict[str, str]
    perftest: str = "ucp_put_bw"


def build_alps_matrix_cases(proto_modes: Iterable[str],
                            rma_rails: Iterable[int]) -> List[ExperimentCase]:
    cases: List[ExperimentCase] = []
    for proto in proto_modes:
        for rma in rma_rails:
            cases.append(
                ExperimentCase(
                    name=f"proto-{proto}-rma{rma}",
                    env={
                        "UCX_PROTO_ENABLE": str(proto),
                        "UCX_MAX_RMA_RAILS": str(rma),
                        "UCX_MAX_RNDV_RAILS": str(rma),
                    },
                ))
    return cases


def build_ucx_matrix_cases(proto_modes: Iterable[str],
                           rma_rails: Iterable[int],
                           perftest: str = "ucp_put_bw") -> List[ExperimentCase]:
    cases: List[ExperimentCase] = []
    for proto in proto_modes:
        for rma in rma_rails:
            cases.append(
                ExperimentCase(
                    name=f"proto-{proto}-rma{rma}",
                    env={
                        "UCX_PROTO_ENABLE": str(proto),
                        "UCX_MAX_RMA_RAILS": str(rma),
                        "UCX_MAX_RNDV_RAILS": str(rma),
                    },
                    perftest=perftest,
                ))
    return cases


def parse_csv_list(value: str) -> List[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def parse_int_csv_list(value: str) -> List[int]:
    return [int(item) for item in parse_csv_list(value)]


def parse_extra_env(items: Iterable[str]) -> Dict[str, str]:
    env: Dict[str, str] = {}
    for item in items:
        key, sep, val = item.partition("=")
        if not sep:
            raise ValueError(f"invalid KEY=VALUE item: {item}")
        env[key] = val
    return env


def shell_join(parts: Iterable[str]) -> str:
    return " ".join(shlex.quote(part) for part in parts)


def build_env_exports(env: Dict[str, str]) -> str:
    return " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())


def is_local_target(target: str) -> bool:
    if target in {"local", "localhost", "127.0.0.1"}:
        return True
    local_names = {socket.gethostname(), socket.getfqdn()}
    return target in local_names


def build_remote_command(target: str, command: str,
                         ssh_port: Optional[int] = None) -> List[str]:
    if is_local_target(target):
        return ["bash", "-lc", command]
    cmd = ["ssh"]
    if ssh_port is not None:
        cmd.extend(["-p", str(ssh_port)])
    cmd.extend([target, command])
    return cmd


class LogCapture:
    def __init__(self, process: subprocess.Popen[str], log_path: pathlib.Path) -> None:
        self.process = process
        self.log_path = log_path
        self.lines: List[str] = []
        self._queue: "queue.Queue[str]" = queue.Queue()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self) -> None:
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        with self.log_path.open("w", encoding="utf-8") as handle:
            assert self.process.stdout is not None
            for raw_line in self.process.stdout:
                handle.write(raw_line)
                handle.flush()
                self.lines.append(raw_line)
                self._queue.put(raw_line)

    def wait_for_substring(self, needle: str, timeout_s: float) -> bool:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if any(needle in line for line in self.lines):
                return True
            try:
                line = self._queue.get(timeout=0.1)
                if needle in line:
                    return True
            except queue.Empty:
                pass
            if self.process.poll() is not None:
                break
        return any(needle in line for line in self.lines)

    def finalize(self) -> None:
        self._thread.join(timeout=5)


def start_logged_process(target: str, command: str, log_path: pathlib.Path,
                         ssh_port: Optional[int] = None) -> LogCapture:
    proc = subprocess.Popen(
        build_remote_command(target, command, ssh_port=ssh_port),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    return LogCapture(proc, log_path)


def terminate_process(capture: LogCapture) -> None:
    if capture.process.poll() is not None:
        capture.finalize()
        return
    capture.process.terminate()
    try:
        capture.process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        capture.process.kill()
        capture.process.wait(timeout=5)
    capture.finalize()


def wait_process_ok(capture: LogCapture, timeout_s: float, role: str) -> None:
    try:
        capture.process.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired as exc:
        terminate_process(capture)
        raise RuntimeError(f"{role} timed out") from exc
    capture.finalize()
    if capture.process.returncode != 0:
        raise RuntimeError(f"{role} failed with exit code {capture.process.returncode}")


def parse_alps_rounds(text: str) -> List[Dict[str, str]]:
    rounds: List[Dict[str, str]] = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line.startswith("ALPS_KV_ROUND "):
            continue
        fields: Dict[str, str] = {}
        for token in line.split()[1:]:
            key, sep, value = token.partition("=")
            if sep:
                fields[key] = value
        rounds.append(fields)
    return rounds


def write_json(path: pathlib.Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_csv(path: pathlib.Path, rows: List[Dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = sorted({key for row in rows for key in row.keys()})
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def compose_env(base_env: Dict[str, str], case: ExperimentCase, extra_env: Dict[str, str]) -> Dict[str, str]:
    env = dict(base_env)
    env.update(case.env)
    env.update(extra_env)
    return env


def alps_server_command(binary: str, port: int, sizes: str, iters: int, warmup: int,
                        threads: int, timeout_ms: int, env: Dict[str, str],
                        workdir: str) -> str:
    cmd = [
        binary,
        "--mode", "server",
        "--port", str(port),
        "--sizes", sizes,
        "--iters", str(iters),
        "--warmup", str(warmup),
        "--threads", str(threads),
        "--timeout-ms", str(timeout_ms),
    ]
    return f"cd {shlex.quote(workdir)} && {build_env_exports(env)} {shell_join(cmd)}"


def alps_client_command(binary: str, server_host: str, port: int, sizes: str, iters: int,
                        warmup: int, threads: int, timeout_ms: int,
                        env: Dict[str, str], workdir: str) -> str:
    cmd = [
        binary,
        "--mode", "client",
        "--host", server_host,
        "--port", str(port),
        "--sizes", sizes,
        "--iters", str(iters),
        "--warmup", str(warmup),
        "--threads", str(threads),
        "--timeout-ms", str(timeout_ms),
    ]
    return f"cd {shlex.quote(workdir)} && {build_env_exports(env)} {shell_join(cmd)}"


def ucx_perftest_command(binary: str, test_name: str, size: str, iters: int, warmup: int,
                         port: int, env: Dict[str, str], server_host: Optional[str] = None,
                         extra_args: Optional[List[str]] = None,
                         workdir: str = str(ROOT)) -> str:
    cmd = [binary]
    if server_host:
        cmd.append(server_host)
    cmd.extend([
        "-t", test_name,
        "-s", size,
        "-n", str(iters),
        "-w", str(warmup),
        "-p", str(port),
        "-f",
        "-v",
    ])
    if extra_args:
        cmd.extend(extra_args)
    return f"cd {shlex.quote(workdir)} && {build_env_exports(env)} {shell_join(cmd)}"


def build_manifest(args: argparse.Namespace, suite: str, cases: List[ExperimentCase]) -> Dict[str, object]:
    serializable_args = {
        key: value
        for key, value in vars(args).items()
        if key != "func"
    }
    return {
        "suite": suite,
        "generated_at": int(time.time()),
        "args": serializable_args,
        "cases": [
            dataclasses.asdict(case)
            for case in cases
        ],
    }


def run_alps_matrix(args: argparse.Namespace) -> int:
    cases = build_alps_matrix_cases(
        proto_modes=parse_csv_list(args.proto_modes),
        rma_rails=parse_int_csv_list(args.rma_rails),
    )
    out_dir = pathlib.Path(args.out_dir)
    base_env = {
        "UCX_NET_DEVICES": args.rdma_device,
        "UCX_TLS": args.ucx_tls,
    }
    extra_env = parse_extra_env(args.extra_env)
    manifest = build_manifest(args, "alps", cases)
    write_json(out_dir / "manifest.json", manifest)

    summary_rows: List[Dict[str, str]] = []
    for case in cases:
        case_dir = out_dir / case.name
        case_dir.mkdir(parents=True, exist_ok=True)
        case_env = compose_env(base_env, case, extra_env)
        for repeat in range(1, args.repeats + 1):
            run_dir = case_dir / f"run-{repeat:02d}"
            run_dir.mkdir(parents=True, exist_ok=True)
            server_cmd = alps_server_command(
                binary=args.alps_binary,
                port=args.port,
                sizes=args.sizes,
                iters=args.iters,
                warmup=args.warmup,
                threads=args.threads,
                timeout_ms=args.timeout_ms,
                env=case_env,
                workdir=args.server_workdir,
            )
            client_cmd = alps_client_command(
                binary=args.alps_binary,
                server_host=args.server_rdma_ip,
                port=args.port,
                sizes=args.sizes,
                iters=args.iters,
                warmup=args.warmup,
                threads=args.threads,
                timeout_ms=args.timeout_ms,
                env=case_env,
                workdir=args.client_workdir,
            )
            if args.dry_run:
                write_json(
                    run_dir / "dry_run.json",
                    {
                        "server_target": args.server_target,
                        "server_cmd": server_cmd,
                        "client_target": args.client_target,
                        "client_cmd": client_cmd,
                    },
                )
                continue

            server_capture = start_logged_process(
                args.server_target,
                server_cmd,
                run_dir / "server.log",
                ssh_port=args.server_ssh_port,
            )
            try:
                if not server_capture.wait_for_substring("ALPS_KV_LISTEN", args.server_ready_timeout_s):
                    raise RuntimeError(f"server did not become ready for case {case.name}")
                client_capture = start_logged_process(
                    args.client_target,
                    client_cmd,
                    run_dir / "client.log",
                    ssh_port=args.client_ssh_port,
                )
                try:
                    wait_process_ok(client_capture, args.client_timeout_s, "client")
                    wait_process_ok(server_capture, args.server_timeout_s, "server")
                except Exception:
                    terminate_process(client_capture)
                    terminate_process(server_capture)
                    raise
            except Exception:
                terminate_process(server_capture)
                raise

            for role, capture in (("server", server_capture), ("client", client_capture)):
                rounds = parse_alps_rounds("".join(capture.lines))
                for fields in rounds:
                    row = dict(fields)
                    row["case"] = case.name
                    row["repeat"] = str(repeat)
                    row["log"] = str((run_dir / f"{role}.log").resolve())
                    row["target"] = args.server_target if role == "server" else args.client_target
                    summary_rows.append(row)

    write_csv(out_dir / "summary.csv", summary_rows)
    return 0


def run_ucx_matrix(args: argparse.Namespace) -> int:
    cases = build_ucx_matrix_cases(
        proto_modes=parse_csv_list(args.proto_modes),
        rma_rails=parse_int_csv_list(args.rma_rails),
        perftest=args.perftest,
    )
    out_dir = pathlib.Path(args.out_dir)
    base_env = {
        "UCX_NET_DEVICES": args.rdma_device,
        "UCX_TLS": args.ucx_tls,
    }
    extra_env = parse_extra_env(args.extra_env)
    manifest = build_manifest(args, "ucx", cases)
    write_json(out_dir / "manifest.json", manifest)

    summary_rows: List[Dict[str, str]] = []
    for case in cases:
        case_dir = out_dir / case.name
        case_dir.mkdir(parents=True, exist_ok=True)
        case_env = compose_env(base_env, case, extra_env)
        for size in parse_csv_list(args.sizes):
            for repeat in range(1, args.repeats + 1):
                run_dir = case_dir / size / f"run-{repeat:02d}"
                run_dir.mkdir(parents=True, exist_ok=True)
                server_cmd = ucx_perftest_command(
                    binary=args.ucx_perftest,
                    test_name=case.perftest,
                    size=size,
                    iters=args.iters,
                    warmup=args.warmup,
                    port=args.port,
                    env=case_env,
                    extra_args=parse_csv_list(args.perftest_extra_args),
                    workdir=args.server_workdir,
                )
                client_cmd = ucx_perftest_command(
                    binary=args.ucx_perftest,
                    test_name=case.perftest,
                    size=size,
                    iters=args.iters,
                    warmup=args.warmup,
                    port=args.port,
                    env=case_env,
                    server_host=args.server_rdma_ip,
                    extra_args=parse_csv_list(args.perftest_extra_args),
                    workdir=args.client_workdir,
                )
                if args.dry_run:
                    write_json(
                        run_dir / "dry_run.json",
                        {
                            "server_target": args.server_target,
                            "server_cmd": server_cmd,
                            "client_target": args.client_target,
                            "client_cmd": client_cmd,
                        },
                    )
                    continue

                server_capture = start_logged_process(
                    args.server_target,
                    server_cmd,
                    run_dir / "server.log",
                    ssh_port=args.server_ssh_port,
                )
                try:
                    time.sleep(args.server_start_delay_s)
                    client_capture = start_logged_process(
                        args.client_target,
                        client_cmd,
                        run_dir / "client.log",
                        ssh_port=args.client_ssh_port,
                    )
                    try:
                        wait_process_ok(client_capture, args.client_timeout_s, "client")
                        wait_process_ok(server_capture, args.server_timeout_s, "server")
                    except Exception:
                        terminate_process(client_capture)
                        terminate_process(server_capture)
                        raise
                except Exception:
                    terminate_process(server_capture)
                    raise

                result_line = ""
                for line in reversed(client_capture.lines):
                    if line.strip():
                        result_line = line.strip()
                        break
                summary_rows.append({
                    "case": case.name,
                    "size": size,
                    "repeat": str(repeat),
                    "perftest": case.perftest,
                    "result_line": result_line,
                    "client_log": str((run_dir / "client.log").resolve()),
                    "server_log": str((run_dir / "server.log").resolve()),
                })

    write_csv(out_dir / "summary.csv", summary_rows)
    return 0


def add_common_matrix_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--server-target", default="local",
                        help="ssh target for the server side, or 'local'")
    parser.add_argument("--client-target", default="local",
                        help="ssh target for the client side, or 'local'")
    parser.add_argument("--server-ssh-port", type=int, default=None,
                        help="optional SSH port for the server target")
    parser.add_argument("--client-ssh-port", type=int, default=None,
                        help="optional SSH port for the client target")
    parser.add_argument("--server-workdir", default=str(ROOT),
                        help="working directory on the server target")
    parser.add_argument("--client-workdir", default=str(ROOT),
                        help="working directory on the client target")
    parser.add_argument("--server-rdma-ip", required=True,
                        help="server RDMA IP used by the client/perftest")
    parser.add_argument("--rdma-device", required=True,
                        help="UCX_NET_DEVICES value, for example roce150s0f0:1")
    parser.add_argument("--ucx-tls", default="rc,sm,self",
                        help="UCX_TLS value")
    parser.add_argument("--proto-modes", default="y,n",
                        help="comma-separated UCX_PROTO_ENABLE values")
    parser.add_argument("--rma-rails", default="1,2",
                        help="comma-separated UCX_MAX_RMA_RAILS values")
    parser.add_argument("--extra-env", action="append", default=[],
                        help="extra KEY=VALUE env var, may be repeated")
    parser.add_argument("--out-dir", required=True,
                        help="directory for logs, manifest and summary")
    parser.add_argument("--port", type=int, default=16000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--server-timeout-s", type=float, default=180.0)
    parser.add_argument("--client-timeout-s", type=float, default=180.0)
    parser.add_argument("--dry-run", action="store_true",
                        help="write commands without executing them")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run or plan ALPS/UCX performance experiment matrices.")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    alps = subparsers.add_parser("run-alps-matrix", help="run alps_kv_bench matrix")
    add_common_matrix_args(alps)
    alps.add_argument("--alps-binary", default="./build/alps_kv_bench")
    alps.add_argument("--sizes", default="16M,32M,64M")
    alps.add_argument("--iters", type=int, default=50)
    alps.add_argument("--warmup", type=int, default=5)
    alps.add_argument("--threads", type=int, default=1)
    alps.add_argument("--timeout-ms", type=int, default=5000)
    alps.add_argument("--server-ready-timeout-s", type=float, default=20.0)
    alps.set_defaults(func=run_alps_matrix)

    ucx = subparsers.add_parser("run-ucx-matrix", help="run ucx_perftest matrix")
    add_common_matrix_args(ucx)
    ucx.add_argument("--ucx-perftest", default="ucx_perftest")
    ucx.add_argument("--sizes", default="16777216,33554432,67108864",
                     help="comma-separated byte sizes for ucx_perftest -s")
    ucx.add_argument("--iters", type=int, default=1000)
    ucx.add_argument("--warmup", type=int, default=100)
    ucx.add_argument("--perftest", default="ucp_put_bw")
    ucx.add_argument("--perftest-extra-args", default="",
                     help="comma-separated extra ucx_perftest args, e.g. -T,4")
    ucx.add_argument("--server-start-delay-s", type=float, default=2.0)
    ucx.set_defaults(func=run_ucx_matrix)

    plan = subparsers.add_parser("print-plan", help="print case plan as JSON")
    plan.add_argument("--suite", choices=["alps", "ucx"], required=True)
    plan.add_argument("--proto-modes", default="y,n")
    plan.add_argument("--rma-rails", default="1,2")
    plan.add_argument("--perftest", default="ucp_put_bw")

    def print_plan(args: argparse.Namespace) -> int:
        if args.suite == "alps":
            cases = build_alps_matrix_cases(parse_csv_list(args.proto_modes),
                                            parse_int_csv_list(args.rma_rails))
        else:
            cases = build_ucx_matrix_cases(parse_csv_list(args.proto_modes),
                                           parse_int_csv_list(args.rma_rails),
                                           args.perftest)
        json.dump([dataclasses.asdict(case) for case in cases], sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0

    plan.set_defaults(func=print_plan)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
