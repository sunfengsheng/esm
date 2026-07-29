# 当前状态（2026-07-29）

本文描述当前 `main` 分支能力，不代表稳定版本承诺。项目目标是接近 Everything 的体验和性能，但目前不能称为完整复刻或完全兼容。

## 1. 已实现

### 索引与实时更新

- 自动发现带盘符的本地 NTFS 固定卷。
- 读取 NTFS MFT，重建父子关系和完整路径。
- 默认多卷服务在初始 MFT 路径重建后先保存并发布名称/路径索引，再以每批 4,096 条、默认最多 4 个 worker 的有界后台任务逐步补齐大小、修改时间、属性和目录状态；CLI、前台 server 和单卷 MFT 服务仍同步补齐。
- 多卷记录命名空间合并。
- USN Journal 增量创建、删除、重命名和更新处理；直接变化项在路径解析后先使旧大小/时间失效，再刷新大小、修改时间和属性。若文件已删除或不可访问，记录仍可按名称搜索，但旧元数据不会继续参与过滤或后续 generation 复用。
- 约每 250 ms 跟随 USN Journal，并约每分钟检查挂载卷集合；完整 MFT reconciliation 只在启动建立 live 边界、USN checkpoint 失效/读取失败或卷集合变化时触发。协调器优先追赶 USN，再处理一个后台元数据批次；过期路径结果不会覆盖重命名或删除后的记录。
- 普通目录递归扫描，以及 `ReadDirectoryChangesW` 触发后的树级重新扫描回退。

### 持久化

- 校验和保护的 metadata snapshot。
- 临时文件 + write-through rename 原子替换。
- memory-mapped v2 紧凑 snapshot。
- live checkpoint 的追加 WAL、完整事务重放、撕裂尾部截断和 checkpoint consolidation。
- Catalog snapshot 保存可直接流式写紧凑节点和名称 arena，避免创建完整临时 `vector<FileRecord>`。
- 多卷后台补齐新增 generation-bound metadata WAL：每批只追加成功读取项的 ID、路径 fingerprint、大小、修改时间和属性，并持久化 `next_id`/完成状态；启动可重放并从 durable cursor 续跑，不需要导出完整 `vector<FileRecord>`。完整 reconciliation 产生新 generation 时，若当前 live USN 状态仍连续可信，并且 MFT 扫描完成后新旧共有卷可再次追赶到当前 journal 边界，会按 ID/完整路径从当前 live base/overlay 复用已知元数据，并把复用结果直接写入新 snapshot；启动 state 失效、扫描后追赶失败、普通 USN 读取失败或 journal gap 时禁用复用。
- 多卷状态 sidecar 保存 snapshot generation、卷身份/root file ID、journal ID、原始 USN boundary 和 live 状态。重新发现的卷集合及 USN checkpoint 全部有效时，健康重启跳过立即完整 MFT reconciliation；状态缺失、损坏、generation 不匹配、卷变化或 journal gap 时保留名称 snapshot 提供查询并安排完整修复。

### 查询与性能路径

- 普通词、引号、限定字段、排除词。
- AND/OR/NOT、`&&`/`||`/`|`、圆括号/尖括号分组和固定优先级。
- `name:`、`path:`、`filelist:`（完整文件名/路径列表）、`ext:`（含分号列表）、`file:`、`folder:`、`root:`、`count:`。
- `child:`、`empty:`、`childcount:`、`childfilecount:`、`childfoldercount:`，按当前 base + overlay 视图匹配或统计目录直接子项。
- 通配符、基础正则、数字/大小范围、Everything 大小常量、修改日期、自然周期/滚动 N 单位/月名/星期名日期常量和属性过滤。
- 大小写、全字、路径、变音符号选项。
- `dupe:name`、`dupe:size`、`dupe:name-size`。
- 基础 Explorer 风格自然排序和服务端排序。
- 65536 桶的 16-bit trigram hash 名称倒排索引，posting 按自然顺序位置做 delta/varint 压缩。
- 从必须出现的名称 trigram 中选择最稀疏 posting，最终由完整 evaluator 消除 hash collision 误报。
- raw 与 accent-folded 名称 gram，默认忽略变音符号时仍可走候选索引。
- 文件名 GUI 使用首键 15 ms、150 ms 输入突发内 60 ms 的自适应防抖；保持单一在途 Pipe 查询并通过 generation 丢弃过期响应。
- 最终结果不再无条件逐项访问文件系统；默认只补齐修改时间仍未知的结果，创建/访问/NTFS Change 时间排序才触发全结果补齐。两种路径都使用轻量槽位+路径请求、最多 4 个 worker 和 UI 原位数值更新。

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

