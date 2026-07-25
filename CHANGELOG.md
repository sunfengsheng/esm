# 变更记录

本项目采用类似 Keep a Changelog 的结构。尚未发布的修改记录在 `Unreleased`；发布版本时再移动到对应版本标题下。

## Unreleased

### Added

- 建立完整文档入口，包括用户手册、查询语法、开发指南、运维手册和当前状态说明。
- 增加仓库级文档同步规则、pre-commit Hook 和 GitHub Actions 文档检查。

### Changed

- 重构根 `README.md`，使其作为项目入口而不是把所有实现细节堆在单一章节中。
- 同步架构、性能和路线图文档与当前代码实现。

### Fixed

- 文档明确多卷 NTFS 服务、数据存放位置、管理员权限要求和“只能搜索 C 盘”等常见问题的排查步骤。

### Performance

- 2026-07-25 18:12 复测确认，修复版安装服务在多轮固定 30 分钟 reconciliation 后仍升到约 901.44 MiB Private Bytes / 886.39 MiB Working Set，峰值约 3426.80 MiB；原因是完整 MFT 路径字符串和新旧搜索索引每轮重叠，释放后仍被 CRT heap 保留。
- 默认多卷服务改为 USN Journal 驱动：每 250 ms 增量跟随、每分钟轻量检查挂载卷，只在启动建立 live 边界、USN checkpoint 失效/读取失败或卷集合变化时执行完整 MFT repair，取消固定 30 分钟全量替换。新的 Release 服务 SHA-256 为 `B808B460467F32BC0567EBD9A2EB28853EABF05B71FD570012E911D0C56CCBB1`；已于 18:20 通过 UAC 安装，18:21:36 完成首次 C:/D:/E: repair 后为约 436.50 MiB Private Bytes / 439.54 MiB Working Set。需运行超过原 30 分钟边界，确认不再出现健康状态下的周期 `Reconciled` 事件。
- `CompactRecord` 从 48 字节压缩到固定 40 字节：正常父关系保存 31 位记录索引，只有父记录缺失时才保存稀疏 64 位 `ParentIdAnchor`；真实 3,264,188 条单索引记录容量由约 149.42 MiB 降到约 124.52 MiB。
- 路径签名 owner 从“每记录一个 32 位索引”改为目录 bitset + rank prefix + 稀疏异常路径回退，owner 元数据由约 12.45 MiB 降到约 0.58 MiB。
- 当前代码在真实 snapshot 独立基准中的稳定 Private Bytes 中位数由约 473.04 MiB 降到约 436.32 MiB，基础结构容量由约 466.16 MiB 降到约 429.41 MiB；同机 Everything 合计约 316.77 MiB，粗略约为其 1.38 倍，记录集和格式不同，仍不能声称达到 Everything。
- 本轮单进程索引构建中位数约 21.972 秒，纯名称查询 p50 约 0.03–4.59 ms，`path:test1` 约 41.40 ms；构建峰值仍约 2279.45 MiB Working Set / 2299.08 MiB Private Bytes。
- 2026-07-25 10:25 已通过 UAC 把最新 Release 服务安装到 `C:\Program Files\everything_sm\esm_service.exe`；安装源与目标 SHA-256 均为 `66DA638E502E33DE06D2F4CE93F1C37220369D2B121CC29F3C5F3C645077A72B`。服务 PID 38636 在 10:26:06 完成 C:/D:/E: 首次全量协调后，12 次、5 秒间隔采样稳定在约 436.97–437.03 MiB Private Bytes / 441.24–441.29 MiB Working Set。
- 旧安装服务在第三次 30 分钟全量 reconciliation 后曾稳定约 953.48 MiB Private Bytes。修复后的首轮服务协调已回落到约 437.00 MiB，证明新二进制已生效；但本次没有捕获启动协调峰值，第二次 30 分钟完整协调仍待验证，暂不能把首轮结果表述为长期稳定结论。IPC 已确认返回 `D:\test1\123456789.txt`；首次冷查询异常为 1765.96 ms，随后 6 次服务端查询为 0.333–0.449 ms。
- 默认多卷 `mft-auto` 服务不再长期保留每卷 `NtfsCatalog`；USN create/update/rename/delete 现在直接命名空间化并应用到全局 `MetadataIndex`，该阶段曾把完整服务首次协调后的 Private Bytes 从约 958 MiB 降到约 479 MiB。
- 基础索引把正常目录和文件都压缩为“名称 + parent ID”；仅卷根、orphan 或路径关系异常记录保留完整路径锚点。真实 326 万条单索引字符 arena 从约 223.56 MiB 降到约 141.16 MiB，Private Bytes 从约 556 MiB 降到约 473 MiB。
- 同机实测 Everything 1.4 主进程和辅助进程合计约 316.77 MiB Private Bytes；上一阶段 ESM 首轮稳定约 479.33 MiB，约为其 1.51 倍，最新修复版首轮约为 437.00 MiB，仍不能声称达到 Everything。
- 当前启动 reconciliation 仍会短时达到约 2.57 GiB Private Bytes，单进程 snapshot 构建峰值约 2.17 GiB；完整 `vector<FileRecord>` 物化和约 966.69 MiB snapshot 仍是下一阶段重点。
- 路径 trigram Bloom 从“每条记录一份 256 位完整路径签名”改为“每个目录一份共享路径签名 + 每条记录一个 32 位 owner”；真实 326 万条单索引签名容量从约 149.41 MiB 降到约 78.62 MiB，Working Set 中位数从约 628.79 MiB 降到约 557.69 MiB。
- posting 构建改为两遍统计并直接写最终 delta/varint byte span，不再分配约 255 MiB 的临时 `uint32_t posting_positions`；同机真实索引构建中位数从约 27.041 秒降到约 22.528 秒。
- 真实 snapshot 基准增加 10 ms 构建期 Working Set/Private Bytes 峰值采样和路径签名 owner 统计；当前单进程构建峰值仍约 2.17 GiB，说明仍需流式 snapshot 构建。
- 共享目录路径签名的内存收益伴随已记录的 `path:test1` 回退：索引内 p50 从上一阶段约 17.84 ms 增至约 31.04 ms；纯名称查询仍大致保持 1–6 ms。
- `MetadataIndex::replace(std::vector<FileRecord>&&)` 现在真正消费并释放百万级源记录，避免快照记录与搜索加速器长期重复驻留。
- 基础索引把普通子文件路径压缩为“父目录完整路径 + 文件名 + parent ID”，孤儿或异常路径保留完整字符串回退。
- 名称 bigram Bloom 签名由 256 位压缩为 128 位；路径 trigram 签名单份仍为 256 位，随后进一步改为目录级共享，兼顾正确性与内存。
- 多卷 MFT 服务不再每 5 分钟从所有 Catalog 重新生成一份全量完整路径快照；改为在完整 MFT reconciliation 已经持有记录时先保存，再让索引消费该记录向量。
- 增加 `MetadataIndex` 结构容量拆分统计和真实 snapshot 内存基准输出。
- 名称 trigram posting 改为保存 `natural_name_order` 的位置，并使用 delta/varint 编码；真实 326 万条 snapshot 上 posting 从约 254.95 MiB 降到约 72.00 MiB。
- `CompactRecord` 把目录、路径模式和属性标志打包，固定记录从约 56 字节降到约 48 字节；单索引 Working Set 从约 836.37 MiB 降到约 628.79 MiB。
- 最新已安装完整服务稳定 Private Bytes 约 943 MiB，6 分钟采样没有再次增长到 2–3 GiB；仍未达到 Everything 约 300 MiB 的用户观察值。

- 使用紧凑 trigram 名称倒排索引缩小候选集合，并由完整 evaluator 校验 hash collision。
- GUI 连续输入采用两阶段结果限制：输入活跃时先返回 200 条，停止约 250 ms 后补全至配置上限。
- 搜索历史延迟写入，Shell 图标按扩展名缓存，文件元数据按需后台补齐。

## 0.1.0 - Unreleased

首个可安装开发版本。当前仓库尚未声明稳定公开 API 或完整 Everything 兼容性。
