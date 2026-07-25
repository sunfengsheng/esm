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

- 等待并采样第二次及后续 30 分钟完整协调，确认 Private Bytes 不会再次从约 437 MiB 增长到约 900 MiB；
- 将 snapshot 直接流式读入紧凑索引，降低当前约 2.30 GiB 的单进程构建峰值；
- 减少完整路径重复和 UTF-16 字符串对象开销；
- 将名称索引直接持久化或并行/增量构建；
- 对 `path:` 建立路径组件/gram 索引；
- 增量更新 posting，而不是大目录变化后重建；
- 更快或并行的 snapshot checksum；
- 建立包含真实键盘、IPC、列表 paint 和 metadata hydration 的 UI 端到端基准；
- 增加长期内存和后台协调回归门槛。
