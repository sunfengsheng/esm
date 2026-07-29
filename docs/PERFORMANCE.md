# 性能说明

## 1. 性能目标

项目希望文件名查询达到可交互的毫秒级响应，但当前不能声称已达到 Everything 的全部性能指标。性能需要分层观察：

1. MFT/扫描建库时间；
2. snapshot 载入和名称索引构建时间；
3. 索引内查询执行时间；
4. Named Pipe 往返时间；
5. GUI 调度、列表刷新、Shell 图标和元数据补齐；
6. 内存、snapshot/WAL 大小和后台协调成本。

## 2. 当前查询优化

### natural-order delta/varint trigram 倒排索引

名称索引使用 65,536 个 16-bit trigram hash bucket。每个 bucket 保存 `natural_name_order` 中单调递增的位置，再做 unsigned delta + varint 编码；查询只解码被选中的最稀疏 posting，随后通过自然顺序数组还原记录索引并运行完整 evaluator。

该方案显著降低普通名称子串查询的扫描量，并把 posting 从固定 4 字节/条压缩到当前真实样本约 1.13 字节/条。16-bit hash 会产生 collision，但只影响候选数量，不影响最终正确性。

### 精确 `filelist:` 候选扫描

单一正向、非正则且不含通配符的 `filelist:` 复用已有名称前缀表，不新增常驻文件名或完整路径哈希表。每个候选先按 basename 的首字符/前两个字符范围缩小 base 记录，再做完整文件名校验；只有名称精确命中的记录才重建路径并运行统一 evaluator。忽略变音符号时同时扫描 accent-folded 前缀表，查询期只用小型 `uint32` 集合去重，overlay 仍完整检查。带 `*`/`?` 的列表和复杂布尔组合仍回退原候选路径。

### GUI 连续输入

- 输入活跃：请求最多 200 条；
- 停止约 250 ms：补全到配置上限，默认 1000；
- 丢弃过期 generation 的响应；
- 相同请求去重；
- 服务端已排序结果不在 GUI 重复排序；
- 历史约 1 秒后写盘；
- 图标按扩展名缓存；
- 大小和时间后台补齐。

## 3. 2026-07-25 开发基线

数据源：

```text
C:\ProgramData\everything_sm\indexes\mft-index.snapshot
record_count = 3,264,188
sort = name natural order
limit = 1000
```

### 索引内查询 p50

测试使用 Release 构建，同一真实 snapshot 连续独立运行 3 次；每次重新载入 snapshot、重建索引，每个查询运行 9 次。下表取 3 次运行各自 p50 的中位数，不是 GUI 端到端延迟。

| 查询 | p50 |
|---|---:|
| `1` | 2.01 ms |
| `12` | 4.59 ms |
| `123` | 1.87 ms |
| `txt` | 1.47 ms |
| `windows` | 1.61 ms |
| `report` | 3.13 ms |
| `123456789` | 1.10 ms |
| `123456789.txt` | 0.03 ms |
| `path:test1` | 41.40 ms |

### 已安装服务 IPC 查询 p50

| 查询 | 服务端 p50 |
|---|---:|
| `1` | 2.43 ms |
| `12` | 3.85 ms |
| `123` | 2.18 ms |
| `txt` | 1.25 ms |
| `windows` | 1.29 ms |
| `report` | 3.02 ms |
| `123456789.txt` | 0.17 ms |
| `path:test1` | 19.60 ms |

该 IPC 表仍是上一轮同机已安装版本基线。当前安装目录中的服务二进制 SHA-256 为 `2AC18ACBC17744D7C9B31B51671BF87A0050F04670D3997FBB02C86DE7383171`，本轮 Release 构建产物为 `1D40AC4D57EB1C1A96AB5EF5EA44A78DCBBA5BF5876779A3C24F86168AD060D9`；两次 UAC 提升均被取消，因此不能把该 IPC 表与本轮约 436 MiB 的单进程索引数据混为一次端到端测试。

### 优化前后

| 查询 | 优化前 | 当前索引内 p50 |
|---|---:|---:|
| `report` | 约 276 ms | 约 3.13 ms |
| `123456789.txt` | 约 466 ms | 约 0.03 ms |

本轮最终代码的 3 次真实索引构建耗时为 21.718、21.972、22.505 秒，中位数约 21.972 秒。`CompactRecord` 和路径签名 owner 压缩后，`path:test1` 的 3 次 p50 中位数为 41.40 ms，比上一正式开发基线的 43.46 ms 略好；纯名称查询仍大致为 0.03–4.59 ms。以上均为索引内基准，不代表 GUI、IPC、图标和元数据补齐的端到端延迟。

## 4. 2026-07-28 精确 `filelist:` 安装服务对照

测试方法：在同一 Windows 机器上复用同一个 `C:\ProgramData\everything_sm\indexes\mft-index.snapshot`（测量时 1,022,362,538 字节），先运行提交 `9e905a7` 的 Release `esm_service.exe`，再运行本轮 Release 二进制；每次替换后重启同一个 `everything_sm` 服务，使用已安装 `esm_cli.exe` 经同一个 Named Pipe 查询，每条查询连续运行 5 次。PowerShell 调用原生 CLI 时把参数写成 `'filelist:\"...\"'`，让 CRT 接收到查询中的字面双引号；否则 `|` 会被解析成普通布尔 OR，不能作为 `filelist:` 基准。下表是 CLI 输出的服务端计时，不包含 GUI、Shell 图标和人工输入延迟。

