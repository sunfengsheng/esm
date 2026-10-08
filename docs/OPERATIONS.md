# 运行与维护

## 1. 运行模式

### 多卷 MFT 服务（推荐安装模式）

```powershell
.\esm_service.exe install-mft-auto "C:\ProgramData\everything_sm\indexes" everything_sm_service
.\esm_service.exe start
```

特点：

- 自动发现带盘符的本地 NTFS 固定卷；
- 合并多个卷的 MFT 记录；
- 默认 Pipe 为 `everything_sm_service`；
- 使用 `mft-index.snapshot`、同代 metadata WAL 和状态 sidecar 加速重启；
- 约每 250 ms 轮询 live 更新；
- 约每 250 ms 读取 USN 增量，约每分钟重新发现挂载的本地 NTFS 卷；
- 不再每 30 分钟无条件重建完整索引；snapshot、state generation、卷集合和 USN boundary 有效时跳过立即完整 reconciliation，只在状态/checkpoint 无效、USN 读取失败或卷集合变化时执行完整修复并刷新同代文件；
- 全新建库和完整 reconciliation 在路径重建后先保存并发布名称索引，再由协调器后台分批补齐大小/修改时间/属性；每批检查 4,096 条基础记录，默认最多 4 个 worker，USN catch-up 优先于下一批；
- 每批之间检查服务停止请求；rename/delete 或索引替换产生的旧路径结果会计为 `stale` 并丢弃。单个元数据读取失败计入 `errors`，不会使卷枚举或名称索引失败；
- 完整修复仍会短时显著增加内存；修复版会精确预留多卷合并 vector，并在协调后归还空闲 CRT heap region。判断回归时应观察 Private Bytes，不能只看 Working Set。

2026-07-25 10:25 的本机安装验证：

- `C:\Program Files\everything_sm\esm_service.exe` SHA-256：`66DA638E502E33DE06D2F4CE93F1C37220369D2B121CC29F3C5F3C645077A72B`；
- Event Log 于 10:26:06 记录 `Reconciled C:, D:, E: with 3264391 entries`；
- PID 38636 在首次协调完成后的 12 次采样中稳定约 437.00 MiB Private Bytes / 441.27 MiB Working Set；
- `esm_cli query everything_sm_service "123456789.txt"` 可返回 `D:\test1\123456789.txt`；
- 18:12 再次检查发现，固定 30 分钟重建多轮执行后已升到约 901.44 MiB Private Bytes / 886.39 MiB Working Set，峰值约 3426.80 MiB；因此上述 437 MiB 仅是首轮值。最新代码已移除无条件周期重建，并于 18:20 完成提升安装；18:21:36 首次 repair 后回落到约 436.50 MiB Private Bytes / 439.54 MiB Working Set。应在 18:51 之后检查 Event Log，确认没有健康状态下的新周期 `Reconciled` 事件。

### 单卷 MFT 服务

```powershell
.\esm_service.exe install-mft D: everything_sm_service
```

这是兼容/诊断模式，不会自动合并其他卷。

### 单卷 live checkpoint 服务

```powershell
.\esm_service.exe install D: "C:\ProgramData\everything_sm\D-service.checkpoint" everything_sm_service
```

checkpoint 必须放在被索引卷之外，否则持久化写入会反过来产生新的 USN 事件，形成自激更新循环。对应元数据 snapshot 位于 `<checkpoint>.metadata`，WAL 位于 `<checkpoint>.wal`。

### 前台服务与递归扫描回退

```powershell
.\esm_server.exe scan D:\work everything_sm
.\esm_server.exe mft D: everything_sm
.\esm_server.exe live D: C:\ProgramData\everything_sm\D.checkpoint everything_sm
```

安装版启动器优先连接 `everything_sm_service`。若 Pipe 不可用，它会依据安装目录中的 `everything_sm.ini` 启动隐藏的单目录 `scan` 服务。回退模式只覆盖配置的 `scan_root`，不是多卷 NTFS 服务的等价替代。

## 2. 服务管理

服务内部名称：

```text
everything_sm
```

显示名称：

```text
everything_sm Search Service
```

命令：

```powershell
.\esm_service.exe status
.\esm_service.exe start
.\esm_service.exe stop
.\esm_service.exe uninstall
```

`install*`、`start`、`stop`、`uninstall` 通常需要提升的管理员 PowerShell；`status` 和普通 Pipe 查询可由普通用户执行。

也可以使用系统命令：

```powershell
Get-Service everything_sm
sc.exe query everything_sm
```

