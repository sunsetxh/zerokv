# Ascend NPU 支持 — 商业影响分析

> 角色: 产品经理 (PM)
> 日期: 2026-03-04
> 关联: risk-assessment-ascend.md

---

## 一、Ascend 生态现状

### 市场份额

- 华为 2025 年出货约 80 万颗 Ascend 910C，2026 年计划翻倍至 160 万颗
- Ascend 在中国国产 AI 芯片市场份额排名第一
- 中国高端 AI 芯片市场 2026 年预计增长超 60%，国产芯片份额有望达约 50%

### 核心客户

| 类别 | 代表 | 规模 |
|------|------|------|
| 互联网巨头 | 字节跳动、百度、阿里巴巴 | 数十万颗级别 |
| AI 公司 | DeepSeek、科大讯飞、商汤 | 训练+推理 |
| 电信运营商 | 中国移动 | 国家级数据中心 |
| 政府/国企 | 各省市智算中心 | 政策驱动 |

### 政策驱动力

- 工信部已将华为和寒武纪 AI 芯片纳入政府采购清单
- 公有云和国家资助数据中心项目已禁止使用外国 AI 芯片
- **Ascend 支持从"差异化"变为"市场准入条件"**

---

## 二、HCCL 生态

| 属性 | 详情 |
|------|------|
| 许可证 | CANN Open Software License v1.0（非标准，仅限 Ascend 处理器） |
| 代码托管 | Gitee（非 GitHub） |
| 版本兼容性 | CANN 8.5.0 有 breaking change |

### 已集成 HCCL 的项目

| 项目 | 状态 |
|------|------|
| PyTorch (torch_npu) | 已集成 |
| vLLM (vllm-ascend) | 活跃开发，v0.13.0 |
| DeepSpeed | 基础支持 |
| **FlagCX** | **已实现 NCCL+HCCL 统一抽象，定位高度重叠** |
| MindSpore | 原生支持 |

---

## 三、商业影响

### 放弃 Ascend 的损失

- 直接丧失 **30-40% 中国市场潜在用户**
- 无法进入政府/国企项目
- 在双栈公司（字节、百度等）中吸引力大打折扣

### FlagCX 竞争威胁

FlagCX（智源研究院）已实现多芯通信统一抽象（NCCL+HCCL+RCCL+MUSACCL），但聚焦 Collective 而非 AXON。我们的差异化需重新定义为：**"零SM消耗 + 双栈 + AXON专注 + KV Cache优化"的组合**。

---

## 四、建议

1. **维持 Ascend 优先级** — 市场准入条件，不可放弃
2. **PoC 前置到第一期** — 技术不确定性尽早消除
3. **寻求华为合作** — 加入昇腾开发者生态计划
4. **法务审查许可证** — CANN License 与项目 License 的兼容性
5. **与 FlagCX 探讨互补** — 他们做 Collective，我们做 AXON

---

**参考资料：**
- [Bloomberg - Huawei Ascend AI Chips](https://www.bloomberg.com/news/articles/2025-09-29/huawei-to-double-output-of-top-ai-chip-as-nvidia-wavers-in-china)
- [TrendForce - China Procurement List](https://www.trendforce.com/news/2025/12/10/news-china-reportedly-adds-huawei-cambricon-ai-chips-to-procurement-list-ahead-of-h200-export-approval/)
- [GitHub - FlagCX](https://github.com/flagos-ai/FlagCX)
- [vLLM Blog - Hardware Plugin](https://blog.vllm.ai/2025/05/12/hardware-plugin.html)
- [ChinaTalk - Can Huawei Compete with CUDA](https://www.chinatalk.media/p/can-huawei-compete-with-cuda)
