# AXON 高性能传输库 — Master/Slave 服务发现与别名路由设计 V3

> 角色: 技术架构设计
> 日期: 2026-03-16
> 状态: 根据 Copilot 两轮 review 重写
> 目标: 为当前 `axon` 增加基于 `master + multiple slaves` 的服务发现、成员管理、别名寻址与点对点数据面路由能力，并明确线程模型、控制面 framing、故障语义、master 的数据面职责，以及命令队列的异步完成模型。

---

## 一、结论先行

V3 明确采用以下设计：

1. `master` 同时承担控制面和数据面职责，并固定为 `RANK0`
2. 所有节点，包括 `master`，都参与数据面收发
3. 数据连接唯一真相源是 `worker_addr`
4. 控制面单独走 TCP，带固定二进制帧头、版本协商和 payload 上限
5. 所有 UCX / `Worker` / `Endpoint` 操作都只能在 progress thread 执行
6. 业务线程只通过 `Cluster` 的命令队列提交操作
7. 命令队列返回值使用显式 `Promise/Future bridging`
8. 不再向业务层暴露裸 `Endpoint::Ptr`
9. `recv_any()` 也必须走命令队列，不能从业务线程直接碰 `Worker::tag_recv()`
10. `kReady`、`kDegraded`、`kFailed` 的行为都被精确定义

这版目标是进入实现前，把真正会阻塞首个 PR 的设计分歧全部定死。

---

## 二、需求范围

### 2.1 业务目标

需要支持：

1. 一个固定 `master`
2. 多个 `slave`
3. 通过稳定别名例如 `RANK0`、`RANK1`、`RANK2` 寻址
4. 业务层直接 `send("RANK0", buffer, tag)`
5. 支持成员加入、掉线、成员表更新

### 2.2 当前仓库已有能力

当前数据面已有：

- `Worker::listen()`
- `Worker::connect(const std::vector<uint8_t>& remote_address)`
- `Worker::address()`
- `Endpoint::tag_send()`
- `Worker::tag_recv()`

当前缺少：

- 服务发现
- 成员表
- alias/rank 路由
- 生命周期管理
- 心跳和故障摘除
- 上层异步命令编排

---

## 三、V3 相对 V2 的新增修正

### 3.1 解决命令队列外层 Future 的完成语义

V2 最大缺口是：

- `Cluster::send()` 返回一个 outer `Future<void>`
- progress thread 上 `Endpoint::tag_send()` 又会产生一个 inner `Future<void>`
- 两者之间没有桥接设计

V3 明确：

- 外层 API 通过 `Promise<T>` 产生 outer future
- command queue 中传递 `Promise<T>`
- progress thread 负责跟踪 inner future
- inner future 完成时，显式 fulfill outer promise

这保证：

- outer future 表示“真实发送完成”
- 而不是“命令已入队”

### 3.2 删除 `endpoint(alias)` 裸暴露

V2 暴露了 `endpoint(alias)`，调用方拿到 `Endpoint::Ptr` 后可以绕过命令队列直接碰 UCX。

V3 改为：

- 移除 `endpoint(alias)` 公共接口
- 若需要对外暴露路由状态，只返回只读 `RouteHandle`
- `RouteHandle` 不带任何 UCX/Endpoint 方法

### 3.3 `recv_any()` 也遵守线程模型

V2 仍有歧义，V3 明确：

- `recv_any()` 不能从业务线程直接调用 `Worker::tag_recv()`
- 它和 `send()` 一样，也通过命令队列在 progress thread 上 post recv

### 3.4 `rank=0` 为 master 保留

V3 明确：

- `rank=0` 只能由 master 使用
- slave 若尝试注册 `rank=0`，master 必须拒绝
- `RegisterAck` 必须给出明确错误码

### 3.5 `wait_ready()` 语义与 master 行为明确

V3 明确：

- slave 的 `wait_ready()`：注册完成、拿到成员表、满足 ready 条件后完成
- master 的 `wait_ready()`：默认表示“控制面已启动 + 自身 `RANK0` 数据面已就绪”
- 若需要等待最小集群规模，必须在 config 中显式给出 `min_cluster_size`

---

## 四、设计原则

### 4.1 控制面和数据面分离

- 控制面负责注册、心跳、成员表广播、掉线通知
- 数据面负责 tag/stream 消息收发
- `master` 承担控制面职责，但不转发业务数据

