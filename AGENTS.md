# Repository instructions / 仓库规则

这些规则适用于本仓库的每一次修改。

- 每次源代码、测试、基准、构建、安装、CI、配置、资源或用户可见行为修改，都必须在同一变更中同步文档。
- 每次受控变更都要更新 `CHANGELOG.md` 的 `Unreleased`。
- 同时至少更新一份长期文档：`README.md`、`CONTRIBUTING.md` 或 `docs/**`。
- 实现状态或已知限制变化时更新 `docs/CURRENT_STATUS.md`。
- 架构、数据流、索引、存储、线程或 IPC 变化时更新 `docs/ARCHITECTURE.md`。
- 性能优化或基准变化时更新 `docs/PERFORMANCE.md`，必须写明测试方法，不得把合成测试夸大为真实端到端结果。
- 用户可见 UI、流程或查询变化时更新 `docs/USER_GUIDE.md` 或 `docs/QUERY_SYNTAX.md`。
- 构建、CI、安装包、服务、配置或恢复变化时更新 `docs/DEVELOPMENT.md` 或 `docs/OPERATIONS.md`。
- 除非有完整验证并同步状态/路线图，否则不得声称已经完整兼容 Everything。
- 提交前运行 `tools/check-docs-updated.ps1 -Staged`、相关测试和 `git diff --check`。