| 查询 | `9e905a7` 5 次范围 / 中位数 | 本轮 5 次范围 / 中位数 |
|---|---:|---:|
| `filelist:"123456789.txt|definitely_missing_esm_probe.txt"` | 778.603–950.810 ms / 854.474 ms | 11.707–23.602 ms / 12.899 ms |
| `filelist:"D:\test1\123456789.txt|D:\definitely_missing_esm_probe.txt"` | 3790.31–5999.75 ms / 4091.17 ms | 11.622–22.988 ms / 13.122 ms |

两组都正确返回 `D:\test1\123456789.txt`。按这 5 次样本的中位数计算，精确文件名列表约快 66 倍，精确完整路径列表约快 312 倍；这是固定机器/固定 snapshot 的小样本对照，不是统计充分的 p50/p95，也不能外推为 Everything 等级性能。本轮安装包为 `everything_sm-0.1.0-setup.exe`，大小 2,551,461 字节，SHA-256 为 `D440FCA59BF1B20AFCB772D8A9079E6CCFBCDF5174CD664FA0FD536D50EE64A2`。

通配符样本 `filelist:"123456789.*"` 仍走完整 evaluator；重启后的 5 次服务端结果波动约 485–1299 ms，受冷页和后台活动影响明显，因此本轮不把它列为优化前后结论。后续应为可提取固定 basename 前缀的通配符设计候选范围，并用更长时间序列报告 p50/p95。

## 5. GUI 合成连续变化测试

测试通过跨进程 `SetWindowText` 连续改变 ComboBox 文本，共 62 次：

- 平均 UI 调用延迟：0.25 ms；
- P50：0.18 ms；
- P95：0.44 ms；
- 最大：3.75 ms；
- 测试期间窗口保持可响应。

**限制：** 这是合成消息调度测试，不包含真实键盘输入、IME、显示器刷新、Shell 图标、完整 IPC 往返和人眼感知，因此不能作为真实端到端输入延迟。它只证明文本变化处理没有长时间同步阻塞调用方。

## 6. 内存状态

### 6.1 优化前历史观察

约 326 万记录的已安装服务曾测得：

| 指标 | 观察值 |
|---|---:|
| Working Set | 约 3589 MB |
| Private Bytes | 约 3662 MB |
| Virtual Size | 约 7832 MB |

主要问题是加载 snapshot 后的 `vector<FileRecord>` 没有被 rvalue `replace` 真正消费，完整路径记录、Catalog 和搜索加速器长期重复驻留。

### 6.2 当前真实 snapshot 的单索引结构

测试方法：2026-07-25，Release 构建；输入为 `C:\ProgramData\everything_sm\indexes\mft-index.snapshot`；记录数 3,264,188；完整进程独立运行 3 次。索引构建结束后读取 Working Set 和 Private Bytes，通过 `MetadataIndex::storage_stats()` 统计 vector capacity；另以 10 ms 间隔采样 snapshot 已载入到索引替换完成之间的进程峰值。

| 项目 | 观察值 |
|---|---:|
| 索引完成 Working Set | 437.88–437.90 MiB，中位数 437.89 MiB |
| 索引完成 Private Bytes | 436.29–436.33 MiB，中位数 436.32 MiB |
| 基础索引结构容量 | 约 429.41 MiB |
| CompactRecord 与稀疏父锚点 | 约 124.52 MiB |
| 字符 arena | 约 141.16 MiB |
| Bloom 签名及 owner | 约 66.75 MiB |
| 目录/异常路径签名 | 536,099 份 |
| 路径签名 owner 元数据 | 约 0.58 MiB |
| trigram posting | 约 72.01 MiB |
| posting entry 数 | 66,808,608 |
| posting 平均字节/entry | 约 1.13 B |
| 相对 `uint32_t` posting 的容量比例 | 约 0.28 |
| 排序/前缀结构 | 约 24.96 MiB |
| name-only 目录和文件路径 | 3,264,052 条 |

当前 `CompactRecord` 固定为 40 字节。正常父关系不再保存 64 位 `parent_id`，而是保存 31 位 `parent_index`；父记录缺失的 orphan/异常记录才进入稀疏 `ParentIdAnchor`。路径重建沿父记录索引 O(1)/层遍历，并在结果物化时恢复原始父 ID。记录容量由上一基线约 149.42 MiB 降到约 124.52 MiB。