安装程序把服务设置为 delayed-auto start。

## 3. 数据文件

### 多卷默认模式

```text
C:\ProgramData\everything_sm\indexes\mft-index.snapshot
C:\ProgramData\everything_sm\indexes\mft-index.snapshot.metadata.wal
C:\ProgramData\everything_sm\indexes\mft-index.snapshot.state
```

三个文件属于同一 generation，应作为一组备份或恢复。snapshot 使用校验和和临时文件替换，保存名称基线；metadata WAL 以 write-through 校验事务追加成功补齐的大小、修改时间、属性和 durable cursor；state sidecar 通过临时文件原子替换，保存卷身份、root file ID 和与 snapshot 同边界的 USN checkpoint。健康启动会先 replay WAL，再验证 state 和重新发现的卷，验证通过后追赶 USN 并从 durable cursor 继续补齐。完整 reconciliation 创建新 generation 时，只有旧 live USN 状态仍连续可信，而且 MFT 扫描后新旧共有卷可再次追赶到当前 journal 边界，才会复用 ID/完整路径一致且修改时间已知的元数据；启动 state 失效、扫描后追赶失败、普通 USN 读取失败或 journal gap 会禁用复用。直接 USN 变化的旧大小/时间会先失效，文件元数据读取失败时保持未知。Event Log 的 `metadata reuse=trusted/disabled`、`reused`、`unknown` 和 `stale` 可用于判断复用效果；预追赶失败还会记录卷和 Win32 error，后台只对未解决项继续读取。不要通过导出完整 `vector<FileRecord>` 强制回写补齐状态，这会重新制造大内存峰值。

### live checkpoint 模式

```text
<checkpoint>
<checkpoint>.metadata
<checkpoint>.wal
```

WAL 采用追加写事务。恢复时只重放完整、通过校验的事务；撕裂尾部可被截断。checkpoint 后进行 consolidation。当前并非所有多卷状态都已迁移到通用 base+delta 数据库。

### GUI 当前用户配置

位于 `%LOCALAPPDATA%\everything_sm`，详见 [用户手册](USER_GUIDE.md)。

## 4. Named Pipe 和权限

Pipe 名标准化为：

```text
\\.\pipe\everything_sm_service
```

协议为项目自有的版本化二进制协议，不是 Everything ETP。当前边界：

- 只接受本机客户端（`PIPE_REJECT_REMOTE_CLIENTS`）；
- 单帧 payload 上限 4 MiB；
- 单次结果上限 1000；
- 精确读写、连接阶段超时和有限重试；
- SYSTEM/Administrators 完全控制；
- 前台 server 默认只授权当前进程 token 的用户 SID；
- SCM 服务安装时捕获执行安装的用户 SID，把 SID 持久化到服务 ImagePath，并只向该 SID 授予 Pipe 读写查询；
- 不再向通用 Authenticated Users (`AU`) 授予访问。

这仍是单用户连接缓解：服务以 SYSTEM 构建全机索引，尚未实现 per-request impersonation 和按用户 ACL 过滤结果。管理员仍可连接；其他本机用户不能使用默认 Pipe。该服务不应部署为远程或共享多用户文件权限边界，更换主要使用用户后应重新安装服务以刷新允许 SID。

## 5. 日志和诊断

服务使用 Windows Event Log 写入错误、警告和主要生命周期事件。安装流程会注册 `everything_sm` Application Event source，并把 `EventMessageFile` 指向带内嵌 message table 的 `esm_service.exe`；正常安装后的事件查看器 Message 应为可读文本，而不是空消息。多卷建库/协调会先记录 `name index published; background metadata hydration scheduled`；后台任务每检查约 250,000 条记录进度，并在完成或服务停止时报告 `examined`、`attempted`、`hydrated`、`errors`、`applied`、`stale`、`elapsed` 以及 `WAL=enabled`/`WAL=disabled`。`hydrated` 是文件系统读取成功数，`applied` 是路径仍匹配并完成合并的记录数；读取失败保留原值，过期路径计为 `stale`。WAL 写入失败会记录警告并继续内存补齐，但该批次不能保证在重启后恢复。可在事件查看器的 Windows 日志中查找来源 `everything_sm`，或用 PowerShell：

