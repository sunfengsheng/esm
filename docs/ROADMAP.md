# 路线图

路线图按“已具备基础能力”和“达到完整产品标准仍需完成”区分，避免把部分实现误写成完整兼容。

## P0：性能与资源占用

### 查询

- [x] 基础名称 trigram 候选缩小。
- [x] raw/accent-folded gram。
- [x] posting 按自然名称顺序构建。
- [x] GUI 两阶段 limit 和过期请求丢弃。
- [ ] `path:` 专用路径组件/gram 索引。
- [ ] 正则/复杂布尔查询的安全候选提取扩展。
- [ ] posting 的增量更新和持久化。
- [ ] 真实键盘到绘制完成的端到端延迟基准。

### 内存

- [x] 紧凑 `NtfsCatalog` 基础层 + overlay。
- [x] v2 memory-mapped snapshot。
- [x] snapshot 流式保存，消除完整临时 `vector<FileRecord>`。
- [ ] Catalog 与 MetadataIndex 共享名称和路径组件。
- [ ] 路径组件化/压缩，减少完整路径重复。
- [ ] 索引直接 mmap 或增量构建。
- [ ] 将约 326 万记录服务内存从当前数 GB 显著降低。

## P1：统一持久化

- [x] atomic checksummed snapshot。
- [x] 单卷 live append-only WAL。
- [x] 完整事务重放和 torn-tail 截断。
- [x] checkpoint consolidation 基础能力。
- [ ] 多卷默认服务使用统一 base snapshot + WAL。
- [ ] 通用 delta replay 和事务恢复。
- [ ] checkpoint 期间持续服务且无大内存峰值。
- [ ] schema 版本迁移与回滚策略。
- [ ] 定期恢复演练和损坏注入测试。

## P2：查询兼容

### 已有基础

- [x] 引号、作用域和排除词。
- [x] AND/OR/NOT、括号和优先级。
- [x] wildcard 和基础 regex。
- [x] size/date/attribute predicate。
- [x] case/whole-word/path/diacritic flags。
- [x] bookmarks、filters、history。
- [x] duplicate name/size/name-size。
- [x] 基础自然排序。

### 仍需完整化

- [ ] Everything 全部函数和别名。
- [ ] 宏与参数化宏。
- [ ] 完整相对日期、范围和时间字段。
- [ ] 完整属性/文件系统函数。
- [ ] 完整 wildcard/regex/escape 兼容矩阵。
- [ ] 全部 duplicate functions。
- [ ] 完整自然排序边界和 locale 行为。
- [ ] 增量 query refinement cache，而不仅是 GUI 两阶段查询。
- [ ] 与 Everything 公开语法文档逐项对照测试。

## P3：NTFS 完整语义

- [x] MFT 枚举和父路径重建。
- [x] USN 创建、删除、重命名和更新基础处理。
- [x] 多卷固定 NTFS 发现和命名空间。
- [x] 周期性常规/完整协调基础能力。
- [x] 初始 MFT 基线原地、有界并行补齐大小和修改时间。
- [ ] 创建时间、访问时间和 NTFS Change/最近变化时间的低内存基础索引。
- [x] 首次建库先发布名称索引、后台每批 4,096 条补齐元数据，提供 Event Log 进度并在批次边界响应停止。
- [x] 将多卷后台补齐结果通过 generation-bound 低内存 metadata WAL 持久化，并保存 durable cursor、完成状态和启动卷/USN sidecar。
- [x] 跨完整 reconciliation generation 按 ID/完整路径迁移可复用 metadata，并让后台批次跳过已知项。
- [ ] 每个 hard-link 目录入口的独立表示。
- [ ] 完整 file reference sequence number 重用处理。
- [ ] 明确 reparse/junction/symlink traversal 策略。
- [ ] mount point 和 volume GUID path 策略。
- [ ] 权限、所有者和 ACL 变化语义。
- [ ] ADS 列举和搜索。
- [ ] 离线卷、可移动卷和卷移除/重接状态机。
- [ ] 更强的 MFT/USN fuzz 与故障注入。

## P4：非 NTFS 和 provider

