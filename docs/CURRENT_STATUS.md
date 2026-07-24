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

3,264,136 条真实 snapshot、名称自然排序、limit 1000，Release 构建独立运行 3 次；每次查询 9 次，下表是各次 p50 的中位数：

| 查询 | 本地索引 p50 | 已安装服务单次验证 |
|---|---:|---:|
| `1` | 1.55 ms | 9.98 ms（单次验证） |
| `12` | 4.81 ms | 已验证可用 |
| `123` | 2.81 ms | 已验证可用 |
| `txt` | 2.17 ms | 已验证可用 |
| `windows` | 1.36 ms | 已验证可用 |
| `report` | 4.46 ms | 已验证可用 |
| `123456789.txt` | 0.04 ms | 1.18 ms（单次验证） |
| `path:test1` | 43.46 ms | 已验证可用 |

本轮名称索引构建中位数约 21.777 秒。单索引 Working Set 中位数约 474.66 MiB，Private Bytes 中位数约 473.04 MiB，结构容量约 466.16 MiB。posting 约 72.01 MiB，66,807,284 个 entry 平均约 1.13 字节，是原始 `uint32_t` posting 容量的约 28%。

这些数据是特定机器和 snapshot 的开发基线，不是通用 SLA；已安装服务列只包含本轮单次功能验证，不应当作 p50。`path:` 查询存在明确回退。完整方法见 [PERFORMANCE.md](PERFORMANCE.md)。

## 5. 已知资源问题

用户观察到的旧安装服务稳定内存约为 1.85–2 GiB；更早的开发版本曾达到 Working Set 约 3589 MB、Private Bytes 约 3662 MB。第一阶段通过释放 rvalue 源记录、普通文件路径组件化、128 位名称 Bloom、delta/varint posting 和 48 字节 `CompactRecord`，把真实 326 万条单搜索索引降到约 628.79 MiB Working Set。

第二阶段把每记录 256 位完整路径 Bloom 改为目录级共享路径签名和每记录 32 位 owner，并让 posting 构建直接写最终 varint byte span。第三阶段让正常目录和文件都使用名称 + 父链重建路径，字符 arena 从约 223.56 MiB 降到约 141.16 MiB；真实单索引进一步降到约 474.66 MiB Working Set / 473.04 MiB Private Bytes。代价是 `path:test1` 的 3 次 p50 中位数约 43.46 ms。

默认多卷 `mft-auto` 服务现已直接把命名空间化 USN 增量应用到 `MetadataIndex`，不再长期保留每卷重复的 `NtfsCatalog`。最终安装二进制与构建产物 SHA-256 一致；索引替换后连续 12 次、每 5 秒采样均为约 479.28 MiB Working Set / 479.33 MiB Private Bytes。优化前同机稳定 Private Bytes 约 957.6–958.0 MiB，本轮下降约 50%。

同机 Everything 1.4.1.1030 主进程和辅助进程合计约 316.77 MiB Private Bytes；当前 ESM 约为其 1.51 倍，已经不再是约 3 倍，但仍然偏大，不能声称达到 Everything。ESM 约 326.4 万条、snapshot 约 966.69 MiB；Everything 约 369.9 万条、数据库约 146.20 MiB，两者不是相同记录集或存储格式。

单进程真实 snapshot 构建采用 10 ms 采样时，峰值仍约 2154.98 MiB Working Set / 2174.28 MiB Private Bytes；完整服务启动 reconciliation 的既有峰值约 2.57 GiB Private Bytes。下一阶段优先避免完整 `vector<FileRecord>` 物化、压缩 `CompactRecord`/字符串 arena，并把稳定搜索结构持久化或 mmap。

## 6. 发布判断

当前可用于开发验证和个人机器试用，但还不应作为具备完整权限隔离、稳定升级、签名供应链和跨 provider 支持的企业级发布。任何状态变化都必须同步更新本文件和 `CHANGELOG.md`。