文件名 SCM 服务还会对成功且连接后总耗时不少于 100 ms 的查询记录 `Slow search:` 警告。为避免连续输入或自动客户端刷满 Application 日志，同一服务进程最多每 5 秒记录一条，期间被抑制的慢查询数会在下一条的 `suppressed_since_last` 字段汇总。字段包括 `total_ms`、`read_ms`、`decode_ms`、`search_ms`、`encode_ms`、`write_ms`、结果数、limit、query 字符数、排序和匹配 flags。日志结构刻意不保存原始 query、文件名或路径。100 ms 是当前固定保守阈值，不是性能 SLA；`total_ms` 从服务端接受连接后开始，不包含客户端排队等待、GUI 防抖、窗口消息调度、ListView 绘制或 Shell 图标加载。

```powershell
Get-WinEvent -LogName Application -MaxEvents 200 |
  Where-Object ProviderName -eq 'everything_sm'

# 仅查看慢查询，并展开分阶段耗时
Get-WinEvent -LogName Application -MaxEvents 1000 |
  Where-Object {
    $_.ProviderName -eq 'everything_sm' -and
    $_.Message -like 'Slow search:*'
  } | Select-Object TimeCreated,Message
```

注意：当前查询 API 的 `timeout_ms` 只约束连接 Pipe 的等待。连接成功后的同步读写以及已进入索引 evaluator 的查询没有服务端协作取消；客户端超时或 GUI 丢弃过期 generation 不等于服务端查询已经停止。若慢查询持续出现，应先根据 `search_ms` 与编码/写回阶段区分索引 evaluator 和传输问题，不要通过强杀服务线程处理。

快速诊断：

```powershell
# 查询服务
.\esm_service.exe status

# 直接枚举 D: 的 MFT 并查询
.\esm_cli.exe mft D: "123456789"

# 查看 USN Journal
.\esm_cli.exe journal D:

# 测试 Pipe 查询
.\esm_cli.exe query everything_sm_service "readme"
```

检查磁盘类型：

```powershell
Get-Volume | Select-Object DriveLetter,FileSystem,DriveType,HealthStatus
```

## 6. 恢复流程

### GUI 无法连接

1. `esm_service.exe status`。
2. 服务停止时，以管理员权限执行 `start`。
3. 服务持续启动失败时查看 Event Log。
4. 确认 ProgramData 数据目录存在且 SYSTEM 可写。
5. 必要时停止服务，备份损坏 snapshot，然后重新安装/启动以重建。

### snapshot 损坏或版本不兼容

1. 停止服务。
2. 把 `mft-index.snapshot` 移到备份目录，不要直接删除唯一副本。
3. 启动服务。
4. 服务会重新枚举已发现 NTFS 卷，先生成并发布名称 snapshot，再后台补齐大小/修改时间。

无可用 snapshot 的首次重建在名称/路径基线发布前仍需等待 MFT 枚举和索引构建；名称索引发布后查询可用，元数据继续在后台逐步完善。若 Event Log 显示大量 `errors`，检查卷在线状态、SYSTEM 访问权限、reparse/特殊系统项和是否正在批量删除文件。少量错误不会阻断名称索引。停止服务时当前最多 4,096 条检查批次会先结束，因此停止不是单个文件粒度的即时取消。

### WAL、sidecar 或 checkpoint 异常

- 多卷 metadata WAL 的不完整事务尾部会在启动 replay 时自动截断；generation 不匹配、事务校验失败、ID 缺失或路径 fingerprint 变化的 update 不会盲目覆盖当前记录。
- metadata WAL 无法使用时，服务保留名称 snapshot，并从 ID 0 重新执行后台元数据补齐。
- state 文件缺失/损坏、generation 不匹配、卷身份/root file ID 变化或 USN journal gap 时，服务先提供名称 snapshot 查询，再安排完整 MFT reconciliation。
- 人工恢复时应停止服务并同时备份 `mft-index.snapshot`、`.metadata.wal` 和 `.state`，不要混用不同 generation 的文件。
- 单卷 live checkpoint 的 WAL 同样只重放完整事务并截断撕裂尾部；若 checkpoint、metadata snapshot 和 WAL 无法建立一致状态，备份后执行全量重建。

## 7. 升级和卸载