已经有布尔、正则、大小/日期/属性、筛选器、书签、历史和 duplicate 的基础能力；本轮补充了 `filelist:` 完整文件名/路径列表，以及 `startwith:`、`endwith:`、`len:`、`depth:`/`parents:`、`parent:`/`infolder:`/`nosubfolders:`、`root:`、`count:`、`child:`、`empty:`、`childcount:`、`childfilecount:`、`childfoldercount:`、`|`/`< >`、`ext:` 分号列表、数字/大小范围、大小常量、`datemodified:`，以及 `next/coming` 自然周期、`last/past/prev/next/coming<N><单位>`、英文月份/星期名称日期常量；周起点读取 Windows 当前用户区域设置，并区分“上一个完整周期”的 `last/prev` 与滚动阈值 `past`。`filelist:` 已覆盖大小写、变音符号、通配符和 base + overlay 语义；单一正向、无通配符的精确列表会复用现有 raw/accent-folded 名称前缀表，并只为 basename 精确候选重建完整路径，不增加常驻多值哈希索引。带 `*`/`?` 的列表、复杂布尔组合和其他无法安全缩小的形式仍使用完整 evaluator；`child:` 与子项统计查询目前需要按需遍历当前目录关系，功能结果覆盖增量 overlay，但尚无 Everything 等级的常驻专用子项索引。仍缺少 Everything 的其余函数、宏、创建/访问/最近变化时间族、`unknown`、完整区域化日期与范围、属性族、转义细节和完整兼容测试矩阵。初始全盘基线仍通过 `FSCTL_ENUM_USN_DATA` 获取名称/父关系；默认多卷服务先发布名称索引，再按重建路径在后台逐批补齐紧凑索引需要的大小、修改时间、属性和目录状态，因此补齐期间名称查询可用，但 `size:` / `dm:` 结果和对应列会逐步完善。无法访问、枚举后瞬时删除、离线或特殊 NTFS 条目可能保留未知/原值；创建时间、访问时间和 NTFS Change/最近变化时间尚未进入紧凑基础索引。

### NTFS 语义

能够处理常见 MFT/USN 场景。多卷服务后台补齐按批次检查停止请求，并通过路径一致性校验丢弃 rename/delete 竞争产生的过期结果。真实 3,300,421 条 snapshot 路径样本中，独立元数据 API 阶段成功补齐 3,293,996 条、失败 6,425 条；失败项不会阻断名称索引，但相关大小/修改时间可能未知。hard-link 的每个目录入口、sequence number 的全部重用边界、reparse/junction/symlink/mount point 策略、权限变化、ADS、离线卷和可移动卷仍不完整。

### WAL/增量持久化

单卷 live 路径具有 WAL 和 checkpoint 恢复。默认多卷服务仍以完整名称 snapshot 为基线，但后台大小/修改时间/属性已通过独立 generation-bound metadata WAL 持久化，并由状态 sidecar 保存 durable cursor 和启动恢复所需的卷/USN boundary。健康重启可重放元数据并续跑；WAL 损坏时保留名称 snapshot、从 ID 0 重新补齐。完整 reconciliation 创建新 generation 前，只有当前 live USN 状态仍连续可信，并且 MFT 扫描后新旧共有卷完成一次额外 USN catch-up，才会把新 MFT 记录原地按 ID 排序，并从当前 live base/overlay 线性复用 ID、完整路径一致且修改时间已知的大小、时间和属性；后台批次跳过这些已知项，只对新文件、路径变化、删除竞争或旧读取失败项执行文件 I/O。启动 state 失效、扫描后 catch-up 失败、普通 USN 读取失败或 journal gap 会禁用复用，因为旧索引可能漏掉路径不变的内容修改。多卷名称变化和 USN delta 仍未形成通用 append-only WAL，运行期 cursor 也不能覆盖 snapshot 所代表的原始 journal boundary；因此该路径尚不是统一的 base snapshot + delta replay + checkpoint consolidation 数据库。

### 非 NTFS

递归扫描和 Windows watcher 可作为兼容回退，但 FAT/exFAT、网络共享、云盘和多 provider 的实时语义尚未完整实现。

### 安全和发布

已有本地 Pipe DACL、NSIS、服务自启动、CI artifact 和 tag release；仍缺 per-request impersonation、按用户搜索权限隔离、代码签名、自动升级、崩溃报告、日志轮转和稳定 SDK。

### 测试运行库基线

