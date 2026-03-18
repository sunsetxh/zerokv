#pragma once

/// @file src/transport/ucx_impl_notes.h
/// @brief Internal header documenting key UCX implementation strategies.
///
/// NOT part of the public API.  This file contains implementation notes,
/// data structures, and patterns for the UCX transport backend.

/*
=============================================================================
 KEY IMPLEMENTATION CHALLENGES AND SOLUTIONS
=============================================================================

1. UCX WORKER THREAD SAFETY
============================

Problem:
  ucp_worker_h is NOT thread-safe.  Multiple threads calling
  ucp_worker_progress() or posting operations concurrently will corrupt
  internal state.

Solution: "One Worker Per Thread" + Lock-Free Wakeup

  - Each Worker is pinned to exactly one thread.  All endpoints created from
    that Worker must only be accessed from the same thread.

  - For cross-thread operation submission, we use a lock-free MPSC queue:

      struct WorkerImpl {
          ucp_worker_h            ucp_worker;
          MPSCQueue<WorkItem>     pending_queue;   // lock-free
          std::atomic<bool>       running{true};
          int                     event_fd;         // for epoll wakeup
      };

  - External threads enqueue work items; the worker thread dequeues and
    executes them during progress():

      // Called from any thread:
      void submit_from_external(WorkItem item) {
          pending_queue.push(std::move(item));
          // Wake up the worker if it's blocked in epoll_wait.
          uint64_t val = 1;
          write(event_fd, &val, sizeof(val));
      }

      // Called from the worker thread only:
      bool progress() {
          // 1. Drain the cross-thread submission queue
          while (auto item = pending_queue.try_pop()) {
              execute(*item);
          }
          // 2. Drive UCX progress
          return ucp_worker_progress(ucp_worker) != 0;
      }

  - For the Python asyncio integration, the worker's ucp_worker_get_efd()
    file descriptor is registered with the event loop via add_reader().
    When UCX signals readiness, the loop calls progress().


2. LARGE MESSAGE (1GB) ZERO-COPY TRANSFER PATH
===============================================

Problem:
  Transferring 1 GiB without intermediate copies.  The naive path
  (memcpy into a pre-registered buffer) doubles memory pressure and
  halves throughput.

Solution: UCX Rendezvous Protocol + Pre-Registration

  Path for pre-registered MemoryRegion (optimal):
    Sender                          Receiver
    ------                          --------
    tag_send(region, tag)           tag_recv(region, tag)
       |                               |
       v                               v
    ucp_tag_send_nbx()             ucp_tag_recv_nbx()
       |                               |
       +------ rendezvous header ------>|
       |<----- RDMA read (rkey) -------+
       |                               |
       v                               v
    completion callback             completion callback

  - For pre-registered MemoryRegion: the rkey is already available,
    UCX performs a direct RDMA read from sender to receiver.  ZERO copies.

  - For raw buffer (not pre-registered): the transport layer checks the
    RegistrationCache first.  On cache miss, it calls ucp_mem_map() to
    pin the buffer, then proceeds with rendezvous.  The registration is
    cached for future reuse.

  Threshold selection:
    - < 8 KB:   Eager (inline copy into the message header).
    - 8-256 KB: Eager with zcopy (registered + inline).
    - > 256 KB: Rendezvous (RDMA read, true zero-copy).

  These thresholds are configurable via:
    UCX_RNDV_THRESH=262144
    UCX_ZCOPY_THRESH=8192


3. PYTHON GIL AND PERFORMANCE
==============================

Problem:
  The GIL serialises Python threads.  A naive implementation that holds
  the GIL during UCX operations would prevent concurrent progress.

Solution: Multi-layer GIL Release Strategy

  a) Release GIL during blocking operations:
     All C++ functions that may block (wait, progress, send, recv) are
     wrapped with pybind11's `py::call_guard<py::gil_scoped_release>()`.

     Example pybind11 binding:
       m.def("tag_send", &Endpoint::tag_send,
             py::call_guard<py::gil_scoped_release>());

  b) asyncio integration avoids GIL contention entirely:
     - Worker.attach_to_event_loop() registers event_fd with asyncio.
     - When the fd is readable, a thin Python callback calls progress()
       (acquires GIL briefly, releases inside progress(), re-acquires).
     - All data operations (send/recv) release the GIL for the duration.

  c) Buffer protocol interaction:
     - When accepting numpy/cupy arrays, we extract the buffer pointer
       and length WHILE holding the GIL (via PyObject_GetBuffer), then
       release the GIL before calling into UCX.
     - Critical: we increment the reference count of the Python buffer
       object to prevent garbage collection during the async operation.

  d) Dedicated progress thread (alternative to asyncio):
     - Spawn a C++ thread that calls worker->run() in a tight loop.
     - This thread never touches Python objects and never needs the GIL.
     - Completion callbacks re-acquire the GIL only if they need to
       invoke Python callbacks.

  Performance impact measurements (typical):
     - GIL acquire/release: ~50-100 ns per transition
     - UCX progress() call: ~200-500 ns
     - RDMA send 4KB: ~2-5 us
     - So GIL overhead is <5% for messages >= 4KB.
     - For < 1KB messages at high rate, the dedicated thread approach
       avoids GIL transitions entirely on the hot path.


4. MEMORY REGISTRATION CACHE
==============================

Problem:
  ucp_mem_map() (memory registration/pinning) costs 10-100 us per call.
  For applications that repeatedly send from the same buffers (common in
  ML training), this overhead dominates small-message latency.

Solution: LRU Registration Cache with Address-Range Tracking

  Data structure:

    struct CacheEntry {
        uintptr_t    addr;
        size_t       length;
        MemoryType   mem_type;
        ucp_mem_h    mem_handle;
        uint64_t     last_access;  // for LRU
    };

    // Interval tree for O(log n) range lookups:
    //   "Is any part of [addr, addr+length) already registered?"
    class RegistrationCacheImpl {
        IntervalTree<uintptr_t, CacheEntry*> tree_;
        LRUList<CacheEntry*>                 lru_;
        size_t                               max_entries_;
        std::mutex                           mu_;
    };

  Lookup algorithm:
    1. Query interval tree for overlapping ranges.
    2. If exact match found -> cache hit, return existing handle.
    3. If partial overlap -> merge: deregister old, register union range.
    4. If no overlap -> cache miss, register new, insert, maybe evict LRU.

  Eviction:
    - When cache exceeds max_entries_, evict the least-recently-used entry.
    - Before eviction, check that no in-flight operation references the entry.
      (Reference counting on CacheEntry.)

  Invalidation:
    - User calls invalidate(addr, len) before freeing the buffer.
    - munmap / cudaFree hooks (on Linux, intercept via LD_PRELOAD or
      __malloc_hook) for automatic invalidation.

  Statistics:
    - Cache hit ratio is exposed via RegistrationCache::hits()/misses().
    - Typical hit rates > 95% for ML training workloads.

*/

