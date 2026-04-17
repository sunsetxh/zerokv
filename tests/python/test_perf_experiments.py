import importlib.util
import json
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT_PATH = ROOT / "scripts" / "perf_experiments.py"


def load_module():
    spec = importlib.util.spec_from_file_location("perf_experiments", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class PerfExperimentsTest(unittest.TestCase):
    def test_alps_matrix_contains_expected_cases(self):
        module = load_module()
        cases = module.build_alps_matrix_cases(proto_modes=["y", "n"], rma_rails=[1, 2])

        self.assertEqual(
            [case.name for case in cases],
            [
                "proto-y-rma1",
                "proto-y-rma2",
                "proto-n-rma1",
                "proto-n-rma2",
            ],
        )
        self.assertEqual(cases[0].env["UCX_PROTO_ENABLE"], "y")
        self.assertEqual(cases[0].env["UCX_MAX_RMA_RAILS"], "1")
        self.assertEqual(cases[-1].env["UCX_MAX_RMA_RAILS"], "2")

    def test_alps_matrix_sets_rndv_rails_to_match_rma_by_default(self):
        module = load_module()
        cases = module.build_alps_matrix_cases(proto_modes=["y"], rma_rails=[1, 2])

        self.assertEqual(cases[0].env["UCX_MAX_RNDV_RAILS"], "1")
        self.assertEqual(cases[1].env["UCX_MAX_RNDV_RAILS"], "2")

    def test_ucx_matrix_uses_ucp_put_bandwidth_default(self):
        module = load_module()
        cases = module.build_ucx_matrix_cases(proto_modes=["y"], rma_rails=[2])

        self.assertEqual(len(cases), 1)
        self.assertEqual(cases[0].name, "proto-y-rma2")
        self.assertEqual(cases[0].perftest, "ucp_put_bw")

    def test_manifest_is_json_serializable(self):
        module = load_module()
        parser = module.build_parser()
        args = parser.parse_args([
            "run-alps-matrix",
            "--server-rdma-ip", "10.0.0.1",
            "--rdma-device", "rxe0:1",
            "--out-dir", "/tmp/out",
            "--dry-run",
        ])
        manifest = module.build_manifest(
            args,
            "alps",
            module.build_alps_matrix_cases(["y"], [1]),
        )

        json.dumps(manifest)

    def test_build_remote_command_supports_ssh_port(self):
        module = load_module()

        self.assertEqual(
            module.build_remote_command("axon@192.168.3.9", "echo hi", ssh_port=2222),
            ["ssh", "-p", "2222", "axon@192.168.3.9", "echo hi"],
        )
        self.assertEqual(
            module.build_remote_command("local", "echo hi", ssh_port=2222),
            ["bash", "-lc", "echo hi"],
        )

    def test_alps_server_command_uses_explicit_workdir(self):
        module = load_module()

        command = module.alps_server_command(
            binary="./build/alps_kv_bench",
            port=16000,
            sizes="16M,32M,64M",
            iters=50,
            warmup=5,
            threads=1,
            timeout_ms=5000,
            env={"UCX_TLS": "rc,sm,self"},
            workdir="/tmp/alps-tree",
        )

        self.assertTrue(command.startswith("cd /tmp/alps-tree && "))


if __name__ == "__main__":
    unittest.main()