MinGW/UCRT 的 `esm_tests.exe` 现在与发布程序一样静态链接运行库，避免从 `PATH` 误载 `mingw64` DLL 导致 `0xc0000139`。2026-07-28 在 `build-ucrt-vendor-final` Release 配置中，主测试目标重新构建后通过。

## 3. 未实现

- Everything ETP 兼容服务。
- 完整 Windows Shell 原生上下文菜单扩展。
- Windows Preview Handler 完整预览。
- Linux/macOS provider。
- 完整 FAT/exFAT、网络共享、云盘 provider。
- ADS 内容搜索。
- 签名安装包和二进制。
- 自动更新服务。

## 4. 当前性能观察

3,264,188 条真实 snapshot、名称自然排序、limit 1000，Release 构建独立运行 3 次；每次查询 9 次，下表是各次 p50 的中位数：

| 查询 | 最新代码本地索引 p50 | 已安装旧服务单次验证 |
|---|---:|---:|
| `1` | 2.01 ms | 9.98 ms（单次验证） |
| `12` | 4.59 ms | 已验证可用 |
| `123` | 1.87 ms | 已验证可用 |
| `txt` | 1.47 ms | 已验证可用 |
| `windows` | 1.61 ms | 已验证可用 |
| `report` | 3.13 ms | 已验证可用 |
| `123456789` | 1.10 ms | 已验证可用 |
| `123456789.txt` | 0.03 ms | 1.18 ms（单次验证） |
| `path:test1` | 41.40 ms | 已验证可用 |

最新代码索引构建中位数约 21.972 秒。单索引 Working Set 中位数约 437.89 MiB，Private Bytes 中位数约 436.32 MiB，结构容量约 429.41 MiB。posting 约 72.01 MiB，66,808,608 个 entry 平均约 1.13 字节，是原始 `uint32_t` posting 容量的约 28%。

这些数据是特定机器和 snapshot 的开发基线，不是通用 SLA；已安装服务列仍来自上一版二进制的单次功能验证，不应当作本轮 p50。`path:` 查询存在明确慢路径。完整方法见 [PERFORMANCE.md](PERFORMANCE.md)。

本轮 `esm_gui_pipeline_benchmark` 合成微基准显示：默认 1000 条结果下，旧完整结果交接估算 0.38 MiB，轻量路径请求 0.22 MiB、返回数值 payload 0.05 MiB；10 万条压力样本分别为 39.22 MiB、22.27 MiB 和 5.34 MiB。该结果只描述 GUI 元数据交接的数据形状，不是端到端搜索延迟或真实进程峰值。测试方法见 [PERFORMANCE.md](PERFORMANCE.md)。

2026-07-29 的工作区 GUI 功能自动化确认：精确查询 `123456789.txt` 返回 2 条，0 B/565 B 和修改时间均能显示，状态为 `2 个结果` 而不是 `2+`；宽查询 `1` 先显示 `200+`，随后 refinement 到 1000 条。短响应现在直接进入缺失元数据补齐和历史流程，只有首屏刚好达到 200 条时才发送第二次最终查询。该检查不是稳定的真实键盘端到端 p50/p95。

## 5. 已知资源问题

用户观察到的旧安装服务稳定内存约为 1.85–2 GiB；更早的开发版本曾达到 Working Set 约 3589 MB、Private Bytes 约 3662 MB。第一阶段通过释放 rvalue 源记录、普通文件路径组件化、128 位名称 Bloom、delta/varint posting 和 48 字节 `CompactRecord`，把真实 326 万条单搜索索引降到约 628.79 MiB Working Set。

随后通过目录级共享路径签名、正常目录/文件全父链组件化以及多卷服务移除常驻 `NtfsCatalog`，上一正式基线的单索引降到约 474.66 MiB Working Set / 473.04 MiB Private Bytes，旧安装服务首次协调后曾约 479.33 MiB Private Bytes。

2026-07-25 的最新代码进一步把 `CompactRecord` 从 48 字节压到固定 40 字节：正常父关系保存 31 位记录索引，缺失父项才使用稀疏 `ParentIdAnchor`。路径签名 owner 从每记录 32 位数组改为目录 bitset + rank prefix + 稀疏 `PathSignatureFallback`，owner 元数据由约 12.45 MiB 降到约 0.58 MiB。真实 3,264,188 条单索引稳定值为约 437.89 MiB Working Set / 436.32 MiB Private Bytes，基础结构容量约 429.41 MiB；`path:test1` p50 约 41.40 ms，纯名称查询约 0.03–4.59 ms。

