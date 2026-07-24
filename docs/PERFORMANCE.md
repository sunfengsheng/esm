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
record_count = 3,264,136
sort = name natural order
limit = 1000
```

### 索引内查询 p50

测试使用 Release 构建，同一真实 snapshot 连续独立运行 3 次；每次重新载入 snapshot、重建索引，每个查询运行 9 次。下表取 3 次运行各自 p50 的中位数，不是 GUI 端到端延迟。

| 查询 | p50 |
|---|---:|
| `1` | 1.55 ms |
| `12` | 4.81 ms |
| `123` | 2.81 ms |
| `txt` | 2.17 ms |
| `windows` | 1.36 ms |
| `report` | 4.46 ms |
| `123456789` | 0.81 ms |
| `123456789.txt` | 0.04 ms |
| `path:test1` | 43.46 ms |

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

本轮最终代码的 3 次真实索引构建耗时为 20.418–21.970 秒，中位数约 21.777 秒。目录也改为父链组件化后，`path:test1` 的 3 次 p50 中位数为 43.46 ms；纯名称查询仍大致为 0.04–4.81 ms。这是进一步节省约 83 MiB 单索引稳定内存后的已知路径查询权衡，不能隐瞒或与纯名称查询混为一谈。

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

测试方法：2026-07-24，Release 构建；输入为 `C:\ProgramData\everything_sm\indexes\mft-index.snapshot`；记录数 3,264,136；完整进程独立运行 3 次。索引构建结束后读取 Working Set 和 Private Bytes，通过 `MetadataIndex::storage_stats()` 统计 vector capacity；另以 10 ms 间隔采样 snapshot 已载入到索引替换完成之间的进程峰值。

| 项目 | 观察值 |
|---|---:|
| 索引完成 Working Set | 473.88–474.68 MiB，中位数 474.66 MiB |
| 索引完成 Private Bytes | 473.01–473.05 MiB，中位数 473.04 MiB |
| 基础索引结构容量 | 约 466.16 MiB |
| CompactRecord | 约 149.42 MiB |
| 字符 arena | 约 141.16 MiB |
| Bloom 签名及 owner | 约 78.62 MiB |
| 目录路径签名 | 536,095 份 |
| 每记录路径签名 owner | 约 12.45 MiB |
| trigram posting | 约 72.01 MiB |
| posting entry 数 | 66,807,284 |
| posting 平均字节/entry | 约 1.13 B |
| 相对 `uint32_t` posting 的容量比例 | 约 0.28 |
| 排序/前缀结构 | 约 24.96 MiB |
| name-only 目录和文件路径 | 3,263,997 条 |

第二阶段不再为每条记录保存一份 256 位完整路径 Bloom。目录保存共享的 256 位完整目录路径签名，每条记录只保存 32 位 owner；文件名部分复用已有的 128 位名称签名。第三阶段进一步让正常目录和文件都只保存名称并通过 `parent_id` 父链重建路径，仅根、orphan 和异常关系保留完整路径锚点。含 `\`、`/`、`:` 的路径词不使用共享签名过滤，直接回退完整 evaluator，避免跨路径边界产生 false negative。字符 arena 从约 223.56 MiB 降到约 141.16 MiB，单索引 Private Bytes 从约 556.27 MiB 降到约 473.04 MiB。

posting 构建仍使用两遍统计并直接把 delta/varint 写入最终 byte span，不临时保存约 255 MiB 的 `uint32_t posting_positions`。最终 posting 容量约 72.01 MiB，构建时间中位数约 21.777 秒。由于测试前仍先把完整 `vector<FileRecord>` snapshot 载入内存，构建阶段 10 ms 采样峰值仍为约 2154.98 MiB Working Set / 2174.28 MiB Private Bytes；这不是完整服务 reconciliation 峰值，也说明下一阶段仍需流式加载或直接从持久化紧凑结构构建。

共享路径签名和全父链路径组件化累计显著降低稳定内存，但 `path:test1` 的 3 次 p50 中位数为 43.46 ms。纯名称查询仍大致为 0.04–4.81 ms。最终 evaluator 仍负责正确性校验，因此 Bloom/hash 误报只增加候选，不改变结果。曾试验把名称 Bloom 从 128 位压到 64 位，`path:test1` 接近 90 ms，因此没有保留该方案。

### 5.3 完整服务与 Everything 对比

测试方法：2026-07-24，把 `build-release\esm_service.exe` 以管理员权限替换到 `C:\Program Files\everything_sm`，确认安装文件 SHA-256 与构建产物一致；服务 PID 32204 启动后每 5 秒采样 Working Set 和 Private Bytes。启动 reconciliation 期间本轮采样看到约 1.49 GiB Private Bytes；更完整的前次启动采样峰值约 2.57 GiB，二者都属于短时构建峰值，不应与稳定状态混为一谈。

索引替换后连续 12 次、约 1 分钟采样完全稳定：

| 指标 | Working Set | Private Bytes |
|---|---:|---:|
| 最小值 | 479.28 MiB | 479.33 MiB |
| 最大值 | 479.28 MiB | 479.33 MiB |
| 最后一次 | 479.28 MiB | 479.33 MiB |

优化前同机已安装服务稳定 Private Bytes 约 957.6–958.0 MiB。多卷 `mft-auto` 不再长期保留每卷 `NtfsCatalog` 后，稳定私有提交量下降约 50%，并且已接近单搜索索引的约 473 MiB，证明原先 Catalog 与 MetadataIndex 的大规模重复驻留已消除。

同一时刻本机 Everything 1.4.1.1030 主进程约 312.78 MiB Private Bytes，辅助/服务进程约 3.99 MiB，合计约 316.77 MiB；其数据库约 146.20 MiB、总条目约 369.9 万。ESM snapshot 约 966.69 MiB、索引条目约 326.4 万。两者不是相同记录集、文件格式或实现，不能直接当作严格同条件基准；按当前稳定 Private Bytes 粗略比较，ESM 约为 Everything 的 1.51 倍，仍未达到目标。

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
