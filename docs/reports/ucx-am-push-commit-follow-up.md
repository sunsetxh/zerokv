# UCX AM Push Commit Follow-up

## 背景

当前 `KVNode::push()` 的发送路径是：

1. 通过控制面 `GetPushTarget` 获取目标节点的 push inbox 信息
2. 通过目标节点的 `push_control_addr` 建立临时 TCP 连接
3. 发送 `ReservePushInbox`
4. 执行 `ucp_put_nbx + flush`
5. 发送 `PushCommit`
6. 等待 `PushCommitResp` 后返回

对应实现位于：

- `src/core/node.cpp`
- `src/core/protocol.h`
- `src/core/protocol.cpp`

当前 `Context::create()` 仅启用了 `UCP_FEATURE_TAG | UCP_FEATURE_STREAM | UCP_FEATURE_RMA | UCP_FEATURE_AMO64`，尚未启用 `UCP_FEATURE_AM`。

## 目标

在不改变 `push()` 成功语义的前提下，先把 `PushCommit` 这一步从 TCP 控制面迁移到 UCX Active Message，减少每次 push 的控制路径固定开销。

## 非目标

- 这一阶段不移除 `GetPushTarget`
- 这一阶段不移除 `ReservePushInbox`
- 不引入 fire-and-forget 语义
- 不直接改成 raw verbs / UCT 专用 fast path

## 为什么先改 commit，不改 reserve

当前接收端只有单个 `push_inbox_region_`，并用 `push_busy_` 做互斥保护。也就是说，发送端在 RDMA 写入前仍然需要先拿到一个“你现在可以写这个 inbox”的许可。

因此：

- `ReservePushInbox` 仍然承担并发安全职责，短期内不能直接删
- `PushCommit` 的职责更像“写完后的轻量通知”，更适合先迁移到 AM

这意味着第一阶段只能减少一次控制面 RPC，不能把 push 控制面完全消掉。

## 推荐方案

### Phase 1: `commit` 改为 `AM + ack`

保留现有 `reserve -> put + flush -> commit` 协议形状，但把最后的 commit/ack 改到 UCX 数据面上。

发送端：

1. 继续执行 `GetPushTarget`
2. 继续通过 TCP 执行 `ReservePushInbox`
3. 继续执行 `put + flush`
4. 改为通过 UCX endpoint 发送一个很小的 `PushCommitAm`
5. 等待远端返回 `PushCommitAckAm`
6. 收到 ack 后返回成功

接收端：

1. 继续在 TCP 侧处理 `ReservePushInbox`
2. 增加 UCX AM handler，用于接收 `PushCommitAm`
3. AM handler 只做校验和入队，不直接执行重活
4. 后台执行路径消费队列，复用现有 finalize 逻辑
5. 完成后通过 UCX endpoint 回复 `PushCommitAckAm`

### 为什么要保留 ack

当前 `push()` 的语义是“远端已经处理并确认成功”后再返回。若改成单向 AM 通知后立刻返回，会把 API 语义改成 fire-and-forget。这不是本阶段目标。

因此第一版必须保留同步确认，只是把确认通道从“临时 TCP 请求/响应”换成“已建立 UCX endpoint 上的小消息往返”。

## 建议的数据结构调整

不要把现有 `PushCommitRequest` 原样搬到 AM。建议在 reserve 响应中增加一个显式 `reservation_id`，后续 commit/ack 都只传这个轻量标识。

建议演进为：

- `ReservePushInboxResponse`
  - `status`
  - `message`
  - `reservation_id`
- `PushCommitAm`
  - `reservation_id`
- `PushCommitAckAm`
  - `reservation_id`
  - `status`
  - `message`

这样做的好处：

- AM 负载固定且很小
- 不需要重复带 `sender_node_id + key + value_size`
- 关联关系更明确，避免字段组合匹配带来的歧义

## 传输层改动范围

### Context

- 在 `Context::create()` 中增加 `UCP_FEATURE_AM`

### Worker / Endpoint

新增最小 AM 支持：

- `register_am_handler(am_id, callback, arg)`
- `am_send(ep, am_id, header, payload)`

如果后续需要严格区分 header/data，也可以先只支持单 buffer 小消息版本，满足 commit/ack 即可。

### KVNode

新增：

- push finalize 事件队列
- 接收 `PushCommitAm` 的 handler
- 将现有 `handle_push_commit()` 拆成：
  - 轻量校验/路由
  - 真正 finalize/publish 的执行函数

## 并发与执行模型

AM callback 不应直接做 `publish_impl()`、内存分配或阻塞等待。推荐模型是：

1. AM callback 校验 `reservation_id`
2. 构造一个 `PushFinalizeEvent`
3. 放入 `KVNode::Impl` 的队列
4. 唤醒后台线程或复用现有后台执行路径
5. 后台线程执行 finalize，再回复 ack

这样可以避免在 UCX progress 上下文中做重活，降低卡住 worker progress 的风险。

## 预期收益

### 能减少什么

- 少一次 `PushCommit` 的 TCP 请求/响应
- 少一次临时 TCP 控制连接上的发送/接收/关闭成本
- 把 commit 通知复用到已建立的 UCX endpoint 上

### 不能减少什么

- `GetPushTarget` 仍然存在
- `ReservePushInbox` 仍然存在
- `put + flush` 仍然是大包阶段主成本之一

### 预期量级

合理预期是小幅到中等收益，而不是数量级提升：

- 小 payload 下，固定开销占比较高，收益更明显
- 大 payload 下，收益会被 `put + flush` 稀释

如果只是把 `commit` 改成 `AM + ack`，更合理的预期是改善尾部固定开销，而不是彻底改变大包吞吐上限。

## 后续更大一步的方向

若将来希望继续减少控制面参与，下一阶段应考虑：

1. 把单 inbox 改成多 slot / ring buffer
2. 通过远端原子或 slot 分配协议消除单 inbox `reserve`
3. 最终把 `reserve` 也迁到数据面

但那已经不属于“只改 commit 通知方式”的低风险 follow-up。

## TODO 摘要

- 先做 `commit -> UCX AM`，保留 `reserve`
- reserve 响应增加 `reservation_id`
- 新增 `PushCommitAm` / `PushCommitAckAm`
- AM callback 只入队，不做 finalize 重活
- 增加针对 `commit_rpc_us` 的拆分指标，区分：
  - reserve 控制面耗时
  - commit 通知/确认耗时
  - 总控制路径耗时