同机 Everything 1.4.1.1030 主进程和辅助进程合计约 316.77 MiB Private Bytes。最新 ESM 单索引基准约 436.32 MiB，修复后的完整安装服务首轮协调后中位数约 437.00 MiB，均粗略为其 1.38 倍。ESM 约 326.4 万条、snapshot 约 966.69 MiB；Everything 约 369.9 万条、数据库约 146.20 MiB，两者不是相同记录集或存储格式，不能声称已经达到 Everything。

旧安装服务在 2026-07-25 08:49 第三次协调完成后曾稳定约 953.48 MiB Private Bytes / 935.32 MiB Working Set，构建中观察到约 2501.88 MiB Private Bytes。本轮已把多卷聚合改为“先收集各卷结果、计算总记录数、一次性 reserve、再移动合并”，避免逐卷追加产生大块中间 `vector<FileRecord>`；协调后调用 `_heapmin` 归还完全空闲的 CRT heap region，并保留 `HeapCompact`。

2026-07-25 10:25 已通过 UAC 安装 SHA-256 为 `66DA638E502E33DE06D2F4CE93F1C37220369D2B121CC29F3C5F3C645077A72B` 的最新服务。Event Log 显示 10:26:06 完成 C:/D:/E: 首次全量协调；随后 12 次采样的 Private Bytes 为 436.97–437.03 MiB、中位数 437.00 MiB，Working Set 为 441.24–441.29 MiB、中位数 441.27 MiB。D 盘 `D:\test1\123456789.txt` 可正常返回，热查询为 0.333–0.449 ms；首次冷查询曾出现 1765.96 ms 异常样本，仍需继续分析。

18:12 的长期复查确认固定周期问题仍存在：服务在 18:04:41 完整协调后为约 901.44 MiB Private Bytes / 886.39 MiB Working Set，历史峰值约 3426.80 MiB Private Bytes。精确 reserve 没有解决数百万独立路径字符串和新旧索引重叠造成的 CRT heap 保留。最新工作区已取消健康状态下每 30 分钟无条件全量替换，改为 USN 增量跟随、每分钟轻量卷发现，并只在启动、journal gap/错误或卷集合变化时完整修复。Release SHA-256 为 `B808B460467F32BC0567EBD9A2EB28853EABF05B71FD570012E911D0C56CCBB1`，已于 18:20 通过 UAC 安装；18:21:36 首次 repair 后为约 436.50 MiB Private Bytes / 439.54 MiB Working Set。仍需跨过原 30 分钟边界验证没有周期性全量替换。

单进程真实 snapshot 构建采用 10 ms 采样时，峰值仍约 2279.45 MiB Working Set / 2299.08 MiB Private Bytes；完整 repair 仍可能达到多 GiB。下一阶段先验证新调度在超过原 30 分钟周期后保持约 437 MiB，再继续减少完整 `vector<FileRecord>` 物化、压缩 UTF-16 字符 arena 和排序索引、把稳定搜索结构持久化或 mmap。

## 6. 发布判断

当前可用于开发验证和个人机器试用，但还不应作为具备完整权限隔离、稳定升级、签名供应链和跨 provider 支持的企业级发布。任何状态变化都必须同步更新本文件和 `CHANGELOG.md`。

## 7. 独立内容搜索原型（2026-07-26）

已完成第一阶段可运行原型：

- `esm_content_service.exe`：单根或多根后台启动扫描、显式 `--all-fixed` 固定卷发现、每根 Xapian 持久分片数据库和递归 watcher、全局聚合查询、独立 `everything_sm_content` Pipe；
- `esm_content.exe`：160 ms debounce、单一长期 IPC worker、过期响应丢弃、服务状态灯、Shell 文件图标、名称/路径/内容摘要/相关度列、黄色匹配高亮、按钮/右键菜单/快捷键，以及显示文件名、完整路径和高亮索引摘要的右侧可开关预览窗格；
- `esm_content_cli.exe`：status/search 真实 IPC 诊断；
- 纯文本扩展名白名单、UTF-8/UTF-16 BOM/本地 ANSI 解码、二进制 NUL 检测和 4 MiB 默认上限；`.docx` 支持 IFilter + 内置 ZIP/XML 回退，`.pdf` 支持 IFilter + 受限基础文本流回退，旧 `.doc` 依赖系统 IFilter；
- Xapian 默认 AND、phrase/boolean/love-hate/wildcard 和 CJK n-gram；
- 仓库固定包含 Xapian Core 1.4.31 官方发布源码，MinGW 默认从 `third_party/xapian-core` 构建静态库，CI 不依赖预编译 Xapian 包；
- 协议、纯文本/DOCX/PDF 提取器、英文/中文查询、摘要高亮、upsert、delete、分片聚合、路径过滤、root key，以及查询与 60 次 commit 交错的并发自动测试；
- 本机真实 E2E 已验证英文、中文、两个临时根聚合查询、第二根 watcher 新增文件命中、两个分片目录和停止后重启查询；真实约 26.47 万文档数据库在修复后连续完成多轮查询且服务保持存活。

