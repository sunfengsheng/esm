# 贡献指南

## 基本流程

1. 从最新 `main` 开始修改。
2. 修改实现和测试。
3. 同步更新文档。
4. 执行构建、测试和文档检查。
5. 提交清晰、可回滚的变更。

## 强制文档同步

任何以下内容发生变化时，必须在同一提交中更新 `CHANGELOG.md` 和至少一份长期文档：

- `include/**`、`src/**`：接口、实现或用户行为；
- `tests/**`、`benchmarks/**`：覆盖范围、测试方法或基准；
- `packaging/**`、`.github/**`、`CMakeLists.txt`：构建、安装、CI 或发布；
- `tools/**`、`assets/**`：开发工具或产品资源；
- 根目录构建/配置脚本。

长期文档包括 `README.md`、`CONTRIBUTING.md` 和 `docs/**`。只更新 `CHANGELOG.md` 不足以说明长期行为。

安装本地 Hook：

```powershell
.\tools\install-git-hooks.ps1
```

手动检查暂存区：

```powershell
.\tools\check-docs-updated.ps1 -Staged
```

检查提交范围：

```powershell
.\tools\check-docs-updated.ps1 -BaseRef HEAD^ -HeadRef HEAD
```

## 构建和测试

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
.\tools\check-docs-updated.ps1 -BaseRef HEAD -HeadRef WORKTREE
git diff --check
```

GUI 菜单相关修改还应运行：

```powershell
.\tools\gui-menu-smoke.ps1 -BuildDir .\build
.\tools\gui-modal-menu-smoke.ps1 -BuildDir .\build
```

## 变更对应文档

- UI、文件操作、快捷键：`docs/USER_GUIDE.md`
- 查询解析和匹配：`docs/QUERY_SYNTAX.md`
- 数据结构、线程、IPC、持久化：`docs/ARCHITECTURE.md`
- 基准和优化：`docs/PERFORMANCE.md`
- 构建、测试、依赖：`docs/DEVELOPMENT.md`
- 服务、安装、恢复：`docs/OPERATIONS.md`
- 完成度和限制：`docs/CURRENT_STATUS.md`、`docs/ROADMAP.md`

## 兼容性表述

不要在代码、文档或发行说明中声称项目已经“完全复刻 Everything”或“完全兼容 Everything”。当前项目是独立 clean-room 实现，查询语法、NTFS 边界、安全隔离、资源占用和发布能力仍有差距。