完整路径 Bloom 仍按目录共享，但不再为每条记录保存 32 位 owner。目录 bitset 标记拥有签名的目录，rank prefix 把目录记录索引映射到签名索引；普通文件通过父目录记录推导 owner，只有 orphan 或异常完整路径进入稀疏 `PathSignatureFallback`。owner 元数据由约 12.45 MiB 降到约 0.58 MiB。含 `\`、`/`、`:` 的路径词仍跳过共享签名过滤并由完整 evaluator 校验，避免跨路径边界 false negative。

posting 构建继续使用两遍统计并直接把 delta/varint 写入最终 byte span，不临时保存约 255 MiB 的 `uint32_t posting_positions`。最终 posting 容量约 72.01 MiB，构建时间中位数约 21.972 秒。由于测试前仍先把完整 `vector<FileRecord>` snapshot 载入内存，构建阶段 10 ms 采样峰值仍为约 2279.45 MiB Working Set / 2299.08 MiB Private Bytes；这不是完整服务 reconciliation 峰值，也说明下一阶段仍需流式加载或直接从持久化紧凑结构构建。

本轮稳定 Private Bytes 从上一正式单索引基线约 473.04 MiB 降到约 436.32 MiB。`path:test1` 的 3 次 p50 中位数为 41.40 ms，纯名称查询约为 0.03–4.59 ms。最终 evaluator 仍负责正确性校验，因此 Bloom/hash 误报只增加候选，不改变结果。曾试验把名称 Bloom 从 128 位压到 64 位，`path:test1` 接近 90 ms，因此没有保留该方案。

### 6.3 完整服务与 Everything 对比

Everything 的同机样本为 1.4.1.1030：主进程约 312.78 MiB Private Bytes，辅助/服务进程约 3.99 MiB，合计约 316.77 MiB；其数据库约 146.20 MiB、总条目约 369.9 万。

当前最新代码的独立真实 snapshot 单索引 Private Bytes 中位数约 436.32 MiB，粗略为 Everything 合计值的 1.38 倍。ESM 约 326.4 万条、snapshot 约 966.69 MiB；Everything 约 369.9 万条、数据库约 146.20 MiB。两者不是相同记录集、文件格式或实现，不能直接当作严格同条件基准，也不能声称 ESM 已达到 Everything。

旧安装服务在 2026-07-25 07:46 启动、07:47 完成首次协调后曾稳定约 479 MiB Private Bytes；但后续每 30 分钟全量 reconciliation 会继续抬高堆提交量。08:49 完成第三次协调时，5 秒间隔采样观察到约 2501.88 MiB 构建中峰值，协调完成后连续 5 次稳定在约 953.48 MiB Private Bytes / 935.32 MiB Working Set。这一结果复现了用户看到的约 900 MiB，说明旧版的 479 MiB 不能表述为长期稳定值。

代码审查定位到多卷合并阶段逐卷 `vector<FileRecord>::insert` 未预留最终容量。对当前 3,264,195 条 snapshot 的诊断计数为 C: 2,031,861、D: 674,176、E: 558,158，`sizeof(FileRecord) == 128`；旧追加顺序在加入 D: 时把容量从 2,031,861 扩到 4,063,722，释放约 248.03 MiB 的旧 buffer，并使最终 vector 比实际记录多保留约 97.60 MiB 容量。修复版先收集各卷结果、计算总记录数并一次性 `reserve`，协调结束调用 `_heapmin` 归还完全空闲的 CRT heap region，再调用 `HeapCompact`。这是真正针对 Private Bytes 的堆回收，不使用 `EmptyWorkingSet` 或 `SetProcessWorkingSetSize(-1, -1)`。

2026-07-25 10:25 已通过 UAC 把最新 Release 服务安装到 `C:\Program Files\everything_sm\esm_service.exe`，安装源和目标 SHA-256 均为 `66DA638E502E33DE06D2F4CE93F1C37220369D2B121CC29F3C5F3C645077A72B`。Event Log 显示 10:25:25 从 3,264,387 条多卷 snapshot 启动，10:26:06 完成 C:/D:/E: 首次全量协调并得到 3,264,391 条；服务 PID 38636 在 10:27:16–10:28:11 的 12 次、5 秒间隔采样中，Working Set 为 441.24–441.29 MiB、中位数 441.27 MiB，Private Bytes 为 436.97–437.03 MiB、中位数 437.00 MiB。完整服务首轮稳定值粗略为同机 Everything 316.77 MiB 合计样本的 1.38 倍，但记录集和格式不同，不能视为严格同条件对比。

IPC 正确返回 `D:\test1\123456789.txt`。首次 `123456789.txt` 冷查询出现 1765.96 ms 服务端异常样本，紧接着 6 次重复查询为 0.333–0.449 ms；`windows` 查询为 5.678 ms。该结果说明热查询恢复到毫秒级，但仍需单独分析首次冷查询延迟。本次安装发生在首次协调开始前，没有同步采到协调过程峰值；第二次 30 分钟完整协调也尚未发生。因此目前可以确认“新服务首轮协调后未停留在 900 MiB”，但还不能确认长期多轮协调完全不再增长。

后续在 18:12 的长期复查推翻了首轮稳定假设：PID 38636 在 18:04:41 又完成一次固定周期 reconciliation 后为 901.44 MiB Private Bytes / 886.39 MiB Working Set，进程历史峰值为 3426.80 MiB Private Bytes / 3372.33 MiB Working Set。15:31、16:02、16:32、17:03、17:34、18:04 均有完整协调事件，说明增长与无条件全量替换直接相关。精确 reserve 只消除了总 vector 扩容，无法消除数百万 `std::wstring` 路径分配以及新旧完整搜索索引重叠后造成的 CRT heap 保留。

最新策略取消固定 30 分钟完整替换，稳定期只读取 USN 增量并每分钟轻量发现挂载卷；完整 MFT repair 仅在 snapshot 启动、USN checkpoint 失效/读取失败或卷集合变化时触发。这样避免健康服务反复制造多 GiB 构建峰值和约一代索引的堆保留，同时保留 journal gap 和新卷恢复路径。该策略的 Release 服务 SHA-256 为 `B808B460467F32BC0567EBD9A2EB28853EABF05B71FD570012E911D0C56CCBB1`，已于 18:20 通过 UAC 安装。新 PID 33288 在 18:21:36 首次 repair 后为约 436.50 MiB Private Bytes / 439.54 MiB Working Set；`123456789.txt`、`windows`、`path:test1` 服务端查询分别约 0.922 ms、7.061 ms、4.089 ms。尚需在 18:51 之后确认没有固定周期 reconciliation，并继续采样稳定内存。

没有保留 `EmptyWorkingSet`/`SetProcessWorkingSetSize(-1, -1)` 方案：它只能改变任务管理器 Working Set，并会增加冷页缺页和首次宽泛查询延迟。当前只在大阶段结束后调用 `_heapmin` 归还完全空闲的 CRT heap region，并调用 `HeapCompact` 整理进程堆。

## 7. 已知慢路径

- `path:` 查询无法只依靠名称 gram，通常比纯名称查询慢；
- 宽泛正则和复杂 OR 可能无法提取 mandatory gram；
- 带 `*`/`?` 的 `filelist:` 目前不使用精确 basename 候选扫描；
- 单字符/双字符查询候选集合天然较大；
- duplicate mode 需要额外分组；
- 首次 Shell 图标或文件元数据读取会触发额外系统调用；
- 首次建库、完整协调和 snapshot 写入会竞争 CPU、内存带宽和磁盘；
- GUI 显示大量列、预览或图标时会增加 UI 工作。

## 8. 基准规范

提交性能数据时必须：

- 使用 Release 构建；
- 记录 commit、机器、CPU、内存、磁盘、记录数和 snapshot 路径；
- 固定查询、排序、case/whole/path/diacritic flags 和 limit；
- 分开 cold start、warm query 和索引构建；
- 至少报告样本数与 p50，重要回归报告 p95/最大值；
- 区分索引内时间、服务端时间、IPC 往返和 GUI 完成时间；
- 性能优化同时运行正确性测试；
- 把新数据写入本文件或新增带日期 baseline。

历史早期数据见 [BASELINE_2026-07-23.md](BASELINE_2026-07-23.md)。

## 8. 下一步

- 安装 USN 驱动修复版并连续运行超过原 30 分钟周期，确认没有新的无条件 reconciliation 事件且 Private Bytes 保持接近首轮基线；
- 将 snapshot 直接流式读入紧凑索引，降低当前约 2.30 GiB 的单进程构建峰值；
- 减少完整路径重复和 UTF-16 字符串对象开销；
- 将名称索引直接持久化或并行/增量构建；
- 对 `path:` 建立路径组件/gram 索引；
- 增量更新 posting，而不是大目录变化后重建；
- 更快或并行的 snapshot checksum；
- 建立包含真实键盘、IPC、列表 paint 和 metadata hydration 的 UI 端到端基准；
- 增加长期内存和后台协调回归门槛。

## 9. 内容搜索原型的真实性能基线（2026-07-26）

### 9.1 测试方法

本次使用 Release MinGW64 构建和仓库内 Xapian Core 1.4.31。内容服务命令行为：

```text
esm_content_service.exe --all-fixed \
  --db-root D:\everything-sm-content-full-20260726-154519 \
  --pipe everything_sm_content_full_20260726-154519 --max-mib 4