真实内容查询速度目前不快：2026-07-26 使用约 264,692～264,693 个文档、约 4.63 GiB Xapian 数据库，在后台仍扫描时，`limit=100` 的不同查询单次 Named Pipe 往返为 671.9～5807.3 ms；`limit=10` 的重复样本约 237.8～756.9 ms。样本数不足以形成发布级 p50/p95，但足以否定“已经达到 Everything 式即时匹配”。摘要生成会读取 document data 中的完整正文，是当前最明显的查询慢路径。

仍属于实验状态：没有 SCM 注册、启动 stale-document reconciliation、通知溢出自动修复、独立 extractor worker、OCR、完整复杂 PDF/Office 语义、ACL impersonation、网络/云盘/可移动卷 provider、持久任务队列、首次建库吞吐/长期内存/真实 GUI 端到端基线和 Xapian GPL 发布合规方案。现有文件名搜索链路未修改，内容服务不可用不会影响 `esm_service.exe`。详情见 [CONTENT_SEARCH.md](CONTENT_SEARCH.md)。


## 2026-07-29：名称/USN delta WAL 与 checkpoint consolidation

默认多卷 `mft-auto` 已具备以下持久化能力：

- 名称、创建、删除、重命名和相关 USN 增量使用 generation-bound append-only WAL v2；
- state 保存 snapshot generation、卷身份、卷根、root file ID、journal ID、原始 USN 边界和 durable cursor；
- 每个已提交 USN batch 先以 write-through 事务写入并 flush，再推进 durable cursor；
- 启动按 snapshot、名称/USN WAL、元数据 hydration WAL、state sidecar 的顺序校验和恢复；
- 不完整事务尾部可截断；generation、卷集合、root 或 journal gap 不可信时保留可用名称基线并安排 reconciliation；
- checkpoint 按 `base + overlay - tombstone` 生成新 generation，并原子切换 snapshot/WAL/state；
- 已有小规模 crash-recovery 与 10 万 delta 合成 consolidation 测试。

仍未完成的边界：checkpoint writer 仍可能物化完整 `vector<FileRecord>`；真实百万级 MFT + 增量场景的耗时、Private Bytes、磁盘 flush 和 SCM/UAC 端到端恢复尚未建立发布级基线。hard-link 每个目录入口的独立表示、完整 MFT slot/parent sequence 传播和 Everything 100% 功能/性能兼容仍未完成。

## 2026-07-29：内容搜索独立应用状态

已完成第一阶段正式隔离：

- 正式 GUI 可执行文件为 `esm_content.exe`，旧 `esm_content_lab.exe` 目标已移除；
- 内容配置、索引和 Pipe 默认分别为 `%LOCALAPPDATA%\everything_sm_content\content.ini`、`%LOCALAPPDATA%\everything_sm_content\index` 和 `everything_sm_content_service`；
- GUI 可按需使用 `CREATE_NO_WINDOW` 启动同目录的 `esm_content_service.exe`；
- 内容服务按 Pipe 创建单实例互斥体，避免 GUI 重复启动相同扫描任务；
- GUI、服务和 CLI 都支持 `--config`，命令行参数仍可覆盖配置；
- 默认首次配置只索引当前用户目录，不会未经确认执行 `--all-fixed`；
- 新增设置持久化、DOCX/PDF 提取自动测试、TXT/DOCX/PDF 临时单根服务/CLI E2E 验证和独立 NSIS 开发安装包；
- 内容应用使用与文件名搜索不同的专属图标；GUI 增加 160 ms 防抖、长期 worker、状态灯、Shell 图标、结果按钮、右键菜单、快捷键和 Everything 风格的右侧摘要预览窗格；
- 文件名搜索进程、数据库、安装包和 IPC 未改为依赖内容搜索。

仍未完成：Windows Preview Handler、PDF/Word 页面级渲染、图片/OCR 预览、SCM 内容服务、持久任务队列、删除 reconciliation、受限 extractor worker、ACL/per-request impersonation、完整复杂 PDF/Office 语义、真实整机长期基准以及 GPL 公开分发审查。因此内容搜索仍是开发功能，不能声称达到生产发布标准。
