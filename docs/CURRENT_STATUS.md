# 当前状态（2026-07-24）

本文描述当前 `main` 分支能力，不代表稳定版本承诺。项目目标是接近 Everything 的体验和性能，但目前不能称为完整复刻或完全兼容。

## 1. 已实现

### 索引与实时更新

- 自动发现带盘符的本地 NTFS 固定卷。
- 读取 NTFS MFT，重建父子关系和完整路径。
- 多卷记录命名空间合并。
- USN Journal 增量创建、删除、重命名和更新处理。
- 约每分钟协调、约每 30 分钟完整协调。
- 普通目录递归扫描，以及 `ReadDirectoryChangesW` 触发后的树级重新扫描回退。

### 持久化

- 校验和保护的 metadata snapshot。
- 临时文件 + write-through rename 原子替换。
- memory-mapped v2 紧凑 snapshot。
- live checkpoint 的追加 WAL、完整事务重放、撕裂尾部截断和 checkpoint consolidation。
- Catalog snapshot 保存可直接流式写紧凑节点和名称 arena，避免创建完整临时 `vector<FileRecord>`。

### 查询与性能路径

- 普通词、引号、限定字段、排除词。
- AND/OR/NOT、括号和固定优先级。
- `name:`、`path:`、`ext:`、`file:`、`folder:`。
- 通配符、基础正则、大小/日期/属性过滤。
- 大小写、全字、路径、变音符号选项。
- `dupe:name`、`dupe:size`、`dupe:name-size`。
- 基础 Explorer 风格自然排序和服务端排序。
- 65536 桶的 16-bit trigram hash 名称倒排索引，posting 按自然顺序位置做 delta/varint 压缩。
- 从必须出现的名称 trigram 中选择最稀疏 posting，最终由完整 evaluator 消除 hash collision 误报。
- raw 与 accent-folded 名称 gram，默认忽略变音符号时仍可走候选索引。

### 服务与 IPC

- Windows SCM 服务安装、启动、停止、状态和卸载。
- 本地 Named Pipe 版本化二进制协议。
- 4 MiB payload、1000 结果上限、超时/重试、精确读写。
- 4 个并发 Pipe worker。
- 拒绝远程客户端并设置显式 DACL。

### GUI

- Everything 风格七菜单布局。
- 搜索历史、筛选器、书签和设置持久化。
- 虚拟结果列表、Shell 图标、列管理和多字段排序。
- 输入活跃时 200 条快速结果，停止约 250 ms 后补全到最终上限。
- 搜索请求去重、后台元数据补齐、扩展名图标缓存、约 1 秒历史写入延迟。
- 双击打开、打开所在目录、剪切/复制/粘贴、复制/移动、重命名、回收站删除、属性和拖放。
- EFU/CSV/文本导出以及文件列表编辑。
- 系统托盘和 `Ctrl+Alt+Space` 全局快捷键。
- NSIS 管理员安装包和 native fallback launcher。

## 2. 部分实现

### Everything 查询兼容

已经有布尔、正则、大小/日期/属性、筛选器、书签、历史和 duplicate 的基础能力，但缺少 Everything 的全部函数、宏、相对日期、属性族、转义细节和完整兼容测试矩阵。

### NTFS 语义

能够处理常见 MFT/USN 场景，但 hard-link 的每个目录入口、sequence number 的全部重用边界、reparse/junction/symlink/mount point 策略、权限变化、ADS、离线卷和可移动卷仍不完整。

### WAL/增量持久化

单卷 live 路径具有 WAL 和 checkpoint 恢复；默认多卷服务仍以完整 snapshot 为主，尚未成为统一的 base snapshot + append-only WAL + delta replay + checkpoint consolidation 数据库。多卷 snapshot 当前在完整 MFT reconciliation 时复用同一记录向量，不再每 5 分钟额外物化所有完整路径，因此降低了内存峰值，但 snapshot 新鲜度与完整 reconciliation 周期绑定。

### 非 NTFS

递归扫描和 Windows watcher 可作为兼容回退，但 FAT/exFAT、网络共享、云盘和多 provider 的实时语义尚未完整实现。

### 安全和发布

已有本地 Pipe DACL、NSIS、服务自启动、CI artifact 和 tag release；仍缺 per-request impersonation、按用户搜索权限隔离、代码签名、自动升级、崩溃报告、日志轮转和稳定 SDK。

## 3. 未实现

- Xapian 或其他文档内容全文索引。
- Everything ETP 兼容服务。
- 完整 Windows Shell 原生上下文菜单扩展。
- Windows Preview Handler 完整预览。
- Linux/macOS provider。
- 完整 FAT/exFAT、网络共享、云盘 provider。
- ADS 内容搜索。
- 签名安装包和二进制。
- 自动更新服务。

## 4. 当前性能观察

3,263,985 条真实 snapshot、名称自然排序、limit 1000，Release 构建独立运行 3 次；每次查询 9 次，下表是各次 p50 的中位数：

| 查询 | 本地索引 p50 | 已安装服务历史 p50 |
|---|---:|---:|
| `1` | 2.08 ms | 2.43 ms |
| `12` | 6.12 ms | 3.85 ms |
| `123` | 2.97 ms | 2.18 ms |
| `txt` | 1.60 ms | 1.25 ms |
| `windows` | 1.67 ms | 1.29 ms |
| `report` | 4.55 ms | 3.02 ms |
| `123456789.txt` | 0.21 ms | 0.17 ms |
| `path:test1` | 17.84 ms | 19.60 ms |

本轮名称索引构建中位数约 27.041 秒。单索引 Working Set 中位数约 628.79 MiB，结构容量约 619.34 MiB。posting 约 72.00 MiB，66,803,856 个 entry 平均约 1.13 字节，是原始 `uint32_t` posting 容量的约 28%。

这些数据是特定机器和 snapshot 的开发基线，不是通用 SLA；已安装服务查询列来自上一轮 IPC 基线。完整方法见 [PERFORMANCE.md](PERFORMANCE.md)。

## 5. 已知资源问题

用户观察到的旧安装服务稳定内存约为 1.85–2 GiB；更早的开发版本曾达到 Working Set 约 3589 MB、Private Bytes 约 3662 MB。本轮通过释放 rvalue 源记录、普通文件路径组件化、128 位名称 Bloom、delta/varint posting 和 48 字节 `CompactRecord`，把真实 326 万条单搜索索引降到约 628.79 MiB Working Set。

当前已安装完整服务实测：

- reconciliation 峰值约 1.54 GiB；
- 首轮稳定约 929 MiB Working Set / 930 MiB Private Bytes；
- 后续每 5 秒一次、持续约 6 分钟的 72 次采样中，Working Set 为 870.66–930.54 MiB，Private Bytes 为 942.89–942.92 MiB；
- 采样期间没有重新增长到 2–3 GiB。

因此 900 MiB 仍然偏大：它约为用户观察到的 Everything 300 MiB 的 3 倍。当前主要剩余项是 `NtfsCatalog` 与 `MetadataIndex` 的两套基础节点/名称、约 223.55 MiB UTF-16 字符 arena、约 149.41 MiB Bloom 签名，以及尚未持久化/mmap 的搜索结构。下一阶段优先做搜索索引持久化/mmap、Catalog 与查询层共享基础数据，并评估目录级路径签名以减少每文件路径 Bloom。

## 6. 发布判断

当前可用于开发验证和个人机器试用，但还不应作为具备完整权限隔离、稳定升级、签名供应链和跨 provider 支持的企业级发布。任何状态变化都必须同步更新本文件和 `CHANGELOG.md`。
