# macOS RDMA 仿真环境搭建指南 (QEMU/UTM)

本指南介绍如何在 macOS (Intel/Apple Silicon) 上通过 QEMU 或 UTM 搭建一个完整的 Linux RDMA 仿真环境。

## 1. 背景与限制

在 macOS 上开发 RDMA 应用（如 `zerokv`）时，由于 macOS 原生不支持 RDMA，必须使用 Linux 仿真（Soft-RoCE/RXE）。

- **Docker Desktop / OrbStack**: 使用的是轻量化、共享且通常为**只读**的 Linux 内核镜像。这导致无法使用 `modprobe` 加载自定义内核模块（如 `rdma_rxe`），或无法修改 `/lib/modules`。
- **QEMU / UTM**: 提供全虚拟化（Full Virtualization），虚拟机拥有独立的内核、模块目录和完全可写的根文件系统。这是在 macOS 上运行 Soft-RoCE 的最稳妥方案。

---

## 2. 方案选择

### 方案 A：双 VM 自动化方案 (推荐，完整双节点 RDMA 测试)

使用 `scripts/qemu_rdma/` 下的一组脚本，在本机启动两台 QEMU VM，通过 Soft-RoCE 实现完整的双节点 RDMA 通信验证。

> **为什么选 Soft-RoCE**: vhost-user-rdma 需要 DPDK + 自定义内核 + 修改版 QEMU，macOS 上不可用；pvrdma 已从 QEMU 9.1 移除。Soft-RoCE 是当前唯一成熟可行的方案。

#### 架构

```
macOS 宿主机 (Apple Silicon, hvf 加速)
│
├── QEMU VM1 (Ubuntu 22.04 ARM64, SSH :2222)
│   └── rxe0 (Soft-RoCE) ─── IP 10.0.0.1 ── socket listen ─────┐
│                                                                  │
├── QEMU VM2 (Ubuntu 22.04 ARM64, SSH :2223)                      │
│   └── rxe0 (Soft-RoCE) ─── IP 10.0.0.2 ── socket connect ──────┘
│
└── 宿主机通过 SSH 控制 VM1/VM2
```

- **net0 (socket listen/connect)**: VM1 监听 `127.0.0.1:12345`，VM2 连接到该端口，形成点对点虚拟局域网用于 RDMA 数据通信
- **net1 (user-mode NAT)**: 各自独立的 SSH 端口转发（VM1: 2222, VM2: 2223）

#### 快速开始

```bash
# 前置条件：安装 QEMU 和 sshpass
brew install qemu
brew install esolitos/ipa/sshpass

# 1. 下载镜像、创建磁盘、生成 cloud-init seed
cd scripts/qemu_rdma
./setup.sh

# 2. 启动两台 VM（等待 SSH 就绪）
./start_vms.sh

# 3. 安装依赖、加载 rxe、编译 zerokv（约 10-15 分钟）
./provision.sh

# 4. 运行双节点 RDMA 测试
./run_test.sh

# 5. 测试完成后停止 VM
./stop_vms.sh
```

#### 脚本说明

| 脚本 | 用途 |
|------|------|
| `setup.sh` | 下载 Ubuntu 22.04 Server cloudimg ARM64，为两台 VM 创建 qcow2 overlay，生成 cloud-init seed ISO（用户 axon/axon，DHCP 网络配置） |
| `start_vms.sh` | 使用 hvf 加速启动两台 QEMU VM，配置 socket listen/connect + user-mode 双网卡，等待 SSH 就绪 |
| `stop_vms.sh` | 优雅停止两台 VM |
| `provision.sh` | 通过 SSH 在两台 VM 内：切换 tuna 镜像源、安装依赖（含 `linux-modules-extra`）、加载 `rdma_rxe`、创建 `rxe0`、配置静态 IP (10.0.0.1/2)、传输并编译 zerokv |
| `run_test.sh` | 自动化测试：`ibv_rc_pingpong`、`ibv_ud_pingpong`、zerokv 单机 loopback |

#### 已验证通过的测试

| 测试项 | 状态 | 说明 |
|--------|------|------|
| 两台 VM `ibv_devices` 显示 rxe0 | PASS | Soft-RoCE 设备创建成功 |
| VM 间 ping (10.0.0.1 ↔ 10.0.0.2) | PASS | VM 间网络互通正常 |
| `ibv_rc_pingpong` 双机互通 | PASS | RC 传输，~578 Mbit/sec |
| `ibv_ud_pingpong` 双机互通 | PASS | UD 传输，~170 Mbit/sec |
| zerokv `ping_pong` 双机 (RDMA) | BLOCKED | UCX 1.12 不支持 Soft-RoCE RDMA 传输；zerokv 示例的 TCP bootstrap 尚未实现 |

#### 已知限制

1. **UCX 版本过旧**: Ubuntu 22.04 自带 UCX 1.12，不支持 Soft-RoCE 的 IB 传输。需要 UCX >= 1.14 才能通过 `UCX_TLS=rc` 使用 RDMA。可通过源码编译更新 UCX 解决。
2. **zerokv 跨节点 bootstrap**: zerokv 的 `ping_pong` 等示例目前仅实现了 `shmem`（单机 loopback）模式，跨节点 TCP/RDMA 连接需要实现 bootstrap 机制。
3. **QEMU 网络性能**: user-mode NAT 网络速度较慢，首次 apt 安装需要较长时间。脚本已切换到 tuna 镜像源加速。

