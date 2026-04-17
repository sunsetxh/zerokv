# Cluster Next Steps TODO

## 当前状态

已完成：
- master/slave 控制面注册与成员表广播
- membership 到 route cache 的基础同步
- `send(alias, ...)` 与 `recv_any(...)` 的最小可用路径
- master/slave 与 slave/slave 的 alias 消息互通
- oversized control frame 拒绝路径

当前测试状态：
- `ctest --test-dir build --output-on-failure`
- 10/10 通过

## TODO

### 高优先级

1. 清理 `slave_sessions_` 中已断开的 session
   - 文件：`src/cluster.cpp`
   - 问题：当前断开后的 session 会保留在 `slave_sessions_` 中，长期运行会积累，广播遍历成本持续变大。

2. 为控制面 header 使用显式字节序编码
   - 文件：`src/cluster.cpp`
   - 问题：`ControlFrameHeader` 仍使用 native endian，而 payload 已经是显式 little-endian，跨架构不一致。

3. 生成真实 `node_id`
   - 文件：`src/cluster.cpp`
   - 问题：当前 `node_id` 为空字符串，后续 stale node 检测与 reconnect 替换逻辑无法实现。

4. 暴露 `bound_control_addr()` 公共访问器
   - 文件：`include/axon/cluster.h`、`src/cluster.cpp`
   - 问题：测试目前依赖“预留端口再释放”的 workaround，存在 TOCTOU 风险。

### MessageKV / 大包性能

5. 优先推广 `send_region()` 和 region 复用
   - 文件：`include/zerokv/message_kv.h`、`src/message_kv.cpp`、`examples/message_kv_demo.cpp`
   - 背景：`1MiB+` 场景下，发送端额外的本地拷贝和 region 准备成本开始明显。
   - 目标：让场景层默认走零拷贝发送，复用预注册 `MemoryRegion`，避免每轮重复分配和注册。

6. 为 `recv_batch()` 引入异步 `fetch_to` / 多 outstanding gets
   - 文件：`src/message_kv.cpp`、`src/kv/node.cpp`
   - 背景：当前 `wait_for_any + per-key ack` 已去掉整批 barrier，但大包阶段仍明显落后于 `kv_bench --mode bench-fetch-to`。
   - 目标：对多个已 ready key 并行发起 `fetch_to`，充分利用带宽，减少串行收敛窗口。

7. 继续瘦身消息语义附带的控制面成本
   - 文件：`src/message_kv.cpp`、`src/kv/node.cpp`
   - 背景：`message_kv` 相比纯 `fetch_to` 仍包含 publish / subscribe / ack / cleanup 语义。
   - 目标：减少额外 metadata 查找、ack 发布和 cleanup 路径上的固定成本，重点优化 `1MiB+` 区间。

8. 把 `KVNode::push()` 的 commit 通知从 TCP 控制面迁到 UCX AM
   - 文件：`src/core/node.cpp`、`src/core/protocol.h`、`src/core/protocol.cpp`、`src/config.cpp`、`src/transport/worker.cpp`
   - 背景：当前 push 路径是 `GetPushTarget -> ReservePushInbox -> put+flush -> PushCommit(TCP)`；其中 `PushCommit` 更像“写完后的轻量通知”，适合先迁到数据面。
   - 目标：第一阶段保留 `reserve`，仅把 `commit` 改成 `UCX AM + ack`，减少一次控制面 RPC 和临时 TCP 往返。
   - 备注：建议在 reserve 响应中引入 `reservation_id`，后续 `PushCommitAm/AckAm` 只传轻量标识；AM callback 只入队，不直接做 finalize 重活。

9. 预热与对象复用继续系统化
   - 文件：`examples/message_kv_demo.cpp`、后续场景封装
   - 背景：`--warmup-rounds` 和跨轮复用已经证明对 steady-state 延迟有显著收益。
   - 目标：把连接、订阅、peer endpoint 和收发 region 的预热/池化做成更稳定的默认策略。

### 测试补充

10. 增加 `min_cluster_size > 1` 行为测试
   - 文件：`tests/integration/test_cluster_discovery.cpp`
   - 目标：验证 master 在成员数未达标前保持 `kWaitingMembership`，达标后再进入 `kReady`。

11. 增加三节点 membership 广播测试
   - 文件：`tests/integration/test_cluster_discovery.cpp`
   - 目标：`slave1` 下线后，`slave2` 也能收到最新成员表并更新路由视图。

12. 增加 malformed frame 测试
   - 文件：`tests/integration/test_cluster_discovery.cpp`
   - 目标：覆盖错误 magic、错误版本、损坏 header 等拒绝路径。

### 后续阶段

13. 把 route refresh 从“同步直连”收敛为更明确的连接状态机
   - 文件：`src/cluster.cpp`
   - 目标：为连接重试、失败状态、重连与故障摘除留出稳定结构。

14. 实现 stale node / reconnect 替换策略
   - 文件：`src/cluster.cpp`
   - 依赖：真实 `node_id`

15. 进一步收敛 `wait_ready()` 语义
   - 文件：`include/axon/cluster.h`、`src/cluster.cpp`
   - 目标：把“控制面 ready”和“数据面 route ready”的边界定义得更清楚。

16. 记录 KV 多网卡演进路径
   - 背景：UCX 本身支持 multi-rail，但当前 AXON KV 只注册一个 `data_addr`
   - 现状：当前只能通过单 `data_addr` + 单 endpoint 间接受益于 UCX 底层自动能力，AXON 本身并不显式建模多 NIC
   - 建议顺序：
     1. 节点注册多个 `data_addr`
     2. 每个 key 选择单个 NIC
     3. 再考虑单对象跨 NIC striping