NSIS 安装程序先通过 SCM 确认旧服务存在，再在覆盖二进制前停止并卸载；只有 `sc query` 返回 1060 才按“服务不存在”继续，其他 SCM 查询错误会中止且返回非零退出码。覆盖升级会先把旧程序、配置、README、卸载器和诊断脚本备份到 `$INSTDIR\.everything_sm-upgrade-backup`，并拒绝覆盖上次失败升级遗留的 recovery 目录；路径限定脚本只停止当前安装目录中的 everything_sm 进程，不会按名称终止其他开发目录中的同名进程。新服务必须通过 `esm_service health everything_sm_service 120000` 的 SCM + Named Pipe 查询后才提交升级；失败时恢复旧文件并尝试重启旧服务。服务管理命令在 `DeleteService` 已报告 marked-for-delete 时仍会等待 SCM 完成真实删除，降低立即重装竞态。正式卸载同样不会忽略服务状态查询、停止或删除失败。当前流程仍没有持久 upgrade manifest/state machine、多文件单点原子提交、后台自动升级或数据库 schema 自动迁移承诺，因此升级前仍应备份 ProgramData 索引目录。

卸载：

- Windows 设置 > 应用；或
- 开始菜单中的卸载快捷方式；或
- 管理员 PowerShell 执行 `esm_service.exe stop`、`esm_service.exe uninstall`。

卸载程序可选择删除机器级索引和当前用户 GUI 数据。

## 8. 发布前运维检查

GitHub 的 unsigned prerelease 只用于下载诊断：若其 `UNSIGNED-PRERELEASE.txt` 记录 installer smoke 不是 `success`，不得把该包宣传为已通过安装验收。生产签名标签仍由流水线强制要求 installer smoke 成功。

- 干净机器安装会触发 UAC；
- 服务被正确注册为 automatic + delayed-auto，并配置 5 秒、30 秒、5 分钟三级重启；
- `sc.exe qsidtype everything_sm` 显示 unrestricted service SID，`sc.exe qfailure everything_sm` 显示 failure actions；
- Event source 注册表项存在，Event Viewer 中 `everything_sm` 消息可读；
- 服务 ImagePath 包含目标安装用户的 `S-1-...` SID，Pipe DACL 不包含 `AU`；
- C/D 等 NTFS 卷均可搜索；
- 重启 Windows 后可从 snapshot 快速可用；
- 新建、重命名、移动和删除可由 USN 增量反映；
- 服务停止期间的变化可在重启协调后恢复；
- 卸载不会遗留运行中的进程或服务；
- NSIS 安装包及其 SHA-256 已生成；portable ZIP 当前暂停发布，不能列入发布附件。

## 9. 实验内容服务运行说明

`esm_content_service.exe` 当前是控制台服务进程原型，不是 SCM 服务。单根启动示例：

```powershell
.\esm_content_service.exe `
  --root D:\work `
  --db "$env:LOCALAPPDATA\everything_sm\content\xapian" `
  --pipe everything_sm_content `
  --max-mib 4
```

多根使用数据库根目录，由服务为每个根创建独立 shard：

```powershell
.\esm_content_service.exe `
  --root D:\work `
  --root E:\documents `
  --db-root D:\everything-sm-content `
  --exclude D:\work\generated
```

整机固定盘索引必须由操作者显式传入 `--all-fixed`。该操作会产生明显的首次扫描 CPU、磁盘读取和数据库写入负载，运行前应确认数据库盘空间、排除目录和单文件上限；安装程序不会自动执行。默认排除系统目录和常见缓存目录，`--no-default-excludes` 会扩大扫描范围，应谨慎使用。

停止时使用 `Ctrl+C`，不要把它注册成正式系统服务。单根默认 `%PROGRAMDATA%\everything_sm\content\xapian`；多根布局为 `<db-root>\volumes\<root-key>\xapian`，均与主文件名 snapshot/WAL 无关。初次扫描在后台运行，Pipe 可立即返回状态和已经提交的结果。

当前恢复方式是停止进程、保留或移走对应 shard 数据库后重新启动扫描。服务停止期间删除的文件可能形成 stale 文档，目录通知溢出也需要重启校准；当前没有持久任务队列、自动 reconciliation、日志轮转、SCM recovery、权限模拟、配置和卸载数据策略。


## 10. 名称 WAL、checkpoint 与恢复

默认多卷服务的 snapshot、名称/USN WAL、元数据 hydration WAL 和 state sidecar 必须属于同一 generation。服务启动时会校验卷集合、卷身份、root file ID、journal ID、USN boundary 和 durable cursor；任一关键边界不可信时，不会把旧增量静默套用到新基线，而是保留可用名称 snapshot 提供查询并安排 reconciliation。

健康恢复检查项：

- snapshot generation 与 state 一致；
- 每个卷的身份、root file ID 和 journal ID 未变化；
- durable cursor 位于当前 journal 可追赶范围内；
- WAL 事务 checksum、长度和提交标记完整；
- 撕裂尾部已截断，且没有跨 generation 的旧 WAL；
- checkpoint 临时文件和 manifest 切换状态一致。

