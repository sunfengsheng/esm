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

## 3. 2026-07-24 开发基线

数据源：

```text
C:\ProgramData\everything_sm\indexes\mft-index.snapshot
record_count = 3,264,059
sort = name natural order
limit = 1000
```

### 索引内查询 p50

测试使用 Release 构建，同一真实 snapshot 连续独立运行 3 次；每次重新载入 snapshot、重建索引，每个查询运行 9 次。下表取 3 次运行各自 p50 的中位数，不是 GUI 端到端延迟。

| 查询 | p50 |
|---|---:|
| `1` | 1.88 ms |
| `12` | 5.61 ms |
| `123` | 2.32 ms |
| `txt` | 1.43 ms |
| `windows` | 1.63 ms |
| `report` | 4.01 ms |
| `123456789` | 3.23 ms |
| `123456789.txt` | 0.17 ms |
| `path:test1` | 31.04 ms |

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

该 IPC 表仍是上一轮同机已安装版本基线，不能与本轮单进程索引数据混为一次端到端测试。

### 优化前后

| 查询 | 优化前 | 优化后服务 p50 |
|---|---:|---:|
| `report` | 约 276 ms | 约 3 ms |
| `123456789.txt` | 约 466 ms | 约 0.17 ms |

本轮最终代码的 3 次真实索引构建耗时为 22.102–22.548 秒，中位数约 22.528 秒。`path:test1` 因路径签名改为目录共享而回退到 31.04 ms；这是用约 70 MiB 稳定内存换取的已知路径查询权衡，不能隐瞒或与纯名称查询混为一谈。

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

测试方法：2026-07-24，Release 构建；输入为 `C:\ProgramData\everything_sm\indexes\mft-index.snapshot`；记录数 3,264,059；完整进程独立运行 3 次。索引构建结束后读取 Working Set 和 Private Bytes，通过 `MetadataIndex::storage_stats()` 统计 vector capacity；另以 10 ms 间隔采样 snapshot 已载入到索引替换完成之间的进程峰值。

| 项目 | 观察值 |
|---|---:|
| 索引完成 Working Set | 557.62–557.71 MiB，中位数 557.69 MiB |
| 索引完成 Private Bytes | 556.20–556.30 MiB，中位数 556.27 MiB |
| 基础索引结构容量 | 约 548.55 MiB |
| CompactRecord | 约 149.42 MiB |
| 字符 arena | 约 223.56 MiB |
| Bloom 签名及 owner | 约 78.62 MiB |
| 目录路径签名 | 536,073 份 |
| 每记录路径签名 owner | 约 12.45 MiB |
| trigram posting | 约 72.00 MiB |
| posting entry 数 | 66,805,057 |
| posting 平均字节/entry | 约 1.13 B |
| 相对 `uint32_t` posting 的容量比例 | 约 0.28 |
| 排序/前缀结构 | 约 24.96 MiB |
| name-only 子文件路径 | 2,727,986 条 |

第二阶段不再为每条记录保存一份 256 位完整路径 Bloom。目录保存共享的 256 位完整目录路径签名，每条记录只保存 32 位 owner；文件名部分复用已有的 128 位名称签名。含 `\`、`/`、`:` 的路径词不使用该共享过滤，直接回退完整 evaluator，避免跨路径边界产生 false negative。相对第一阶段，签名容量从约 149.41 MiB 降到约 78.62 MiB，单索引 Working Set 从约 628.79 MiB 降到约 557.69 MiB。

posting 构建也改为两遍统计并直接把 delta/varint 写入最终 byte span，不再临时保存约 255 MiB 的 `uint32_t posting_positions`。最终 posting 容量仍约 72.00 MiB，构建时间中位数从约 27.041 秒降到约 22.528 秒。由于测试前仍先把完整 `vector<FileRecord>` snapshot 载入内存，构建阶段 10 ms 采样峰值仍为约 2154.9 MiB Working Set / 2174.1 MiB Private Bytes；这不是完整服务 reconciliation 峰值，也说明下一阶段仍需流式加载或直接从持久化紧凑结构构建。

共享路径签名节省约 70 MiB，但 `path:test1` 的 3 次 p50 中位数从上一阶段 17.84 ms 增至 31.04 ms。纯名称查询仍大致为 1–6 ms。最终 evaluator 仍负责正确性校验，因此 Bloom/hash 误报只增加候选，不改变结果。

### 5.3 完整服务观察

当前已安装服务二进制与本轮构建的 SHA-256 一致。一次覆盖旧 snapshot 加载、多卷 MFT reconciliation、搜索索引替换和稳定运行的采样中：

| 阶段 | Working Set | Private Bytes |
|---|---:|---:|
| reconciliation 峰值 | 约 1.54 GiB | 约 1.55 GiB |
| 首轮稳定状态 | 约 929 MiB | 约 930 MiB |

随后在 2026-07-24 对同一服务 PID 每 5 秒采样一次、共 72 次（约 6 分钟）：

| 指标 | Working Set | Private Bytes |
|---|---:|---:|
| 最小值 | 870.66 MiB | 942.89 MiB |
| 最大值 | 930.54 MiB | 942.92 MiB |
| 最后一次 | 870.66 MiB | 942.89 MiB |

这 6 分钟内没有再次增长到 2–3 GiB，说明已删除的“每 5 分钟全量路径 snapshot 物化”没有反弹。Working Set 中途下降是 Windows 回收冷页，Private Bytes 基本不变，因此真实稳定私有提交量仍应按约 943 MiB 观察，不能把 871 MiB 当作结构内存已经释放。

相对用户看到的旧服务约 1.85–2 GiB，本轮稳定内存基本减半；但它仍明显高于 Everything 在该机器上约 300 MiB 的用户观察值，不能声称已经达到目标。剩余差距主要来自每卷 `NtfsCatalog` 与全局 `MetadataIndex` 两套基础元数据、约 223.56 MiB UTF-16 字符 arena、名称/共享路径签名，以及尚未持久化/mmap 的搜索加速器。当前工作树的单索引已经进一步降到约 557.69 MiB，但尚未把这一版安装为完整服务重新采样，因此不能直接把约 71 MiB 单索引降幅等同为完整服务稳定内存降幅。

没有保留 `EmptyWorkingSet`/`SetProcessWorkingSetSize(-1, -1)` 方案：它只能改变任务管理器 Working Set，并会增加冷页缺页和首次宽泛查询延迟。当前只在大阶段结束后调用 `HeapCompact` 回收已经释放的临时堆块。

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

- 让 Catalog 与 MetadataIndex 共享名称/路径组件，优先消除完整服务仍存在的双份基础数据；
- 将 snapshot 直接流式读入紧凑索引，降低当前约 2.17 GiB 的单进程构建峰值；
- 减少完整路径重复和 UTF-16 字符串对象开销；
- 将名称索引直接持久化或并行/增量构建；
- 对 `path:` 建立路径组件/gram 索引；
- 增量更新 posting，而不是大目录变化后重建；
- 更快或并行的 snapshot checksum；
- 建立包含真实键盘、IPC、列表 paint 和 metadata hydration 的 UI 端到端基准；
- 增加长期内存和后台协调回归门槛。
