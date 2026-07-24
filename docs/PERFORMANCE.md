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

### 紧凑 trigram CSR 倒排索引

名称索引使用 65,536 个 16-bit trigram hash bucket，通过 `offsets[65537]` 和连续 posting 数组存储。查询从 mandatory name trigrams 中选择最稀疏 posting，再运行完整 evaluator。

该方案显著降低普通名称子串查询的扫描量，并保持索引结构连续、适合缓存。16-bit hash 会产生 collision，但只影响候选数量，不影响最终正确性。

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
record_count ≈ 3,262,754
sort = name natural order
limit = 1000
```

### 索引内查询 p50

| 查询 | p50 |
|---|---:|
| `1` | 1.92 ms |
| `12` | 3.85 ms |
| `123` | 1.42 ms |
| `txt` | 0.96 ms |
| `windows` | 1.14 ms |
| `report` | 1.89 ms |
| `123456789` | 1.07 ms |
| `123456789.txt` | 0.04 ms |
| `path:test1` | 13.95 ms |

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

### 优化前后

| 查询 | 优化前 | 优化后服务 p50 |
|---|---:|---:|
| `report` | 约 276 ms | 约 3 ms |
| `123456789.txt` | 约 466 ms | 约 0.17 ms |

名称倒排索引在该 snapshot 上构建约 26.7 秒。

## 4. GUI 合成连续变化测试

测试通过跨进程 `SetWindowText` 连续改变 ComboBox 文本，共 62 次：

- 平均 UI 调用延迟：0.25 ms；
- P50：0.18 ms；
- P95：0.44 ms；
- 最大：3.75 ms；
- 测试期间窗口保持可响应。

**限制：** 这是合成消息调度测试，不包含真实键盘输入、IME、显示器刷新、Shell 图标、完整 IPC 往返和人眼感知，因此不能作为真实端到端输入延迟。它只证明文本变化处理没有长时间同步阻塞调用方。

## 5. 内存状态

约 326 万记录的已安装服务开发样本：

| 指标 | 观察值 |
|---|---:|
| Working Set | 约 3589 MB |
| Private Bytes | 约 3662 MB |
| Virtual Size | 约 7832 MB |

单独 `NtfsCatalog` 已做到约 99 MB/百万节点，但完整服务还包含 `MetadataIndex`、完整路径、搜索字符串、倒排 posting、覆盖层和运行时工作集。当前内存仍是与 Everything 差距最大的指标之一。

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

- 让 Catalog 与 MetadataIndex 共享名称/路径组件；
- 减少完整路径重复和 UTF-16 字符串对象开销；
- 将名称索引直接持久化或并行/增量构建；
- 对 `path:` 建立路径组件/gram 索引；
- 增量更新 posting，而不是大目录变化后重建；
- 更快或并行的 snapshot checksum；
- 建立包含真实键盘、IPC、列表 paint 和 metadata hydration 的 UI 端到端基准；
- 增加长期内存和后台协调回归门槛。
