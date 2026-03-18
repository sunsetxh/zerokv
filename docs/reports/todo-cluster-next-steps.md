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
   - 文件：`include/p2p/cluster.h`、`src/cluster.cpp`
   - 问题：测试目前依赖“预留端口再释放”的 workaround，存在 TOCTOU 风险。

### 测试补充

5. 增加 `min_cluster_size > 1` 行为测试
   - 文件：`tests/integration/test_cluster_discovery.cpp`
   - 目标：验证 master 在成员数未达标前保持 `kWaitingMembership`，达标后再进入 `kReady`。

6. 增加三节点 membership 广播测试
   - 文件：`tests/integration/test_cluster_discovery.cpp`
   - 目标：`slave1` 下线后，`slave2` 也能收到最新成员表并更新路由视图。

7. 增加 malformed frame 测试
   - 文件：`tests/integration/test_cluster_discovery.cpp`
   - 目标：覆盖错误 magic、错误版本、损坏 header 等拒绝路径。

### 后续阶段

8. 把 route refresh 从“同步直连”收敛为更明确的连接状态机
   - 文件：`src/cluster.cpp`
   - 目标：为连接重试、失败状态、重连与故障摘除留出稳定结构。

9. 实现 stale node / reconnect 替换策略
   - 文件：`src/cluster.cpp`
   - 依赖：真实 `node_id`

10. 进一步收敛 `wait_ready()` 语义
   - 文件：`include/p2p/cluster.h`、`src/cluster.cpp`
   - 目标：把“控制面 ready”和“数据面 route ready”的边界定义得更清楚。