### 4.2 rank 是内部主键，alias 是外部命名

- 内部路由主键使用 `rank`
- 对外 API 可接受 `rank` 或 `alias`
- MVP 中 `alias` 直接由 `rank` 派生：`RANK{rank}`

### 4.3 数据连接只认 `worker_addr`

- 数据面连接一律使用 `worker_addr`
- `control_host/control_port` 只用于控制面
- 不允许用 `host:port` 做 UCX 数据面 bootstrap

### 4.4 所有 UCX 调用必须串行进入 progress thread

- 业务线程不能直接碰 `Worker` / `Endpoint`
- 所有操作通过命令队列提交
- progress thread 是唯一执行上下文

---

## 五、总体架构

```text
+------------------------------------------------------------------+
|                         Application API                          |
|    send("RANK0") / send(rank) / route(alias) / wait_ready()      |
+------------------------------------------------------------------+
|                             Cluster                              |
|  state machine | membership snapshot | command queue             |
|  route table   | pending ops         | promise/future bridge     |
+------------------------------------------------------------------+
|                 Control Plane (TCP, framed protocol)             |
|  HELLO | REGISTER | MEMBERSHIP_UPDATE | HEARTBEAT | DISCONNECT   |
+------------------------------------------------------------------+
|                     Existing axon Data Plane                      |
|             Worker | Listener | Endpoint | tag/stream            |
+------------------------------------------------------------------+
|                               UCX                                |
+------------------------------------------------------------------+
```

新增模块建议：

- `include/axon/cluster.h`
- `src/cluster.cpp`
- `include/axon/internal/discovery.h`
- `src/discovery.cpp`

注意：

- `DiscoveryServer` / `DiscoveryClient` 不进入 public API
- 它们是 `Cluster` 的内部组件

---

## 六、角色定义

### 6.1 Master

`master` 同时是：

- 控制面注册中心
- `RANK0` 数据面节点

职责：

1. 监听固定控制面地址
2. 自身作为 `RANK0` 进入成员表
3. 接收 slave 注册
4. 校验 rank / alias 唯一性
5. 生成 `MembershipSnapshot`
6. 广播成员变更
7. 接收心跳
8. 标记节点 offline

### 6.2 Slave

职责：

1. 启动本地数据面 `Worker` 和 listener
2. 获取自身 `worker_addr`
3. 连接 master 控制面
4. 注册自身信息
5. 接收成员表
6. 按 rank 规则建立点对点数据连接
7. 维护心跳

---

## 七、数据模型

### 7.1 线上传输类型

运行时字段和线上字段分离：

```cpp
struct PeerDescriptor {
    uint32_t rank = 0;
    std::string alias;
    std::vector<uint8_t> worker_addr;
    std::string node_id;
    bool is_master = false;
};
```

控制面地址不需要广播给所有 peer。仅 slave 需要知道 master 控制地址，其他 slave 的控制地址不进入成员表快照。

### 7.2 运行时节点状态

```cpp
struct PeerRuntimeState {
    PeerDescriptor desc;
    uint64_t last_heartbeat_ms = 0;
    bool control_alive = true;
    bool data_alive = true;
    bool online = true;
};
```

### 7.3 成员表快照

```cpp
struct MembershipSnapshot {
    uint64_t epoch = 0;
    std::vector<PeerDescriptor> peers;
};

using MembershipSnapshotPtr = std::shared_ptr<const MembershipSnapshot>;
```

### 7.4 路由项

```cpp
struct RouteEntry {
    PeerDescriptor peer;
    Endpoint::Ptr endpoint;

    enum class State {
        kDisconnected,
        kConnecting,
        kReady,
        kFailed,
        kClosed
    } state = State::kDisconnected;

    Status last_error = Status::OK();
};
```

### 7.5 RouteHandle

对外如果需要查询路由状态，只返回只读句柄：

```cpp
struct RouteHandle {
    uint32_t rank = 0;
    std::string alias;
    bool connected = false;
    Status last_error = Status::OK();
};
```

该类型不包含任何 UCX/Endpoint 方法。

---

## 八、线程模型与异步模型

### 8.1 唯一执行上下文

所有 UCX / `Worker` / `Endpoint` 生命周期操作都在 progress thread 执行：

