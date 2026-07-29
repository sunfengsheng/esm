# 文档导航

本目录是 `everything_sm` 的长期项目文档入口。文档内容应与当前 `main` 分支实现保持一致。

## 用户

- [用户手册](USER_GUIDE.md)：安装、启动、GUI 操作、快捷键和故障排查。
- [查询语法](QUERY_SYNTAX.md)：当前支持的查询表达式和兼容边界。
- [独立内容搜索原型](CONTENT_SEARCH.md)：Xapian 内容服务、实验 UI、运行方式和已知限制。
- [当前状态](CURRENT_STATUS.md)：已实现、部分实现、未实现和已知问题。

## 开发者

- [架构文档](ARCHITECTURE.md)：进程、索引、持久化、IPC 和 GUI 数据流。
- [开发指南](DEVELOPMENT.md)：依赖、构建、测试、基准、安装包和修改流程。
- [性能说明](PERFORMANCE.md)：基准方法、当前数据和性能约束。
- [历史基线](BASELINE_2026-07-23.md)：早期性能与容量记录，仅作历史比较。

## 运行、发布和计划

- [运行与维护](OPERATIONS.md)：服务、数据文件、恢复、诊断和卸载。
- [路线图](ROADMAP.md)：后续工作以及与 Everything 的差距。
- [变更记录](../CHANGELOG.md)：版本和 `Unreleased` 变更。
- [贡献指南](../CONTRIBUTING.md)：提交要求和文档同步规则。

## 文档维护约定

| 变更类型 | 必须更新 |
|---|---|
| 用户可见功能、菜单、快捷键 | `CHANGELOG.md` + `USER_GUIDE.md` |
| 查询语法、过滤器 | `CHANGELOG.md` + `QUERY_SYNTAX.md` |
| 架构、数据流、持久化、IPC | `CHANGELOG.md` + `ARCHITECTURE.md` |
| 性能优化或基准变化 | `CHANGELOG.md` + `PERFORMANCE.md` |
| 构建、测试、CI、安装包 | `CHANGELOG.md` + `DEVELOPMENT.md` 或 `OPERATIONS.md` |
| 完成度或已知限制 | `CHANGELOG.md` + `CURRENT_STATUS.md`/`ROADMAP.md` |

提交前运行：

```powershell
.\tools\check-docs-updated.ps1 -Staged
```

- [Everything ???????](EVERYTHING_COMPATIBILITY.md)????????????????????
