#!/usr/bin/env python3
"""
examples/python_usage.py
Demonstrates the Python API for the AXON transport library.
"""

import asyncio
import numpy as np

import zerokv

# ============================================================================
# Example 1: Basic asyncio send/recv
# ============================================================================
async def example_basic():
    """Tag-matched send/recv with asyncio."""
    ctx = zerokv.Context(
        transport="ucx",
        num_workers=2,
        memory_pool_size=128 * 1024 * 1024,
        registration_cache=True,
        ucx_tls="rc,ud,shm,self",
    )
    worker = ctx.create_worker()
    worker.attach_to_event_loop()  # auto-progress via asyncio

    # Connect
    ep = await worker.connect("192.168.1.2:13337")

    # Send a numpy array (zero-copy via buffer protocol)
    data = np.ones((1024, 1024), dtype=np.float32)
    await ep.tag_send(data, tag=zerokv.make_tag(0, 42))

    # Receive into a pre-allocated buffer
    recv_buf = np.empty((1024, 1024), dtype=np.float32)
    nbytes, matched_tag = await ep.tag_recv(recv_buf, tag=42)

    print(f"Received {nbytes} bytes, tag={zerokv.tag_user(matched_tag)}")
    print(f"Data matches: {np.array_equal(data, recv_buf)}")

    await ep.close()


# ============================================================================
# Example 2: GPU transfer with cupy
# ============================================================================
async def example_gpu():
    """Zero-copy GPU-to-GPU transfer using cupy arrays."""
    import cupy as cp

    ctx = zerokv.Context(transport="ucx")
    worker = ctx.create_worker()
    worker.attach_to_event_loop()

    ep = await worker.connect("192.168.1.2:13337")

    # cupy arrays on GPU - buffer protocol exposes device pointer
    gpu_data = cp.random.randn(1024, 1024).astype(cp.float32)
    await ep.tag_send(gpu_data, tag=100)

    # Receive directly into GPU memory
    gpu_recv = cp.empty((1024, 1024), dtype=cp.float32)
    await ep.tag_recv(gpu_recv, tag=100)

    print(f"GPU transfer complete, allclose={cp.allclose(gpu_data, gpu_recv)}")
    await ep.close()


# ============================================================================
# Example 3: Pre-registered memory for hot-path transfers
# ============================================================================
async def example_registered_memory():
    """Using MemoryRegion for pre-registered zero-copy transfers."""
    ctx = zerokv.Context(transport="ucx")
    worker = ctx.create_worker()
    worker.attach_to_event_loop()

    ep = await worker.connect("10.0.0.1:13337")

    # Pre-register a large numpy array (avoids per-send registration overhead)
    big_array = np.zeros(1 << 30, dtype=np.uint8)  # 1 GiB
    region = zerokv.MemoryRegion.register(ctx, big_array)

    # Hot loop: send from pre-registered memory (true zero-copy path)
    for i in range(100):
        await ep.tag_send(region, tag=zerokv.make_tag(0, i))

    await ep.close()


# ============================================================================
# Example 4: Memory pool for many small transfers
# ============================================================================
async def example_memory_pool():
    """MemoryPool for efficient allocation of registered buffers."""
    ctx = zerokv.Context(transport="ucx")
    worker = ctx.create_worker()
    worker.attach_to_event_loop()

    ep = await worker.connect("10.0.0.1:13337")
    pool = zerokv.MemoryPool.create(ctx, pool_bytes=64 * 1024 * 1024)

    # Concurrent sends using pool-allocated buffers
    tasks = []
    for i in range(1000):
        buf = pool.allocate(4096)
        # write into the buffer via numpy view
        arr = np.frombuffer(buf, dtype=np.float32)
        arr[:] = float(i)

        task = asyncio.create_task(ep.tag_send(buf, tag=zerokv.make_tag(0, i)))
        tasks.append((task, buf))

    # Wait for all sends, then return buffers to pool
    for task, buf in tasks:
        await task
        pool.deallocate(buf)

    print(f"Pool usage: {pool.used_bytes}/{pool.total_bytes}")
    await ep.close()