#include <ucp/api/ucp.h>

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace axon {
namespace internal {

// ---------------------------------------------------------------------------
// Lock-free MPSC queue for cross-thread work submission
// ---------------------------------------------------------------------------

template <typename T>
class MPSCQueue {
public:
    void push(T item) {
        auto* node = new Node{std::move(item), nullptr};
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    /// Try to pop from the consumer side (single consumer only).
    bool try_pop(T& out) {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next == nullptr) return false;
        out = std::move(next->value);
        tail_ = next;
        delete tail;
        return true;
    }

private:
    struct Node {
        T                    value;
        std::atomic<Node*>   next{nullptr};
    };

    // Sentinel node
    Node*              stub_ = new Node{T{}, nullptr};
    std::atomic<Node*> head_{stub_};
    Node*              tail_{stub_};
};

// ---------------------------------------------------------------------------
// Interval tree node for registration cache (simplified red-black tree)
// ---------------------------------------------------------------------------

struct RegCacheEntry {
    uintptr_t         addr;
    size_t            length;
    MemoryType        mem_type;
    ucp_mem_h         mem_handle;
    std::atomic<int>  ref_count{0};
};

// Full IntervalTree implementation would go here (omitted for brevity;
// use boost::icl::interval_map or a custom augmented BST).

}  // namespace internal
}  // namespace axon
