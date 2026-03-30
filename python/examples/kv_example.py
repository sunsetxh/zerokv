#!/usr/bin/env python3

from __future__ import annotations

import argparse
import sys
import time

import axon


def make_config(transport: str) -> axon.Config:
    return axon.Config(transport=transport)


def run_server(args: argparse.Namespace) -> int:
    server = axon.KVServer(make_config(args.transport))
    status = server.start(args.listen)
    status.throw_if_error()
    print(f"kv server listening on {server.address}")
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()
    return 0


def run_publish(args: argparse.Namespace) -> int:
    node = axon.KVNode(make_config(args.transport))
    status = node.start(args.server_addr, args.data_addr, args.node_id)
    status.throw_if_error()
    try:
        fut = node.publish(args.key, args.value.encode("utf-8"))
        fut.get()
        fut.status.throw_if_error()
        print(f"published key={args.key} on node={node.node_id}")
        metrics = node.last_publish_metrics()
        if metrics is not None:
            print(
                "publish_metrics "
                f"total_us={metrics.total_us} "
                f"prepare_region_us={metrics.prepare_region_us} "
                f"pack_rkey_us={metrics.pack_rkey_us} "
                f"put_meta_rpc_us={metrics.put_meta_rpc_us}"
            )
        if args.hold:
            print("holding published key; press Ctrl-C to exit")
            while True:
                time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        node.stop()
    return 0


def run_fetch(args: argparse.Namespace) -> int:
    node = axon.KVNode(make_config(args.transport))
    status = node.start(args.server_addr, args.data_addr, args.node_id)
    status.throw_if_error()
    try:
        fut = node.fetch(args.key)
        result = fut.get()
        fut.status.throw_if_error()
        data = result.data
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = repr(data)
        print(
            f"fetched key={args.key} owner={result.owner_node_id} "
            f"version={result.version} size={len(data)} value={text}"
        )
        metrics = node.last_fetch_metrics()
        if metrics is not None:
            print(
                "fetch_metrics "
                f"total_us={metrics.total_us} "
                f"get_meta_rpc_us={metrics.get_meta_rpc_us} "
                f"peer_connect_us={metrics.peer_connect_us} "
                f"rdma_prepare_us={metrics.rdma_prepare_us} "
                f"rdma_get_us={metrics.rdma_get_us}"
            )
    finally:
        node.stop()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="AXON Python KV example")
    parser.add_argument("--mode", choices=["server", "publish", "fetch"], required=True)
    parser.add_argument("--transport", default="rdma")
    parser.add_argument("--listen", default="0.0.0.0:15000")
    parser.add_argument("--server-addr")
    parser.add_argument("--data-addr", default="0.0.0.0:0")
    parser.add_argument("--node-id", default="py-node")
    parser.add_argument("--key", default="demo-key")
    parser.add_argument("--value", default="hello-from-python")
    parser.add_argument("--hold", action="store_true")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.mode == "server":
        return run_server(args)

    if not args.server_addr:
        parser.error("--server-addr is required for publish/fetch")

    if args.mode == "publish":
        return run_publish(args)
    if args.mode == "fetch":
        return run_fetch(args)

    parser.error(f"unsupported mode: {args.mode}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