1. `Worker::connect(worker_addr)`
2. `Endpoint::tag_send()`
3. `Worker::tag_recv()`
4. `Endpoint::close()`
5. route rebuild
6. route failover

### 8.2 命令队列

业务线程调用：

```cpp
cluster->send("RANK3", buf, len, tag);
```

业务线程只做：

1. 参数检查
2. alias 解析
3. 创建 `Promise<T>`
4. 把 command 投递到队列
5. 返回 outer future

progress thread 负责：

1. 消费命令
2. 调用 inner UCX 操作
3. 跟踪 inner future
4. 回写 outer promise

### 8.3 Promise / Future bridging

这是 V3 必须定死的点。

示意：

```cpp
template <typename T>
class Promise;

struct SendCommand {
    uint32_t target_rank = 0;
    std::shared_ptr<std::vector<uint8_t>> payload;
    Tag tag = 0;
    std::shared_ptr<Promise<void>> promise;
};
```

执行流程：

```text
caller thread:
  outer = cluster->send(...)
  -> create Promise<void>
  -> enqueue SendCommand{..., promise}
  -> return promise->get_future()

progress thread:
  -> dequeue command
  -> inner = endpoint->tag_send(...)
  -> put {inner, promise} into pending_ops

progress loop:
  -> poll pending_ops
  -> if inner ready and status ok:
         promise->set_value()
  -> if inner ready and status failed:
         promise->set_error(status)
  -> if cluster stopping:
         promise->set_error(kCanceled)
```

要求：

1. outer future 不得在“命令入队”时完成
2. outer future 必须继承 inner future 的最终状态
3. outer future 必须在 stop/cancel 时可完成

`Promise<T>` 契约：

1. `Promise<T>` 是单次使用对象，只能 fulfill 一次
2. `set_value()` / `set_error()` 可由 progress thread 安全调用
3. 对应 `Future<T>` 可由业务线程安全轮询或等待
4. 若 `Promise<T>` 在 fulfill 前随 cluster stop 一起销毁，必须先向 future 写入 cancel/error，再释放
5. 不允许多个线程并发 fulfill 同一个 promise

### 8.4 `recv_any()` 也走命令队列

V3 明确：

- `recv_any()` 不是直接从业务线程碰 `Worker::tag_recv()`
- 它通过 `PostRecvCommand` 进入队列
- 返回值同样通过 promise 桥接

这样发送与接收都遵守相同的线程约束。

### 8.5 锁约束

建议：

- `membership_mutex` 保护 snapshot 指针替换
- `routes_mutex` 保护 `rank -> RouteEntry`
- 锁内禁止调用任何 UCX API

规则：

- 锁内只做纯内存修改
- 一旦要碰 `Worker` / `Endpoint`，必须切到命令队列

---

## 九、Cluster 状态机

### 9.1 ClusterState

```cpp
enum class ClusterState {
    kInit,
    kStarting,
    kRegistering,
    kWaitingMembership,
    kConnectingPeers,
    kReady,
    kDegraded,
    kStopping,
    kStopped,
    kFailed
};
```

### 9.2 状态语义

- `kInit`
  - 已创建，未启动
- `kStarting`
  - listener、progress thread、控制面线程启动中
- `kRegistering`
  - 正在向 master 注册
- `kWaitingMembership`
  - 已注册，等待成员表
- `kConnectingPeers`
  - 正根据成员表预连接
- `kReady`
  - 满足 ready 条件，可正常按 alias 收发
- `kDegraded`
  - 控制面或部分 route 异常，但仍保留已有活跃 route
- `kStopping`
  - 正在关闭
- `kStopped`
  - 已完全停止
- `kFailed`
  - 无法继续工作，需外部重启

### 9.3 `kReady` 的定义

slave 的 `kReady` 条件：

1. 已注册成功
2. 已收到最新成员表
3. 对所有 `peer.rank > self.rank` 的预期主动连接都已达到 `kReady`
4. 对所有 `peer.rank < self.rank` 的入向连接，不要求在进入 `kReady` 前全部到齐

原因：

- 入向连接受远端启动时序影响，若要求全部到齐会放大启动耦合

master 的 `kReady` 条件：

1. 控制面服务已启动
2. 自身 `RANK0` 数据面已就绪
3. 若配置了 `min_cluster_size > 1`，则要求当前 membership 节点数达到阈值
4. `kReady` 不自动等价于“所有 peer route 都已 ready”

说明：