### 方案 B：UTM (GUI 操作，适合单机调试)

[UTM](https://getutm.app/) 是基于 QEMU 的 macOS 原生图形界面工具，支持 Apple Silicon 的原生虚拟化。

1. **下载与安装**: 从官网或 Mac App Store 下载 UTM。
2. **新建虚拟机**:
   - 选择 `Virtualize` -> `Linux`。
   - 建议使用 [Ubuntu 22.04 Server](https://ubuntu.com/download/server/arm) (Apple Silicon 选 ARM64，Intel 选 AMD64)。
3. **网络设置**: 默认使用 `Shared Network` 即可。
4. **共享文件夹**: 在 UTM 设置中挂载 `zerokv` 项目目录。

### 方案 C：QEMU CLI 手动操作

```bash
brew install qemu
# 启动示例（需要提前准备好镜像和启动脚本）
qemu-system-aarch64 -m 4G -smp 4 -cpu host -accel hvf ...
```

---

## 3. 虚拟机内环境配置 (手动操作参考)

> 以下内容适用于手动搭建 UTM 或单 VM 环境的场景。使用自动化脚本（方案 A）时无需手动执行。

进入虚拟机终端后，执行以下步骤配置 RDMA 仿真：

### 3.1 安装必要工具
```bash
sudo apt update
sudo apt install -y rdma-core ibverbs-utils iproute2 build-essential cmake \
    libucx-dev pkg-config linux-modules-extra-$(uname -r)
```

### 3.2 加载 Soft-RoCE 内核模块
在全虚拟化的虚拟机中，这一步将不再报错：
```bash
# 加载模块
sudo modprobe rdma_rxe

# 验证加载成功
lsmod | grep rxe
```

### 3.3 创建虚拟 RDMA 设备
1. **确认网卡名称**:
   ```bash
   ip addr  # 假设网卡名为 enp0s1 或 eth0
   ```
2. **创建链接**:
   ```bash
   # 将 rxe0 设备关联到物理/虚拟网卡
   sudo rdma link add rxe0 type rxe netdev enp0s1
   ```
3. **验证设备**:
   ```bash
   ibv_devices
   # 预期输出应包含: rxe0 ...
   ```

---

## 4. 在虚拟机中开发与验证 ZeroKV

### 4.1 挂载代码
利用 UTM 的 VirtFS 挂载 macOS 上的项目代码（路径通常在 `/mnt/` 下）。

### 4.2 编译与运行
```bash
cd /path/to/zerokv
mkdir -p build && cd build
cmake .. -DZEROKV_BUILD_TESTS=OFF -DZEROKV_BUILD_PYTHON=OFF
make -j$(nproc)

# 单机 loopback 测试（shmem 传输）
ZEROKV_TRANSPORTS=shmem ./ping_pong --listen :13337 &
ZEROKV_TRANSPORTS=shmem ./ping_pong --connect localhost:13337
```

---

## 5. 常见问题 (FAQ)

- **Q: 为什么 `lsmod` 还是空的？**
  A: 确保你是在 UTM/QEMU 启动的**真实虚拟机**中运行，而不是在 macOS 宿主机或普通的 Docker 容器中。
- **Q: `modprobe rdma_rxe` 提示 "Module not found"？**
  A: 需要安装 `linux-modules-extra-$(uname -r)` 包，cloud image 默认不包含 RDMA 模块。
- **Q: 性能如何？**
  A: Soft-RoCE 在 CPU 上仿真 RDMA 逻辑，性能弱于真实硬件，但对于功能验证和逻辑开发（Zero-copy 流程、异步 Future 等）完全足够。
- **Q: 重启后设备消失？**
  A: `rdma link` 命令不持久化。建议将其加入 `/etc/rc.local` 或编写一个简单的 setup 脚本。
- **Q: 双 VM 方案的 inter-VM 网卡名是什么？**
  A: 通常为 `enp0s1`（第一块 virtio-net 设备，用于 socket listen/connect），`provision.sh` 会自动检测。
- **Q: VM 间无法 ping 通？**
  A: 确保 VM1 先于 VM2 启动（VM1 需要 listen 在 `127.0.0.1:12345`，VM2 connect 到该端口）。
- **Q: UCX 提示 "no usable transports/devices"？**
  A: Ubuntu 22.04 的 UCX 1.12 不支持 Soft-RoCE。需要从源码编译 UCX >= 1.14，或使用 `ibv_*` 工具直接测试 RDMA 功能。
- **Q: `sshpass` 命令找不到？**
  A: 安装：`brew install esolitos/ipa/sshpass`
- **Q: apt 安装包极慢？**
  A: 切换到国内镜像：`sudo sed -i 's|http://ports.ubuntu.com/ubuntu-ports|https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports|g' /etc/apt/sources.list`
