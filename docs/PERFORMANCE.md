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

## 4. GUI 合成连续变化测试

测试通过跨进程 `SetWindowText` 连续改变 ComboBox 文本，共 62 次：

- 平均 UI 调用延迟：0.25 ms；
- P50：0.18 ms；
- P95：0.44 ms；
- 最大：3.75 ms；
- 测试期间窗口保持可响应。

**限制：** 这是合成消息调度测试，不包含真实键盘输入、IME、显示器刷新、Shell 图标、完整 IPC 往返和人眼感知，因此不能作为真实端到端输入延迟。它只证明文本变化处理没有长时间同步阻塞调用方。

## 5. 内存状态

### 5.1 优化前历史观察

约 326 万记录的已安装服务曾测得：

| 指标 | 观察值 |
|---|---:|
| Working Set | 约 3589 MB |
| Private Bytes | 约 3662 MB |
| Virtual Size | 约 7832 MB |

主要问题是加载 snapshot 后的 `vector<FileRecord>` 没有被 rvalue `replace` 真正消费，完整路径记录、Catalog 和搜索加速器长期重复驻留。

### 5.2 当前真实 snapshot 的单索引结构

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

### 5.3 完整服务与 Everything 对比

Everything 的同机样本为 1.4.1.1030：主进程约 312.78 MiB Private Bytes，辅助/服务进程约 3.99 MiB，合计约 316.77 MiB；其数据库约 146.20 MiB、总条目约 369.9 万。

当前最新代码的独立真实 snapshot 单索引 Private Bytes 中位数约 436.32 MiB，粗略为 Everything 合计值的 1.38 倍。ESM 约 326.4 万条、snapshot 约 966.69 MiB；Everything 约 369.9 万条、数据库约 146.20 MiB。两者不是相同记录集、文件格式或实现，不能直接当作严格同条件基准，也不能声称 ESM 已达到 Everything。

旧安装服务在 2026-07-25 07:46 启动、07:47 完成首次协调后曾稳定约 479 MiB Private Bytes；但后续每 30 分钟全量 reconciliation 会继续抬高堆提交量。08:49 完成第三次协调时，5 秒间隔采样观察到约 2501.88 MiB 构建中峰值，协调完成后连续 5 次稳定在约 953.48 MiB Private Bytes / 935.32 MiB Working Set。这一结果复现了用户看到的约 900 MiB，说明旧版的 479 MiB 不能表述为长期稳定值。

代码审查定位到多卷合并阶段逐卷 `vector<FileRecord>::insert` 未预留最终容量。对当前 3,264,195 条 snapshot 的诊断计数为 C: 2,031,861、D: 674,176、E: 558,158，`sizeof(FileRecord) == 128`；旧追加顺序在加入 D: 时把容量从 2,031,861 扩到 4,063,722，释放约 248.03 MiB 的旧 buffer，并使最终 vector 比实际记录多保留约 97.60 MiB 容量。修复版先收集各卷结果、计算总记录数并一次性 `reserve`，协调结束调用 `_heapmin` 归还完全空闲的 CRT heap region，再调用 `HeapCompact`。这是真正针对 Private Bytes 的堆回收，不使用 `EmptyWorkingSet` 或 `SetProcessWorkingSetSize(-1, -1)`。

2026-07-25 10:25 已通过 UAC 把最新 Release 服务安装到 `C:\Program Files\everything_sm\esm_service.exe`，安装源和目标 SHA-256 均为 `66DA638E502E33DE06D2F4CE93F1C37220369D2B121CC29F3C5F3C645077A72B`。Event Log 显示 10:25:25 从 3,264,387 条多卷 snapshot 启动，10:26:06 完成 C:/D:/E: 首次全量协调并得到 3,264,391 条；服务 PID 38636 在 10:27:16–10:28:11 的 12 次、5 秒间隔采样中，Working Set 为 441.24–441.29 MiB、中位数 441.27 MiB，Private Bytes 为 436.97–437.03 MiB、中位数 437.00 MiB。完整服务首轮稳定值粗略为同机 Everything 316.77 MiB 合计样本的 1.38 倍，但记录集和格式不同，不能视为严格同条件对比。

IPC 正确返回 `D:\test1\123456789.txt`。首次 `123456789.txt` 冷查询出现 1765.96 ms 服务端异常样本，紧接着 6 次重复查询为 0.333–0.449 ms；`windows` 查询为 5.678 ms。该结果说明热查询恢复到毫秒级，但仍需单独分析首次冷查询延迟。本次安装发生在首次协调开始前，没有同步采到协调过程峰值；第二次 30 分钟完整协调也尚未发生。因此目前可以确认“新服务首轮协调后未停留在 900 MiB”，但还不能确认长期多轮协调完全不再增长。

后续在 18:12 的长期复查推翻了首轮稳定假设：PID 38636 在 18:04:41 又完成一次固定周期 reconciliation 后为 901.44 MiB Private Bytes / 886.39 MiB Working Set，进程历史峰值为 3426.80 MiB Private Bytes / 3372.33 MiB Working Set。15:31、16:02、16:32、17:03、17:34、18:04 均有完整协调事件，说明增长与无条件全量替换直接相关。精确 reserve 只消除了总 vector 扩容，无法消除数百万 `std::wstring` 路径分配以及新旧完整搜索索引重叠后造成的 CRT heap 保留。

最新策略取消固定 30 分钟完整替换，稳定期只读取 USN 增量并每分钟轻量发现挂载卷；完整 MFT repair 仅在 snapshot 启动、USN checkpoint 失效/读取失败或卷集合变化时触发。这样避免健康服务反复制造多 GiB 构建峰值和约一代索引的堆保留，同时保留 journal gap 和新卷恢复路径。该策略的 Release 服务 SHA-256 为 `B808B460467F32BC0567EBD9A2EB28853EABF05B71FD570012E911D0C56CCBB1`，已于 18:20 通过 UAC 安装。新 PID 33288 在 18:21:36 首次 repair 后为约 436.50 MiB Private Bytes / 439.54 MiB Working Set；`123456789.txt`、`windows`、`path:test1` 服务端查询分别约 0.922 ms、7.061 ms、4.089 ms。尚需在 18:51 之后确认没有固定周期 reconciliation，并继续采样稳定内存。

没有保留 `EmptyWorkingSet`/`SetProcessWorkingSetSize(-1, -1)` 方案：它只能改变任务管理器 Working Set，并会增加冷页缺页和首次宽泛查询延迟。当前只在大阶段结束后调用 `_heapmin` 归还完全空闲的 CRT heap region，并调用 `HeapCompact` 整理进程堆。

## 6. 已知慢路径

- `path:` 查询无法只依靠名称 gram，通常比纯名称查询慢；
- 宽泛正则和复杂 OR 可能无法提取 mandatory gram；
- 单字符/双字符查询候选集合天然较大；
- duplicate mode 需要额外分组；
- 首次 Shell 图标或文件元数据读取会触发额外系统调用；
- 首次建库、完整协调和 snapshot 写入会竞争 CPU、内存带宽和磁盘；
- GUI 显示大量列、预览或图标时会增加 UI 工作。

## 7. 基准规范

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