- master 的 `kReady` 默认表示“可对外提供控制面服务，并具备参与数据面的本地能力”
- 它不保证对所有已注册 peer 的数据路由都已经建好
- 如果调用方在 `wait_ready()` 后立刻 `send("RANKN")`，仍可能因为 route 尚未 ready 而收到 `kInProgress` / `kEndpointClosed` 类错误
- 如果未来希望 master 的 ready 严格等价于“所有已知 peer 的主动连接都 ready”，需要单独增加更严格的 ready policy，而不是修改当前默认语义

### 9.4 `kDegraded` 的定义

进入条件：

- 与 master 控制面断开
- 或关键 route 失败但尚未完全退出

行为：

1. 已处于 `RouteEntry::kReady` 的 route 仍可发送
2. 不再对新 membership 变更做 ready 保证
3. `route(alias)` 仍可返回当前状态
4. `send(alias)` 对 failed / missing route 返回错误
5. `recv_any()` 仍可继续工作
6. 若与 master 恢复连接，必须重新注册、等待新成员表，然后再回到 `kReady`

---

## 十、控制面协议

控制面使用 TCP 长连接，带固定帧头。

### 10.1 固定帧头

帧头格式固定，不参与版本协商。
若未来需要破坏性修改帧头，必须更换 `magic`。

```cpp
struct ControlFrameHeader {
    uint32_t magic = 0x50325031;  // "AXON1"
    uint16_t header_version = 1;  // fixed framing version
    uint16_t type = 0;
    uint32_t payload_size = 0;
    uint32_t reserved = 0;
};
```

说明：

- `header_version` 只描述 framing 头格式
- 业务协议版本通过 `HELLO/HELLO_ACK` 协商

### 10.2 HELLO / HELLO_ACK

```cpp
struct Hello {
    uint16_t min_proto_version;
    uint16_t max_proto_version;
    std::string node_id;
    bool is_master;
};

struct HelloAck {
    bool accepted;
    uint16_t selected_proto_version;
    std::string error;
};
```

### 10.3 接收状态机

```text
kReadHeader
  -> validate magic/header_version/payload_size
  -> kReadPayload
  -> dispatch(type)
  -> kReadHeader
```

约束：

- `payload_size` 最大 1 MiB
- 超限直接断连
- 必须支持半包累积和粘包拆帧

### 10.4 消息类型

```cpp
enum class ControlMsgType : uint16_t {
    kHello = 1,
    kHelloAck = 2,
    kRegisterReq = 3,
    kRegisterAck = 4,
    kMembershipUpdate = 5,
    kHeartbeat = 6,
    kDisconnect = 7,
    kError = 8
};
```

### 10.5 注册错误码

```cpp
enum class RegisterErrorCode : uint16_t {
    kNone = 0,
    kRankReservedForMaster = 1,
    kRankAlreadyTaken = 2,
    kAliasAlreadyTaken = 3,
    kProtocolVersionMismatch = 4,
    kInvalidRequest = 5
};
```

### 10.6 注册规则

- `rank=0` 只允许 master 使用
- slave 使用 `rank=0` 必须返回 `kRankReservedForMaster`
- slave 若使用已被占用的其他 rank，返回 `kRankAlreadyTaken`

slave 收到上述错误后：

- 直接进入 `kFailed`
- 由上层决定是否更换 rank 重试

---

## 十一、连接时序

### 11.1 Master 启动

```text
1. 创建 Context / Worker
2. 启动 Worker listener
3. 获取自身 worker_addr
4. 启动控制面服务
5. 将自身写入 MembershipSnapshot，rank=0 alias=RANK0
6. 每次有新的 MembershipUpdate 生效时，master 也必须执行 §11.3 的 diff-routes 逻辑
7. 对所有满足 self.rank < peer.rank 的 peer（即所有 rank > 0 的节点），主动 enqueue connect commands
8. 若 min_cluster_size == 1，则可进入 kReady
9. 否则等待更多 slave 注册
```

说明：

- master 不只是维护成员表，它也必须像普通数据面节点一样参与 route 建立
- 因为 master 固定为 `RANK0`，所以对所有其他 peer 都是主动拨号方

### 11.2 Slave 启动