- [x] 普通递归扫描原型。
- [x] `ReadDirectoryChangesW` 触发的回退协调。
- [ ] FAT/exFAT 完整实时 provider。
- [ ] 网络共享 provider 和断线重连。
- [ ] 云盘占位符/provider 语义。
- [ ] 自动发现并统一管理非 NTFS 数据源。
- [ ] Linux provider。
- [ ] macOS provider。
- [ ] provider SDK 和稳定生命周期接口。

## P5：GUI 产品化

- [x] 原生搜索窗口、实时列表和系统托盘。
- [x] 全局快捷键、列管理、文件图标和选择感知右键菜单。
- [x] 拖放、双击打开、打开所在目录、重命名、删除、复制/移动。
- [x] 书签、筛选器、历史、文件列表和导出。
- [x] Everything 风格七菜单结构和菜单冒烟测试。
- [ ] Windows 原生 Shell context menu extension/完整 IContextMenu 托管。
- [ ] Windows Preview Handler 集成。
- [ ] 高 DPI、多显示器和可访问性完整测试。
- [ ] 深色模式和主题。
- [ ] 大结果集 paint/scroll 性能专项优化。
- [ ] 安装、首次建库和错误恢复的引导 UI。

## P6：安全、运维和发布

- [x] 本地 Pipe、拒绝远程客户端和显式 DACL。
- [x] NSIS 安装/卸载。
- [x] delayed-auto Windows 服务。
- [x] GitHub Actions 构建、测试、installer/portable artifact 和 tag release。
- [x] 文档同步 Hook 与 CI 检查。
- [ ] per-request impersonation。
- [ ] 按用户 ACL 过滤查询结果。
- [ ] 服务最小权限设计。
- [ ] 结构化日志、日志轮转和诊断包。
- [ ] 崩溃报告和隐私策略。
- [ ] 代码签名与安装包签名。
- [ ] 安全自动升级、回滚和数据库迁移。
- [ ] 稳定公开 SDK/API 版本策略。
- [ ] HTTP/ETP 服务；如实现 ETP，必须明确兼容范围。

## P7：内容搜索

- [ ] 定义与文件名目录分离的内容索引接口。
- [ ] Xapian 可选 provider 原型。
- [ ] 文本提取器隔离进程。
- [ ] 内容类型、大小、权限和隐私策略。
- [ ] 增量内容重建和删除传播。
- [ ] 内容查询与文件名查询组合。

## 完成标准

任何条目只有同时满足以下条件才能标记完成：

1. 实现已进入 `main`；
2. 有自动化测试或明确人工验证；
3. 用户/架构/运维/性能文档已同步；
4. `CHANGELOG.md` 已记录；
5. 不以单一 happy path 代替完整边界声明。

### 2026-07-26 第一阶段进展

已建立独立 Xapian 内容服务、独立 Pipe、CLI 和实验 UI，并完成纯文本提取、CJK n-gram、摘要/高亮、watcher 增量更新和基础自动测试。该阶段只验证技术路径，不改变现有文件名搜索服务。

P7 下一批工作按以下顺序推进：

1. 启动 stale-document reconciliation 和 watcher overflow 自动校准；
2. 可取消的长期查询 worker 与 UI 关闭生命周期；
3. 独立受限 extractor worker、超时和崩溃隔离；
4. 压缩正文 sidecar，避免完整正文直接放大 Xapian document data；
5. 多 root/provider 配置、SCM 服务和安装/恢复策略；
6. ACL impersonation、真实容量/性能/长期内存基线；
7. Xapian GPL 分发合规完成后，再考虑把入口合并进主 GUI。

## 2026-07-29 ?????? NTFS ??????

- [x] ?? generation/volume/root-bound ??/USN append-only WAL v2
- [x] torn-tail ???checksum?journal cursor ???? checkpoint ?????????
- [x] base + overlay - tombstone checkpoint consolidation ???
- [ ] generation directory + atomic current manifest + streaming/mapped consolidation
- [ ] NTFS object identity / directory-entry identity schema ? snapshot/WAL ??
- [ ] hard-link ???????????????????????
- [ ] MFT slot sequence ???????? parent?? WAL ?? hydration ??????
- [ ] ???????? reconciliation ???????????
- [ ] ?? Everything ???????????????????????
