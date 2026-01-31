#!/usr/bin/env python3
"""
ZeroKV Python Example

This example demonstrates:
1. Starting an ZeroKV Server
2. Connecting a client and retrieving data
3. Using NumPy arrays and PyTorch tensors
4. Monitoring performance
"""

import sys
import time
import numpy as np
import zerokv

# Try importing PyTorch
try:
    import torch
    TORCH_AVAILABLE = True
except ImportError:
    print("Warning: PyTorch not available. Skipping PyTorch examples.")
    TORCH_AVAILABLE = False


def server_example():
    """Example of using ZeroKV Server"""
    print("=" * 60)
    print("Server Example")
    print("=" * 60)

    # Create and start server
    server = zerokv.Server(device_id=0)
    server.start("0.0.0.0", 50051)
    print("Server started on 0.0.0.0:50051\n")

    # Register NumPy array
    print("Registering NumPy arrays...")
    embeddings = np.random.randn(1024, 768).astype(np.float32)
    server.put("embeddings", embeddings)
    print(f"  Registered 'embeddings': shape={embeddings.shape}, dtype={embeddings.dtype}")

    weights = np.random.randn(512, 512).astype(np.float32)
    server.put("weights", weights)
    print(f"  Registered 'weights': shape={weights.shape}, dtype={weights.dtype}")

    # Register PyTorch tensor (if available)
    if TORCH_AVAILABLE and torch.npu.is_available():
        print("\nRegistering PyTorch tensors...")
        torch.npu.set_device(0)
        tensor = torch.randn(256, 256, device='npu:0')
        server.put("torch_data", tensor)
        print(f"  Registered 'torch_data': shape={tensor.shape}, device={tensor.device}")

    # Get server statistics
    print("\nServer is ready for client connections.")
    print("Press Ctrl+C to stop the server.\n")

    # Start monitoring
    monitor = server.get_monitor()

    try:
        while True:
            time.sleep(5)

            # Print statistics every 5 seconds
            stats = monitor.get_all_stats()
            if stats:
                print("\n--- Performance Statistics ---")
                for op, op_stats in stats.items():
                    if op_stats['total_ops'] > 0:
                        print(f"{op}:")
                        print(f"  Total Ops: {op_stats['total_ops']}")
                        print(f"  Success Rate: {100.0 * op_stats['success_ops'] / op_stats['total_ops']:.2f}%")
                        print(f"  P95 Latency: {op_stats['p95_latency_us']:.2f} us")
                        print(f"  Throughput: {op_stats['throughput_mbps']:.2f} MB/s")
                print("-----------------------------\n")

    except KeyboardInterrupt:
        print("\nShutting down server...")
        server.shutdown()
        print("Server stopped.")


def client_example():
    """Example of using ZeroKV Client"""
    print("=" * 60)
    print("Client Example")
    print("=" * 60)

    # Wait for server to be ready
    print("Waiting for server to start...")
    time.sleep(2)

    # Create and connect client
    client = zerokv.Client(device_id=1)
    client.connect("127.0.0.1", 50051)
    print("Connected to server at 127.0.0.1:50051\n")

    # Get NumPy array
    print("Retrieving 'embeddings' from server...")
    start_time = time.time()
    embeddings = client.get("embeddings")
    elapsed = time.time() - start_time

    print(f"  Shape: {embeddings.shape}")
    print(f"  Dtype: {embeddings.dtype}")
    print(f"  Transfer time: {elapsed * 1e6:.2f} us")
    print(f"  Size: {embeddings.nbytes / (1024 * 1024):.2f} MB")
    print(f"  Bandwidth: {(embeddings.nbytes / (1024 * 1024)) / elapsed:.2f} MB/s\n")

    # Get PyTorch tensor (if available)
    if TORCH_AVAILABLE and torch.npu.is_available():
        print("Retrieving 'torch_data' from server...")
        torch.npu.set_device(1)
        tensor = client.get_torch("torch_data")
        print(f"  Shape: {tensor.shape}")
        print(f"  Device: {tensor.device}")
        print(f"  Dtype: {tensor.dtype}\n")

    # Run multiple Get operations to test performance
    print("Running 10 Get operations for performance test...")
    for i in range(10):
        data = client.get("weights")
        print(f"  Get #{i + 1}: {data.shape}")

    # Print performance statistics
    monitor = client.get_monitor()
    stats = monitor.get_stats('GET')

    print("\n--- Client Performance Statistics ---")
    print(f"Total GET Ops: {stats['total_ops']}")
    print(f"Success Ops: {stats['success_ops']}")
    print(f"Failed Ops: {stats['failed_ops']}")
    print(f"Avg Latency: {stats['avg_latency_us']:.2f} us")
    print(f"P50 Latency: {stats['p50_latency_us']:.2f} us")
    print(f"P95 Latency: {stats['p95_latency_us']:.2f} us")
    print(f"P99 Latency: {stats['p99_latency_us']:.2f} us")
    print(f"Throughput: {stats['throughput_mbps']:.2f} MB/s")
    print("------------------------------------\n")

    # Disconnect
    client.disconnect()
    print("Client disconnected.")


def benchmark_example():
    """Benchmark example"""
    print("=" * 60)
    print("Benchmark Example")
    print("=" * 60)

    # Start server
    server = zerokv.Server(device_id=0)
    server.start("0.0.0.0", 50051)
    print("Server started\n")

    # Register test data of different sizes
    sizes = [1024, 10240, 102400, 1024000]  # 1KB, 10KB, 100KB, 1MB
    for size in sizes:
        data = np.random.randn(size).astype(np.float32)
        server.put(f"data_{size}", data)
        print(f"Registered 'data_{size}': {data.nbytes / 1024:.2f} KB")

    time.sleep(1)

    # Connect client
    client = zerokv.Client(device_id=1)
    client.connect("127.0.0.1", 50051)
    print("\nClient connected\n")

    # Benchmark different data sizes
    print("Running benchmarks...\n")
    print(f"{'Size':<15} {'Iterations':<12} {'Avg Latency (us)':<20} {'Throughput (MB/s)':<20}")
    print("-" * 70)

    for size in sizes:
        iterations = max(10, 1000000 // size)  # More iterations for smaller sizes

        start_time = time.time()
        for _ in range(iterations):
            data = client.get(f"data_{size}")
        elapsed = time.time() - start_time

        avg_latency_us = (elapsed / iterations) * 1e6
        throughput_mbps = (data.nbytes * iterations / (1024 * 1024)) / elapsed

        size_str = f"{data.nbytes / 1024:.0f} KB" if data.nbytes < 1024 * 1024 else f"{data.nbytes / (1024 * 1024):.2f} MB"
        print(f"{size_str:<15} {iterations:<12} {avg_latency_us:<20.2f} {throughput_mbps:<20.2f}")

    # Cleanup
    client.disconnect()
    server.shutdown()
    print("\nBenchmark complete.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python python_example.py [server|client|benchmark]")
        sys.exit(1)

    mode = sys.argv[1]

    if mode == "server":
        server_example()
    elif mode == "client":
        client_example()
    elif mode == "benchmark":
        benchmark_example()
    else:
        print(f"Unknown mode: {mode}")
        print("Available modes: server, client, benchmark")
        sys.exit(1)
