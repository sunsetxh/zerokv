from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
INIT_FILE = ROOT / "python" / "axon" / "__init__.py"
STUB_FILE = ROOT / "python" / "axon" / "_core.pyi"


class PythonApiSurfaceTest(unittest.TestCase):
    def test_init_exports_modern_kv_api(self) -> None:
        content = INIT_FILE.read_text(encoding="utf-8")
        for symbol in [
            "KVServer",
            "KVNode",
            "KeyInfo",
            "FetchResult",
            "PublishMetrics",
            "FetchMetrics",
            "PushMetrics",
            "SubscriptionEvent",
            "SubscriptionEventType",
            "Status",
        ]:
            self.assertIn(f'"{symbol}"', content, f"{symbol} should be exported from axon.__init__")

    def test_stub_declares_modern_kv_api(self) -> None:
        content = STUB_FILE.read_text(encoding="utf-8")
        for snippet in [
            "class Status:",
            "class KVServer:",
            "class KVNode:",
            "class KeyInfo:",
            "class FetchResult:",
            "class PublishMetrics:",
            "class FetchMetrics:",
            "class PushMetrics:",
            "class SubscriptionEventType",
            "class SubscriptionEvent:",
            "def publish(",
            "def fetch(",
            "def push(",
            "def subscribe(",
            "def unsubscribe(",
            "def drain_subscription_events(",
            "def last_publish_metrics(",
            "def last_fetch_metrics(",
            "def last_push_metrics(",
        ]:
            self.assertIn(snippet, content, f"Missing stub surface: {snippet}")


if __name__ == "__main__":
    unittest.main()