```text
1. 创建 Context / Worker
2. 启动 Worker listener
3. 获取自身 worker_addr
4. 建立到 master 的 TCP 控制连接
5. HELLO / HELLO_ACK
6. 发送 RegisterRequest
7. 接收 RegisterAck
8. 接收 MembershipUpdate
9. 应用成员表
10. 若 self.rank < peer.rank，则向 peer.worker_addr 发起数据连接
11. 满足 ready 条件后进入 kReady
```

### 11.3 成员表更新

```text
on MembershipUpdate(epoch=N):
  if N <= local_epoch:
      ignore
  else:
      replace snapshot
      diff routes
      enqueue connect/close/failover commands
      when ready condition satisfied:
          state = kReady or kDegraded
```

---

## 十二、公开 API 草案

### 12.1 配置对象

```cpp
struct MasterClusterConfig {
    std::string control_bind_addr;
    std::string data_bind_addr = "0.0.0.0:0";
    size_t min_cluster_size = 1;  // includes master
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
};

struct SlaveClusterConfig {
    uint32_t rank;
    std::string master_control_addr;
    std::string data_bind_addr = "0.0.0.0:0";
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
};
```

### 12.2 alias 工具

```cpp
inline std::string rank_alias(uint32_t rank) {
    return "RANK" + std::to_string(rank);
}
```

### 12.3 Cluster

```cpp
class Cluster : public std::enable_shared_from_this<Cluster> {
public:
    using Ptr = std::shared_ptr<Cluster>;

    static Ptr create_master(Context::Ptr ctx, Worker::Ptr worker,
                             MasterClusterConfig cfg);
    static Ptr create_slave(Context::Ptr ctx, Worker::Ptr worker,
                            SlaveClusterConfig cfg);

    Status start();
    Status stop();

    [[nodiscard]] ClusterState state() const noexcept;
    [[nodiscard]] uint32_t self_rank() const noexcept;
    [[nodiscard]] std::string self_alias() const;
    [[nodiscard]] bool is_master() const noexcept;

    [[nodiscard]] std::optional<uint32_t> resolve_rank(std::string_view alias) const;
    [[nodiscard]] MembershipSnapshotPtr membership() const;
    [[nodiscard]] std::optional<RouteHandle> route(std::string_view alias) const;

    Future<void> wait_ready();
    Future<void> wait_ready(std::chrono::milliseconds timeout);

    Future<void> send(std::string_view alias, const void* buffer, size_t length, Tag tag);
    Future<void> send(uint32_t rank, const void* buffer, size_t length, Tag tag);

    Future<std::pair<size_t, Tag>>
    recv_any(void* buffer, size_t length, Tag tag, Tag tag_mask = kTagMaskAll);
};
```

### 12.4 API 语义

- `route(alias)`
  - 只返回只读 `RouteHandle`
  - 不提供任何 `Endpoint` 访问能力

- `send(alias, ...)`
  - 通过命令队列执行
  - outer future 表示真实发送完成，而非“已入队”
  - 若 cluster 处于 `kInit` / `kStarting` / `kRegistering` / `kWaitingMembership` / `kConnectingPeers`，返回 `kInProgress`
  - 若 cluster 处于 `kReady`，但目标 route 尚未 ready，返回 `kInProgress` 或 `kEndpointClosed`
  - 若 cluster 处于 `kDegraded`，对 missing / failed route 返回错误；对已 ready route 允许继续发送
  - 若 cluster 处于 `kStopping` / `kStopped`，返回 `kCanceled`
  - 若 cluster 处于 `kFailed`，返回 `kInternalError` 或专用 route-not-ready 错误

- `recv_any()`
  - 使用创建 cluster 时传入的单个 worker
  - 通过命令队列在 progress thread 上 post recv
  - 多个并发 `recv_any()` 调用是允许的；每次调用独立 post 一个接收，哪个请求匹配到消息由底层 tag matching 决定

- `wait_ready(timeout)`
  - timeout 到期返回超时错误
  - 对 master 而言，默认只表示“控制面 up + 自身本地数据面 ready + min_cluster_size 条件满足”
  - 不承诺所有远端 route 已 ready

### 12.5 `send()` 错误语义建议

建议新增一组 cluster 级错误语义，避免把 route 未就绪和 transport 级错误混淆：

```cpp
enum class ClusterErrorCode : uint16_t {
    kRouteNotReady = 10000,
    kRouteMissing = 10001,
    kClusterNotReady = 10002
};
```

