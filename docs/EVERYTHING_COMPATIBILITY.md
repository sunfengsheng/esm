# Everything 兼容性矩阵

## 说明与测试口径

本项目是 clean-room 实现。当前对照对象为本机安装的 **Everything 1.4.1.1030 x64**（`C:\Program Files\Everything\Everything.exe`），核对日期为 2026-07-29。本表用于记录差距，不表示已经达到 100% 兼容。

“100% 兼容”必须同时覆盖查询语法与边界、NTFS 文件身份和实时更新、GUI 与 Shell 行为、持久化与恢复、权限隔离、安装升级，以及在同一数据集和机器上的端到端性能。当前项目不得据此作完整兼容声明。

状态定义：

- `PASS`：已有实现，并有针对该项的验证；
- `PARTIAL`：存在可用子集，但仍有明确缺口；
- `FAIL`：关键能力尚未实现；
- `UNTESTED`：尚无足够对照数据。

## 功能矩阵

| 领域 | Everything 对照能力 | 当前状态 | 主要缺口 |
|---|---|---:|---|
| 名称/路径搜索 | 即时文件名、路径和扩展名搜索 | PARTIAL | 语法和边界仍未全部对齐 |
| 布尔表达式 | AND、OR、NOT、分组和优先级 | PARTIAL | 缺完整 golden/conformance 测试 |
| 正则/通配符 | regex、`*`、`?` 及相关开关 | PARTIAL | 与 Everything 的全部语义未逐项验证 |
| 大小/日期/属性 | Everything 1.4 查询函数和常量 | PARTIAL | 仍缺部分函数、属性和时间边界 |
| macros/filters/bookmarks/history | 宏、筛选器、书签和历史 | PARTIAL | GUI 子集已实现，宏体系不完整 |
| duplicate functions | 名称、大小等重复项函数 | PARTIAL | 函数组合和大数据性能未完整验证 |
| 排序/自然排序 | 多列排序与自然数字顺序 | PARTIAL | locale、稳定性和特殊字符仍需对照 |
| 增量 refinement | 输入过程中先快返、稳定后补全 | PARTIAL | 缺真实 GUI p50/p95/max 基线 |
| 多卷 NTFS 枚举 | 固定卷 MFT 聚合 | PARTIAL | 离线卷、移动卷和挂载点策略不完整 |
| USN 实时更新 | 创建、删除、重命名和属性变化 | PARTIAL | journal gap、恢复和长期压力仍需扩展 |
| hard-link | 每个目录入口独立可见 | FAIL | 当前模型仍以文件记录为主，未完整表示所有目录入口 |
| sequence 重用 | MFT slot 重用边界 | FAIL | 完整 file reference/parent sequence 传播未完成 |
| reparse/junction/symlink/mount | 明确且一致的遍历策略 | PARTIAL | 策略和跨 provider 测试不足 |
| FAT/exFAT/网络/云盘 | 非 NTFS provider | PARTIAL | 只有目录扫描/Watcher 子集，缺完整管理 |
| ACL/按用户隔离 | 查询结果按调用用户权限过滤 | FAIL | 缺 per-request impersonation 和结果过滤 |
| GUI 菜单与列 | Everything 风格桌面窗口 | PARTIAL | 菜单已较完整，细节和高级窗口仍有差距 |
| 托盘/快捷键/拖放 | 常用桌面交互 | PARTIAL | 冲突处理、Shell 边界和自动化验证不足 |
| Shell 操作 | 打开、定位、重命名、删除、复制/移动 | PARTIAL | 与 Explorer/Everything 的所有错误语义未对齐 |
| Preview | Windows Preview Handler | PARTIAL | 当前仅基础预览信息，未接完整 Preview Handler |
| ETP/HTTP/SDK/CLI | 对外协议和稳定 SDK | FAIL | 缺 ETP/HTTP 与 SDK conformance tests |
| 安装/服务/恢复 | UAC、SCM、snapshot/WAL 恢复 | PARTIAL | 自动升级、签名、崩溃报告仍缺 |
| 日志/配置/发布 | 可运维发布产品 | FAIL | 日志轮转、签名和升级通道未完成 |

## 性能矩阵

必须在同一机器、同一卷集合、同一查询集和相同冷/热缓存条件下对照；合成微基准、进程内查询或单次人工观察不能代替端到端结论。

| 指标 | 验收要求 | 当前状态 |
|---|---|---:|
| 初次 MFT 枚举 | 与 Everything 1.4.1.1030 同机对照，目标不超过 1.10 倍 | UNTESTED |
| USN 追赶吞吐 | 同一 journal 区间对照，目标不超过 1.10 倍 | UNTESTED |
| 精确名称查询 p95 | 同一查询集对照，目标不超过 1.10 倍 | UNTESTED |
| 宽泛名称查询 p95 | 同一查询集对照，目标不超过 1.10 倍 | UNTESTED |
| 连续输入端到端 p95 | 同一自动化脚本对照，且无超过 100 ms UI stall | UNTESTED |
| 首次结果时间 | 同一窗口自动化脚本对照，目标不超过 1.10 倍 | UNTESTED |
| 稳态 Private Bytes | 同一索引和稳定期对照，目标不超过 1.10 倍 | FAIL |
| reconciliation 峰值内存 | 同一卷集合对照，目标不超过 1.20 倍 | UNTESTED |
| crash recovery 时间 | 指定 WAL 规模和故障点对照，目标不超过 1.20 倍 | UNTESTED |

2026-07-29 本机观察中，Everything 1.4.1.1030 的一个索引进程约为 310.82 MiB Private Bytes；当前 `esm_service.exe` 仍明显更高。该观察只说明内存目标尚未达到，不构成完整同条件基准。

## 当前优先级

1. 建立同机、同数据集、同查询集的自动化对照 runner；
2. 完善 NTFS 对象身份、目录入口身份和 hard-link 表示；
3. 传播并验证 MFT slot/parent sequence 边界；
4. 继续降低 checkpoint、启动构建和稳态内存；
5. 把 GUI、查询、恢复和安全矩阵中的 FAIL/UNTESTED 项逐项转为可重复测试；
6. 在完成前持续明确标注“部分兼容”，不使用 Everything 100% 兼容宣传。