# ============================================================================
# Example 5: RDMA one-sided put/get
# ============================================================================
async def example_rdma():
    """One-sided RDMA operations."""
    ctx = zerokv.Context(transport="ucx")
    worker = ctx.create_worker()
    worker.attach_to_event_loop()

    ep = await worker.connect("10.0.0.1:13337")

    # Allocate and register local memory
    local = zerokv.MemoryRegion.allocate(ctx, 4096)

    # Exchange remote key out-of-band (e.g., via tag_send/tag_recv)
    remote_addr: int = 0  # received from peer
    remote_key: bytes = b""  # received from peer

    # RDMA write
    await ep.put(local, remote_addr, remote_key)

    # RDMA read
    await ep.get(local, remote_addr, remote_key)

    # Ensure remote visibility
    await ep.flush()
    await ep.close()


# ============================================================================
# Example 6: Server with accept loop
# ============================================================================
async def example_server():
    """Server that accepts connections and echoes data."""
    ctx = zerokv.Context(transport="ucx")
    worker = ctx.create_worker()
    worker.attach_to_event_loop()

    async def handle_client(ep: zerokv.Endpoint):
        """Handle one client connection."""
        buf = np.empty(1024 * 1024, dtype=np.uint8)
        try:
            while ep.is_connected:
                nbytes, tag = await ep.tag_recv(buf, tag=zerokv.TAG_ANY)
                # Echo back
                await ep.tag_send(
                    memoryview(buf)[:nbytes], tag=tag
                )
        except zerokv.AXONError as e:
            if e.code != zerokv.ErrorCode.ENDPOINT_CLOSED:
                raise

    clients: list[asyncio.Task] = []

    def on_accept(ep: zerokv.Endpoint):
        task = asyncio.create_task(handle_client(ep))
        clients.append(task)

    listener = worker.listen(":13337", on_accept)
    print(f"Server listening on {listener.address}")

    # Run forever
    await asyncio.Event().wait()


# ============================================================================
# Example 7: Registration cache for dynamic buffers
# ============================================================================
async def example_registration_cache():
    """RegistrationCache amortises registration for repeated buffers."""
    ctx = zerokv.Context(transport="ucx")
    worker = ctx.create_worker()
    worker.attach_to_event_loop()

    ep = await worker.connect("10.0.0.1:13337")
    cache = zerokv.RegistrationCache.create(ctx, max_entries=1024)

    # Simulate dynamic buffer reuse
    buffers = [np.random.randn(4096).astype(np.float32) for _ in range(10)]

    for i in range(10000):
        buf = buffers[i % len(buffers)]
        # Cache handles registration: first call registers, subsequent calls are cache hits
        region = cache.get_or_register(buf)
        await ep.tag_send(region, tag=zerokv.make_tag(0, i))

    print(f"Cache stats: hits={cache.hits}, misses={cache.misses}")
    await ep.close()


# ============================================================================
# Example 8: Concurrent operations with asyncio.gather
# ============================================================================
async def example_concurrent():
    """Multiple concurrent send/recv using asyncio.gather."""
    ctx = zerokv.Context(transport="ucx")
    worker = ctx.create_worker()
    worker.attach_to_event_loop()

    ep = await worker.connect("10.0.0.1:13337")

    # Fire 10 sends concurrently
    arrays = [np.full(1024, i, dtype=np.float32) for i in range(10)]
    await asyncio.gather(
        *(ep.tag_send(arr, tag=zerokv.make_tag(0, i)) for i, arr in enumerate(arrays))
    )

    # Fire 10 receives concurrently
    recv_bufs = [np.empty(1024, dtype=np.float32) for _ in range(10)]
    results = await asyncio.gather(
        *(ep.tag_recv(buf, tag=zerokv.make_tag(0, i)) for i, buf in enumerate(recv_bufs))
    )

    for nbytes, matched_tag in results:
        print(f"  tag={zerokv.tag_user(matched_tag)}, bytes={nbytes}")

    await ep.close()


# ============================================================================
if __name__ == "__main__":
    import sys

    examples = {
        "basic": example_basic,
        "gpu": example_gpu,
        "registered": example_registered_memory,
        "pool": example_memory_pool,
        "rdma": example_rdma,
        "server": example_server,
        "cache": example_registration_cache,
        "concurrent": example_concurrent,
    }

    name = sys.argv[1] if len(sys.argv) > 1 else "basic"
    if name not in examples:
        print(f"Unknown example: {name}")
        print(f"Available: {', '.join(examples)}")
        sys.exit(1)

    asyncio.run(examples[name]())