如果本期不扩展错误码枚举，也至少在实现里把这三种情况映射为稳定可区分的 `Status.message()`。

---

## 十三、故障检测与恢复

### 13.1 心跳分层

不要把“控制面断了”和“数据面断了”混在一起。

运行时单独维护：

- `control_alive`
- `data_alive`

### 13.2 控制面失联

若 slave 与 master 的 TCP 控制面连接断开：

1. 节点进入 `kDegraded`
2. 已有 `kReady` route 暂不立即 teardown
3. 启动重连和重新注册
4. 若超过 `control_grace_period = 3 * heartbeat_timeout` 仍未恢复，则关闭所有非本地 route 并进入 `kFailed`

说明：

- teardown 范围是“所有非本地 route”
- 这是偏保守策略，优先一致性

### 13.3 数据面错误

若 route 的 endpoint error callback 触发：

1. callback 内不直接加锁
2. 只投递 `kMarkRouteFailed` 命令
3. progress thread 更新 `RouteEntry`
4. 若 peer 仍在 membership 中，则按策略重连

### 13.4 相同 rank 不同 node_id 重连

若相同 `rank` 以不同 `node_id` 重连：

1. master 将旧实例标记 stale
2. `epoch++`
3. 广播新成员表
4. peers 关闭旧 route，建立新 route

---

## 十四、测试计划

### 14.1 单元测试

1. `rank_alias(0) == "RANK0"`
2. `resolve_rank("RANK3") == 3`
3. 非法 alias 拒绝
4. `payload_size` 上限校验
5. framing 半包解析
6. `rank=0` 注册冲突错误码
7. promise/outer future 桥接

### 14.2 集成测试

1. `master(RANK0) + 2 slaves` 注册成功
2. 所有节点看到一致成员表
3. `RANK1 -> RANK0` 发送成功
4. `RANK0 -> RANK2` 发送成功
5. 高 rank 先启动、低 rank 后启动，最终 ready
6. `wait_ready()` 可正常解除阻塞
7. outer future 与真实完成时刻一致

### 14.3 失败路径测试

1. 并发重复 rank 注册
2. malformed frame
3. oversized payload
4. `send()` 发生在 `wait_ready()` 完成前
5. master 控制面断开但数据面仍存活
6. slave 间数据面断开但 master 仍可达
7. 旧 node_id 被新实例替换
8. route error callback 不死锁
9. slave 使用 `rank=0` 注册被拒绝
10. `recv_any()` 通过命令队列正确 post recv

---

## 十五、实现拆分建议

### Phase 1: 纯模型与接口骨架

交付：

1. `ClusterState`
2. `PeerDescriptor`
3. `MembershipSnapshot`
4. `MasterClusterConfig`
5. `SlaveClusterConfig`
6. `RouteHandle`
7. alias/rank 工具函数

### Phase 2: Promise / Future bridging

交付：

1. `Promise<T>` 草案
2. `pending_ops` 跟踪机制
3. outer/inner future 桥接

### Phase 3: 控制面 framing 与注册

交付：

1. 固定帧头
2. `HELLO/HELLO_ACK`
3. 注册与成员表

### Phase 4: 数据面路由

交付：

1. route 表
2. rank-based single dial
3. `send(alias, ...)`
4. `recv_any()`

### Phase 5: 故障恢复

交付：

1. heartbeat
2. `kDegraded`
3. re-register
4. route failover

---

## 十六、关键决策总结

1. `master` 是 `RANK0`，同时承担控制面和数据面职责
2. 数据连接唯一真相源是 `worker_addr`
3. 业务线程不直接碰 UCX，统一通过命令队列
4. 命令队列返回值必须用 promise/future bridging 连接真实 UCX 完成状态
5. 不再暴露裸 `Endpoint::Ptr`
6. `recv_any()` 也必须走命令队列
7. 控制面 framing 固定，业务协议版本通过 `HELLO` 协商

---

## 十七、建议的下一步

进入代码实现前，建议先做一个小 PR，只包含：

1. `include/axon/cluster.h`
2. `ClusterState`
3. `PeerDescriptor`
4. `MembershipSnapshot`
5. `RouteHandle`
6. `MasterClusterConfig`
7. `SlaveClusterConfig`
8. `Promise<T>` 设计占位
9. alias/rank 单元测试

这样可以先把最敏感的接口和异步完成语义固定下来，再进入协议和路由实现。
