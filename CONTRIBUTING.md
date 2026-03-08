# ZeroKV 贡献指南

感谢您对 ZeroKV 项目的关注！

## 行为准则

请阅读并遵守我们的 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。

## 如何贡献

### 报告 Bug

1. 检查是否已存在相关 Issue
2. 使用 Issue 模板创建新 Issue
3. 包含复现步骤和环境信息

### 提出新功能

1. 使用 Feature Issue 模板
2. 详细描述功能需求
3. 说明对用户价值的场景

### 贡献代码

1. Fork 仓库
2. 创建功能分支: `git checkout -b feature/your-feature`
3. 编写代码并测试
4. 提交更改: `git commit -m 'Add new feature'`
5. 推送分支: `git push origin feature/your-feature`
6. 创建 Pull Request

## 任务领取

### 优先级

- **P1**: 必须完成的紧急任务
- **P2**: 重要但可稍后完成
- **P3**: 改进性需求

### 领取流程

1. 查看 GitHub Issues 列表
2. 选择 P1 优先级任务
3. 在 Issue 下评论 "I'll work on this"
4. 将 Issue 分配给自己
5. 开始开发

### 开发完成

1. 确保所有测试通过
2. 更新相关文档
3. 在 PR 描述中关联 Issue
4. 请求代码 Review

## 代码规范

- 遵循 C++17 标准
- 使用 4 空格缩进
- 命名清晰有意义
- 添加必要的注释

## 测试要求

- 新功能必须包含测试
- 确保现有测试不破坏
- 运行本地测试后再提交

## 文档

- 更新 API 文档
- 添加使用示例
- 说明任何 Breaking Changes

## 许可

贡献即表示您同意将代码按 MIT 许可证发布。