```

测试时数据库约 4,968,235,452 字节（4.63 GiB），status 在 264,692～264,693 个文档之间，`ready=1`、`indexing=1`；即服务正在后台重新扫描和提交，不是静止数据库。使用 `esm_content_cli.exe` 顺序发起真实 Named Pipe 查询，计时包含 CLI 进程启动、Pipe 往返、Xapian 查询、读取返回文档 data、为每个命中生成摘要以及响应编码，但不包含实验 GUI 绘制。当前 document data 保存路径和最多 4 MiB 的完整正文。

修复前的第一次连续输入测试中，`x` 有效完成约 2718.4 ms，随后服务因 `Xapian::DatabaseModifiedError` 终止；后续约 5 秒是 Pipe 超时，**不是有效查询延迟**。修复后 commit 与只读查询快照协调，并增加 Xapian/非标准异常边界；同一数据库完成下列查询后服务 PID 仍存活。

### 9.2 连续前缀和常见词，`limit=100`

下表每个查询只有一次顺序样本，用来模拟连续输入后每次请求均实际执行的最坏产品路径；它不是统计意义上的 p50/p95。

| 查询 | IPC 往返 | estimated | returned |
|---|---:|---:|---:|
| `x` | 1753.5 ms | 34,348 | 100 |
| `xa` | 3378.7 ms | 591 | 100 |
| `xap` | 4713.3 ms | 43 | 43 |
| `xapi` | 1599.2 ms | 2 | 2 |
| `xapian` | 671.9 ms | 3,891 | 100 |
| `你` | 1855.9 ms | 1,075 | 100 |
| `你好` | 5807.3 ms | 87 | 87 |
| `database` | 1815.4 ms | 5,352 | 100 |
| `error` | 1482.2 ms | 68,084 | 100 |
| `TODO` | 1180.6 ms | 14,193 | 100 |

十个不同查询的范围为 671.9～5807.3 ms。由于查询集合异质、每项只有一个样本，不能把其中位数或最大值包装为发布级延迟分布。结果已经足够说明当前内容查询不具备 Everything 文件名搜索那种连续输入即时响应。

### 9.3 返回数量敏感性

以下每个组合重复 2 次；服务仍处于 `indexing=1`：

| 查询 | limit | 两次 IPC 往返 |
|---|---:|---:|
| `x` | 10 | 301.1 / 372.1 ms |
| `x` | 50 | 1079.0 / 1157.2 ms |
| `x` | 100 | 1548.8 / 1629.6 ms |
| `xapian` | 10 | 237.8 / 303.3 ms |
| `xapian` | 50 | 412.0 / 458.7 ms |
| `xapian` | 100 | 614.8 / 605.5 ms |
| `你好` | 10 | 756.9 / 594.9 ms |
| `你好` | 50 | 1009.6 / 893.8 ms |
| `你好` | 100 | 4705.0 / 4712.9 ms |

返回数量增加时延迟显著上升；`你好` 从 50 增至全部约 88 条时出现约 4.7 秒慢路径。结合当前实现可判断，主要成本不只是倒排匹配，还包括为每个命中读取包含完整正文的 document data，并调用 `MSet::snippet()` 扫描正文生成摘要。这是基于实现和 limit 对照结果的工程判断，后续仍需增加服务内部阶段计时来精确拆分。

### 9.4 当前结论和下一步

- 当前“匹配正确性和并发存活”已有真实数据库验证，但“匹配速度快”不成立；
- UI 的 180 ms debounce 和 generation 丢弃只能避免显示过期响应，不能取消服务端已经开始的旧查询；连续输入仍会浪费 CPU、磁盘和 Pipe worker；
- 下一步优先增加请求 generation/cancellation、单字符策略、查询阶段计时、先返回路径/相关度再异步生成少量可见行摘要，以及避免每个结果读取整段正文；
- 再评估持久只读数据库句柄、并行 shard 查询和正文压缩 sidecar；
- 完成优化后需在 `indexing=0` 与 `indexing=1` 两种状态分别报告 cold/warm p50、p95、最大值和真实 GUI 完成时间。

## 10. 目录直接子项查询真实服务观察（2026-07-28）

### 10.1 测试方法

本次使用 Release MinGW/UCRT 构建和 NSIS 安装版，服务通过 `everything_sm_service` Named Pipe 提供查询。测试基于当前真实多卷索引；历史基线约 326 万条记录。每次使用 `esm_cli.exe` 发起 `limit=5` 的真实 IPC 查询，计时包含客户端进程、Named Pipe 往返以及服务端按需构建目录直接子项计数，不包含 GUI 列表绘制。

目录子项函数目前不会为普通名称、路径、大小或日期查询增加常驻计数数组。只有查询包含 `child:`、`empty:`、`childcount:`、`childfilecount:` 或 `childfoldercount:` 时，服务才遍历 base 与实时 overlay 的父关系并生成临时命中/统计结构。因此本节数据反映当前 O(N) 慢路径，不能外推为普通名称查询性能；包含 T 个不同 `child:` 条件时还需要对每个直接子名称执行最多 T 次匹配，当前上界为 O(N×T)。

### 10.2 单次功能验证样本

| 查询场景 | 观察值 |
|---|---:|
| 新建空目录后 `name:esm-empty-probe-20260728 empty:` | 约 317～444 ms |
| 新建一个直接子文件后 `name:esm-empty-probe-20260728 childcount:=1 childfilecount:=1 childfoldercount:=0` | 约 452 ms |
| 删除直接子文件后再次查询 `name:esm-empty-probe-20260728 empty:` | 约 472 ms |
| 安装版实时创建子文件后 `name:esm-child-probe-… child:needle-child-….txt` | 546.986 ms |
| 删除该子文件后同一 `child:` 查询清空 | 567.048 ms |

这些数据来自少量顺序功能验证样本，不是 p50、p95 或发布级基准。USN 传播时机、服务冷热状态和后台协调均可能影响结果。当前实现验证了 base 与 overlay 创建/删除的正确性，但尚未达到 Everything 同类目录子名称/计数函数的响应水平。两条 `child:` 数据同样各只有一次真实安装服务功能样本，只能证明实时创建/删除语义和当前量级，不能作为延迟分布。

### 10.3 安装版进程内存观察

包含 `child:` 的最新 NSIS 安装包大小为 2,538,445 字节，SHA-256 为 `CE748F2A783C8D4DB6657C14441E2CFF36A0C9B6F478E4D16C9338FBA4E055F3`；静默提权安装退出码为 0，构建版与 `C:\Program Files\everything_sm\esm_service.exe` 哈希一致。安装后的 PID 27928 稳定样本为 466,415,616 字节 Working Set、462,663,680 字节 Private Bytes。本次安装没有高频采样启动峰值；前一安装服务启动阶段曾观察到约 1.64 GiB Working Set / Private Bytes。

稳定样本不能替代启动峰值。更早的首次重建流程还观察过约 2.7 GiB 峰值，因此当前仍需优化 snapshot 载入、repair/reconciliation 与索引替换期间的临时结构，不能声称内存已达到 Everything。

## 11. 初始 NTFS 元数据补齐真实路径样本（2026-07-28）

### 11.1 测试方法

从本机 `C:\ProgramData\everything_sm\indexes\mft-index.snapshot` 读取 3,300,421 条真实记录，用临时 `metadata_probe.exe` 把样本记录的大小和修改时间清零，再调用本轮新增的 `hydrate_file_search_metadata_records` 对记录中的真实绝对路径执行 `GetFileAttributesExW`。下列耗时只覆盖元数据 API 阶段，不包含 snapshot 载入、MFT 枚举、路径重建、`MetadataIndex` 构建、snapshot 保存、IPC 或 GUI；文件系统缓存状态未严格控制，因此不能视为首次整机建库 SLA。

### 11.2 全量样本

4 个 worker 的结果：

```text
attempted=3,300,421
hydrated=3,293,996
errors=6,425
elapsed=75,682 ms
throughput=43,609 records/s
```

失败项包括无法访问、枚举后瞬时消失和不能由当前重建路径读取的条目；实现保留这些记录原值并继续发布名称索引。

### 11.3 10 万条 worker 对照

同一机器后续缓存较暖的 100,001 条样本：

| worker | 阶段耗时 | 观察吞吐 |
|---:|---:|---:|
| 1 | 4,516 ms | 22,143 records/s |
| 2 | 2,695 ms | 37,106 records/s |
| 4 | 1,679 ms | 59,559 records/s |
| 8 | 1,083 ms | 92,337 records/s |
| 12 | 1,299 ms | 76,983 records/s |

首次相对冷的同规模 4-worker 样本为 11,112 ms / 8,999 records/s。后续对照明显受到缓存变暖影响，只用于观察并行扩展趋势，不能与冷启动直接横向比较。默认值保持每卷 4 个 worker，避免多卷同时建库时线程数和随机元数据读取负载失控。


### 11.4 两阶段服务验证方法与边界

默认多卷服务现在把“可查询时间”和“元数据完成时间”拆开：名称基线进入 `MetadataIndex` 后即可接受 Named Pipe 查询，大小/修改时间随后按 4,096 条检查批次、默认最多 4 个 worker 补齐，活动批次之间约等待 10 ms。实现验证使用 Release 构建的 `esm_tests`，覆盖 ID 顺序分页、removed/overlay 跳过、元数据合并，以及 rename/delete 后旧路径结果被计为 stale 且不能覆盖新记录；这属于正确性测试，不是端到端性能基准。

本轮尚未在清空 snapshot 的真实全新安装上记录“服务开始”到“名称索引发布”和“后台元数据完成”两个墙钟时间，因此不提供新的首次建库数值，也不声称元数据瞬间完整。后续真实测试应固定卷集合、文件数、机器、电源方案和缓存条件，并分别记录：

1. SCM 启动到 Event Log 出现 `name index published` 的时间；
2. 同期 Named Pipe 名称查询 p50/p95；
3. `Background metadata hydration completed` 的总耗时、`hydrated/errors/stale`；
4. 补齐期间 `size:` / `dm:` 结果收敛过程、USN 延迟、停止延迟、CPU、磁盘队列和 Private Bytes 峰值。

原 3,300,421 条、75,682 ms 数据只代表独立文件元数据 API 阶段，可用于估计后台工作量，不能直接当作新服务流程的首次可查询时间或完整端到端耗时。后台补齐只保留有限批次路径，预期避免第二份全盘路径向量；该内存结论仍需在真实全新建库和 reconciliation 中采样验证。

### 11.5 Metadata WAL 正确性验证与性能边界

本轮 Release 单元测试覆盖 generation 初始化、事务追加/replay、大小/时间/属性恢复、durable `next_id`/完成状态、缺失 ID、路径 fingerprint stale、generation mismatch、事务尾部撕裂截断及再次 replay。固定 update 编码约 40 字节；单个 4,096 条批次在全部成功时约产生 160 KiB update payload，另有事务头/尾。每批使用 write-through append 并调用 `FlushFileBuffers`，因此持久性会引入额外磁盘 flush 延迟。

这些只是 Release 正确性测试和格式规模估算，不是整机端到端性能基准。本轮尚未测量真实 SCM 重启恢复时间、每批 flush 开销、补齐期间 USN 延迟、磁盘队列、冷缓存吞吐或长期 WAL 增长，也没有建立新的启动/查询 SLA。后续基准必须分别报告 snapshot load、WAL replay、USN catch-up、metadata hydration 和 GUI/IPC 时间，不能把单元测试或合成 payload 大小夸大为真实用户体验。
### 11.6 跨 generation 元数据复用的测试方法与边界

完整 reconciliation 只在旧 live USN 状态连续可信时执行复用：新 MFT 扫描完成后，先把新旧共有卷的旧 live 状态追赶到当前 journal 边界，覆盖扫描前后尚未轮询的原地内容变化；再把新 MFT 记录原地按 ID 排序，并在当前 `MetadataIndex` 的共享锁下按 ID 线性前进。base 记录通过完整路径再次校验，overlay 优先于 base，removed、路径不一致和修改时间为 0 的来源不会复用。启动 state 失效、扫描后预追赶失败、普通 USN 读取失败或 journal gap 会将整代标记为 `metadata reuse=disabled`，因为单靠 ID/路径无法识别漏记的原地内容修改。成功项只复制大小、修改时间、属性和目录标志，新 snapshot 随后直接保存这些值，不创建第二份全盘路径向量。后台 `metadata_hydration_batch()` 会跳过修改时间已知的 base 项；稀疏情况下单批最多检查返回上限的 64 倍，但文件 I/O 候选仍最多 4,096 条，以减少只推进 cursor 的空 WAL 事务和 `FlushFileBuffers` 次数。

本轮验证方法仅为 `build-ucrt-vendor-final` Release 正确性测试：构造无序新基线，覆盖 matching base、matching overlay、路径变化、removed、来源未知和新 ID，检查排序、复用统计及未知项保持未修改；同时验证后台批次跳过已知 base 元数据，并验证直接 USN 变化后的文件元数据读取失败会把旧大小/修改时间置为未知。该测试没有使用真实百万级 MFT、SCM 服务、磁盘 flush 或并发 USN 压力，因此不能声称 reconciliation 时间、启动时间、Private Bytes 或磁盘写入已经改善到某个实测数值。后续真实基准应记录 `reused metadata/unknown/stale`、原地排序与线性核对时间、新 snapshot 保存时间、WAL 事务数、补齐文件 I/O 数、USN catch-up 延迟和峰值内存，并与未复用版本在同一 snapshot/卷集合上对照。


## 2026-07-29：真实 reconciliation 基准入口

Release 目标 `esm_reconcile_benchmark` 用于观察默认多卷 NTFS 完整 reconciliation 的真实阶段，而不是生成合成百万记录。它依次记录：

1. 固定 NTFS 卷发现；
2. 每卷 `FSCTL_ENUM_USN_DATA` MFT/USN 枚举（不包含后续 size/time hydration）；
3. 多卷合并、一次性 reserve 和卷级 ID namespacing；
4. `MetadataIndex` base 构建；
5. `snapshot_records()` checkpoint 物化；
6. 原子 snapshot 写入；
7. 后台每 10 ms 采样 Working Set 和 Private Bytes。

输出文件包含阶段耗时、每卷记录数、峰值内存和 `million_scale=1/0`。该工具需要能够读取卷 MFT 的管理员权限；非提升进程得到 `records=0` 时只能证明权限路径被拒绝，不能作为性能数据。

验证命令：

```powershell
cmake --build build-ucrt-vendor-final --config Release   --target esm_service esm_tests esm_reconcile_benchmark -j 4
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
ctest --test-dir build-ucrt-vendor-final -C Release --output-on-failure
```

2026-07-29 的非提升 Release 运行完成了构建和测试，但真实 MFT 阶段返回 `records=0`，因此没有发布任何 reconciliation 时间或峰值内存结论。后续需要在明确提升的同一机器上运行，并同时记录文件名 GUI、Named Pipe、USN catch-up 和 metadata hydration；不能把 checkpoint 单元测试、合成记录或无权限样本夸大为 Everything 级端到端结果。

## 12. 内容 GUI 连续输入与文档提取功能检查（2026-07-29）

### 12.1 测试方法

- 使用 `build-ucrt-vendor-final` Release 构建；
- 创建唯一测试 Pipe 和只包含 `welcome.txt`、`word-support.docx`、`pdf-support.pdf` 的临时根；
- DOCX/PDF 样本包含各自唯一词，通过真实 `esm_content_service.exe`、Named Pipe 和 `esm_content_cli.exe` 查询；
- 使用 Windows UI 自动化向内容 GUI 输入查询，检查搜索框、清空/打开/打开目录按钮、结果列、状态灯、状态文字和结果计数；
- 连续输入路径使用 160 ms debounce、单一长期 IPC worker 和 generation 丢弃过期结果。

### 12.2 观察结果与边界

服务完成后报告 `documents=3 ready=1 indexing=0`；DOCX 和 PDF 唯一词分别返回对应文件。本机 `LoadIFilter` 对 DOCX 返回 class-not-registered、对 PDF 返回 `E_NOINTERFACE`，因此该样本验证的是内置 DOCX/PDF 回退，而不是第三方 IFilter。UI 自动化确认 TXT 查询可显示结果、完成状态和结果计数。

这是一组 3 文档的小样本功能与响应性检查，没有采集稳定的端到端延迟分布，也没有覆盖整机扫描、复杂 PDF、Office 大文档、OCR、冷缓存、后台高负载或真实键盘/IME 的 p50/p95。长期 worker 主要消除连续输入时反复创建 detached thread 和过期结果回写；它不代表 Xapian 查询本身变快，更不能据此声称达到 Everything 的即时搜索性能。


## 13. 文件名 GUI 输入与元数据交接基线（2026-07-29）

### 13.1 测试环境与方法

- 构建：`build-ucrt-vendor-final`，`Release`，MinGW/UCRT64；
- 机器：Intel Core i7-10750H，6 核 12 线程，物理内存约 15.8 GiB；
- 真实索引：`C:\ProgramData\everything_sm\indexes\mft-index.snapshot`，3,300,421 条记录；
- 进程内真实 snapshot 基准：`esm_real_search_benchmark.exe`，每个查询预热后运行 9 次并报告 p50/p95，`limit=1000`；
- GUI 数据交接微基准：`esm_gui_pipeline_benchmark.exe <result-count>`，分别运行 21 次，比较旧的完整 `vector<SearchResult>` 深复制与新的“结果槽位 + 路径”请求构建；内存数字由容器容量、对象大小和字符串 capacity 估算，不是 Working Set/Private Bytes；
- 本节包含一次 Windows UI 自动化功能检查，但没有自动化真实键盘/IME、稳定窗口绘制、Shell 图标或 Named Pipe 端到端延迟分布，因此不能据此声称达到 Everything 的端到端体感或性能。

### 13.2 真实 330 万记录进程内基线

本轮未修改核心倒排索引；该数据用于给 GUI 优化提供服务端量级背景：

| 阶段/查询 | 结果 |
|---|---:|
| snapshot load | 3172 ms |
| MetadataIndex build | 18053 ms |
| build peak Private Bytes | 2320.20 MiB |
| index ready Private Bytes | 440.95 MiB |
| `1` p50 / p95 | 2.92 / 3.46 ms |
| `txt` p50 / p95 | 1.56 / 2.30 ms |
| `123456789.txt` p50 / p95 | 0.04 / 0.12 ms |
| `path:test1` p50 / p95 | 46.70 / 65.33 ms |

这些是同进程查询时间，不包含 Named Pipe 序列化、GUI 线程调度、ListView 失效重绘和图标加载。`path:` 仍是明显慢路径。

### 13.3 GUI 元数据交接合成微基准

| 结果数 | 旧完整深复制 p50 / p95 | 新轻量请求构建 p50 / p95 | 旧交接估算 | 新请求估算 | 数值更新 payload 估算 |
|---:|---:|---:|---:|---:|---:|
| 1,000 | 0.39 / 0.54 ms | 0.21 / 0.36 ms | 0.38 MiB | 0.22 MiB | 0.05 MiB |
| 100,000 | 38.31 / 47.73 ms | 22.92 / 26.17 ms | 39.22 MiB | 22.27 MiB | 5.34 MiB |

100,000 条样本的 21 次范围：旧深复制 30.98–49.16 ms，新轻量请求构建 17.76–27.30 ms。新的运行时峰值还会同时包含请求与数值更新数组，不能简单把 22.27 MiB 当作完整峰值；但窗口线程不再复制名称字符串，后台完成后也只向窗口传回固定大小数值更新，不再把完整结果向量往返一次。

### 13.4 GUI 功能自动化观察

2026-07-29 使用工作区 `esm_gui.exe` 连接本机已运行服务，并通过 Windows UI 自动化注入查询、等待后读取可访问性树：

- `123456789.txt` 返回 2 条；`D:\test1\123456789.txt` 显示 0 B 和 `2026-07-24 15:47`，Recent 快捷方式显示 565 B 和同一分钟修改时间；状态为 `2 个结果，13.92 ms`，没有 `+`，验证少于 200 条的首屏被直接视为完整并可启动缺失元数据补齐。
- 宽查询 `1` 在输入后约 180 ms 的观察点显示 `200+ 个结果，104.02 ms`，再等待约 1.2 秒后显示 `1000 个结果，105.83 ms`，验证仅满 200 条首屏进入最终 refinement。
- 自动化输入 `path:program files ext:exe` 的调用本身约 33.6 ms，查询完成时搜索框仍保持焦点；该次状态栏为 `4 个结果，435.74 ms`，再次说明 `path:`/复杂组合仍是慢路径。另一次单字符输入调用约 40.9 ms。

状态栏中的毫秒值来自服务响应，不等于从物理按键到首帧绘制的端到端延迟；自动化调用耗时也包含工具与输入注入开销。以上均为单次功能观察，不是 p50/p95，不覆盖真实键盘/IME、冷缓存或后台高负载。

### 13.5 行为优化和剩余瓶颈

- 首个输入仍使用 15 ms 防抖；150 ms 内的后续输入使用 60 ms 防抖，以合并快速输入突发。少于 200 条的首屏直接视为完整；只有刚好达到 200 条且最终上限更大时才显示 `+` 并安排 refinement。
- 客户端仍保持最多一个同步 Named Pipe 查询在途，不会在每个按键上调用 `CancelSynchronousIo`；这避免取消风暴占满服务端 worker，但一个已经开始的慢查询仍可能增加最新输入的尾部延迟。
- 默认名称、路径、大小、修改时间和类型视图不再触发最终结果的无条件完整文件系统 hydration；只为修改时间仍未知的结果构建轻量请求。按创建时间、访问时间或 NTFS Change 时间排序时才读取全部结果路径，并最多使用 4 个 worker。
- 下一步仍需要可取消的服务端 query generation、Named Pipe/反序列化/列表重绘分阶段计时，以及真实 GUI 连续输入 p50/p95。当前优化不能被描述为 Everything 100% 性能兼容。