checkpoint consolidation 会把当前 `base + overlay - tombstone` 写入新 generation snapshot，然后原子切换 snapshot/WAL/state。若恢复时看到 snapshot 已生成但 state/manifest 未切换，继续使用上一完整 generation；若 state 指向缺失或损坏 snapshot，则拒绝该 generation 并进入安全 reconciliation。不要手工混用不同 generation 的 snapshot、WAL 或 sidecar。

建议诊断顺序：

1. 查看 Windows Event Log 中的 generation、卷校验、WAL replay 和 reconciliation 事件；
2. 记录当前数据目录后停止服务；
3. 只在备份后隔离损坏 generation 文件，不要直接编辑二进制 WAL；
4. 重启服务并确认名称 snapshot 是否先恢复查询；
5. 等待 USN catch-up 或完整 reconciliation 完成，再比较大小/时间和增量结果；
6. 对百万级 checkpoint 的耗时和内存问题，使用 `esm_reconcile_benchmark` 与真实 SCM 观察分别记录，不能用合成结果替代生产结论。

## 独立内容服务配置与运行

默认配置路径：

```text
%LOCALAPPDATA%\everything_sm_content\content.ini
```

服务可直接从配置启动：

```powershell
.\esm_content_service.exe --config "$env:LOCALAPPDATA\everything_sm_content\content.ini"
```

GUI `esm_content.exe` 会按需以无控制台窗口方式启动服务。相同 Pipe 的服务使用单实例互斥体，重复启动会正常退出而不会建立第二套扫描线程。默认 Pipe 是 `everything_sm_content_service`。

配置和数据库是当前用户级状态；独立 GPL 预览安装包不需要管理员权限，也不安装 SCM 服务。停止服务可关闭 GUI 后按 PID 终止 `esm_content_service.exe`，或在前台调试时使用 `Ctrl+C`。卸载程序会询问是否删除 `%LOCALAPPDATA%\everything_sm_content`。安装目录的 `SOURCE-CODE.txt` 指向同一版本的完整对应源码 ZIP；发布验收必须校验该资产及其 SHA-256 均存在。

若需要整机固定盘索引，必须显式把配置中的 `all_fixed` 改为 `1` 或传入 `--all-fixed`。操作前应确认数据库空间、排除目录、当前用户访问权限和首次扫描负载。多用户不得共享同一个可写 Xapian 数据库；当前尚无 per-request impersonation。

## 11. P0 健康检查、诊断与升级恢复

### 服务健康检查

```powershell
# 默认 Pipe=everything_sm，默认等待 120000 ms
.\esm_service.exe health

# 安装包当前使用的 Pipe 和自定义等待时间
.\esm_service.exe health everything_sm_service 180000
```

返回成功表示 SCM 为 Running 且 Named Pipe 已完成一次真实查询；只看到进程存在或 `sc query` 为 Running 不代表索引已经可用。非零退出时先查看 Application 日志中的 `everything_sm` provider，再导出诊断包。

### 诊断包与 WER dump

管理员安装会为 `esm_service.exe`、`esm_gui.exe` 和 `esm_server.exe` 配置 WER LocalDumps。dump 可能包含查询、路径、文件名和进程内存中的其他敏感数据，只能按敏感诊断材料传输和保存。

```powershell
& "$env:ProgramFiles\everything_sm\export-diagnostics.ps1" `
  -OutputPath "$env:USERPROFILE\Desktop\everything_sm-diagnostics.zip"
```

诊断脚本收集服务状态、ImagePath、版本、有限的 ProgramData 文件清单、最近 Application 事件和最多 10 个日志文件，并对报告中的当前用户名/用户目录做基础脱敏；复制进 ZIP 的日志文件保持原样，它不会自动收集 dump。提交前仍应人工检查整个 ZIP。

### 覆盖升级与 recovery 目录

升级备份位于：

```text
%ProgramFiles%\everything_sm\.everything_sm-upgrade-backup
```

新版本只有通过 `esm_service health` 后才删除该目录。健康检查失败时安装器恢复旧文件并尝试重启旧服务；若恢复不完整，目录会保留供人工处理，下一次安装会拒绝覆盖。此流程不是掉电级原子升级：不要在升级时强制关机，生产升级前应另行备份 `%ProgramData%\everything_sm`。

静默卸载使用保守默认值：遇到不可恢复错误时取消，删除数据提示默认选择“否”，因此 `/S` 默认保留机器索引和当前用户设置。
