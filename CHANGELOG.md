# 变更记录

本项目采用类似 Keep a Changelog 的结构。尚未发布的修改记录在 `Unreleased`；发布版本时再移动到对应版本标题下。

## Unreleased

### Changed

- GitHub 标签构建在配置生产 PFX 时继续生成并校验签名安装包；未配置证书时不再中止，而是生成带 `UNSIGNED-PRERELEASE.txt` 警告的未签名预发布包，并把 GitHub Release 标记为 prerelease。MSVC CI 改为使用 runner 当前默认 Visual Studio 生成器，安装 smoke 使用无空格的临时安装目录，避免 runner 工具链升级和参数拆分造成误失败。
- 未签名预发布版在 MinGW/MSVC 构建和测试通过后允许保留可下载 Release，即使隔离 runner 的安装 smoke 失败，也会把失败状态写入 `UNSIGNED-PRERELEASE.txt`；生产签名标签仍把 smoke 成功作为发布硬门槛。安装 smoke 增加命令、退出码、安装目录和 SCM 状态诊断。

### Added

- 新增 `esm_service health [pipe-name] [timeout-ms]`：先确认 SCM 服务为 Running，再通过真实 Named Pipe 空查询验证索引端到端可用；安装器和 CI 安装 smoke 不再只把“进程已启动”当作健康。
- 新增路径限定的 `stop-install-processes.ps1`、WER LocalDumps 配置和 `export-diagnostics.ps1` 诊断 ZIP；停止进程只作用于当前安装目录中的 everything_sm 可执行文件，诊断报告默认对用户名和用户目录做基础脱敏；复制的原始日志需在分享前人工检查。
- 新增 MinGW 与 MSVC Release 双编译器 CI、100/20 轮进程强杀恢复压力入口，以及静默安装、同版本覆盖升级、Pipe 健康检查和静默卸载 smoke job。标签发布新增 EXE、内嵌卸载器和最终 NSIS 安装器 Authenticode 签名入口；无证书标签只能生成明确标记的未签名预发布包，不能冒充生产实签版本。
- 新增 metadata snapshot v3 多根 component/anchor 格式：`reserved[0] == 1` 保存完整路径 anchor，普通记录只保存名称组件并通过 `parent_id` 恢复路径；加载器拒绝非法 anchor 标记、非零保留字节、缺失父节点、自循环和空 anchor。

- 为所有 Windows EXE 增加由 CMake 项目版本统一生成的 VERSIONINFO；为 `esm_service.exe` 嵌入 Event Log message table，安装服务时注册 `everything_sm` Application Event source，使生命周期、恢复和慢查询事件可显示可读消息。 同时兼容 Windows SDK 将 `READ_USN_JOURNAL_DATA` 映射为 V1 的 48 字节布局，为全部 C++ target 统一启用 MSVC `/utf-8`，并修复 GUI 参数名与 Windows SDK `small` 定义冲突及 64 位控件 ID 转换，恢复 MSVC/Windows SDK 全目标构建路径；目录 watcher 测试对杀毒/索引器造成的短暂 sharing violation 采用最长 2 秒的有界重试，避免把环境竞争误判为产品失败。
- Windows 自动测试临时目录清理仅对 `ERROR_SHARING_VIOLATION`/`ERROR_ACCESS_DENIED` 执行最长 2 秒的有界重试；scanner 测试在扫描前显式关闭并校验输出流。持续句柄占用、超时或其他错误仍使测试失败，不用无限重试掩盖句柄泄漏。
- SCM 服务安装新增自动启动、延迟启动、服务 SID、失败时 5 秒/30 秒/5 分钟三级重启以及 24 小时失败计数重置；任一关键服务配置或 Event source 注册失败都会删除本次创建的服务并返回失败。旧 ImagePath 缺少 SID 或运行参数损坏时仍先连接 SCM，再以明确 Win32 状态停止并写事件，不再表现为 1053/“服务未及时响应启动或控制请求”。
- 文件名搜索 Pipe DACL 从所有本机 Authenticated Users 收紧为安装服务时捕获的用户 SID；SYSTEM 和 Administrators 保留完全控制。Pipe host 会在创建 worker 前一次性解析并校验 SID，避免错误安全配置进入快速重试。新增 SID/DACL 与无效 SID 启动拒绝测试。该措施是单用户发布缓解，尚不等于 per-request impersonation 或按文件 ACL 过滤。
- 主 NSIS 安装器在升级前仅对 SCM 中真实存在的服务执行停止/卸载，并把 `ERROR_SERVICE_DOES_NOT_EXIST` 与其他 SCM 查询失败分开处理；服务安装/启动失败时会确认并清理可能残留的服务，只有确认服务不存在后才记录兼容模式。启动失败后的服务清理、正式卸载和升级卸载都会等待 SCM 真正删除服务，删除已进入 marked-for-delete 状态时继续等待；所有中止路径返回非零退出码。快捷方式改为当前安装用户范围。
- 暂停 CI 和 GitHub Release 的主程序 portable ZIP，避免无 SCM 的 portable 包静默只扫描默认 `C:\`；当前只发布管理员 NSIS 安装包及其 SHA-256。

- 文件名 Named Pipe 查询新增协议外分阶段诊断，记录读取、解析、索引搜索、编码、写回和总耗时；SCM 服务对成功且总耗时不少于 100 ms 的请求以最多每 5 秒一条的频率写入 Windows Event Log，并汇总被抑制条数。诊断仅保存查询字符数、选项和结果数，不记录查询文本、文件名或路径，并保持 IPC v1 wire format 不变。
- 新增 `esm_gui_pipeline_benchmark`，用于对比完整 `SearchResult` 深复制与轻量元数据请求交接的合成成本；新增连续输入防抖边界和结果元数据原位应用测试。
- 为独立内容搜索应用新增专属紫蓝色文档/放大镜图标，并把 GUI 资源、NSIS 安装图标和卸载图标切换到该资源；保留可重复生成的 SVG/PNG/ICO 源文件和脚本。
- 内容搜索 GUI 新增 160 ms 防抖、单一长期 IPC worker、过期结果丢弃、服务状态灯、Shell 文件图标、清空/打开/打开所在目录按钮、结果右键菜单以及 `Ctrl+L`、`Esc`、`Enter` 快捷键，降低连续输入时的线程创建和界面抖动。
- 内容搜索 GUI 新增 Everything 风格的右侧可开关预览窗格：默认选中首个结果，显示文件名、完整路径和 Xapian 索引摘要，并以 RichEdit 黄色粗体标出命中；可通过顶部按钮、“查看 → 预览窗格”或 `Ctrl+Shift+P` 切换。
- 内容索引新增统一文档提取调度：`.docx` 优先 Windows IFilter 并回退到内置 ZIP/XML 提取，`.pdf` 优先 Windows IFilter 并回退到受限基础文本流提取，旧 `.doc` 使用系统 IFilter；增加不依赖 Office/PDF 软件的 DOCX/PDF 自动测试。
- 将 Xapian 内容搜索正式拆分为独立应用边界：新增正式 GUI 目标 `esm_content.exe`、独立 `%LOCALAPPDATA%\everything_sm_content` 配置/数据库根、`everything_sm_content_service` Pipe、配置 round-trip 测试、显式配置路径校验和按 Pipe 单实例服务保护；内容 GUI 可按需无控制台启动同目录服务，文件名搜索进程与数据库不变。
- 新增独立的每用户 NSIS 开发安装包脚本 `packaging/build-content-installer.ps1`；包内只包含内容 GUI、内容服务和内容 CLI，不需要管理员权限，也不向主文件名搜索安装包混入 Xapian。由于 GPL/source-distribution 审查尚未完成，脚本必须显式传入 `-AllowDevelopmentPackage`，产物仅供本地开发验证。
- 新增 `docs/EVERYTHING_COMPATIBILITY.md`，以 Everything 1.4.1.1030 为本机对照，按查询、NTFS、GUI、发布和性能列出 PASS/PARTIAL/FAIL/UNTESTED，明确禁止无验证的 100% 兼容声明。
- 多卷 `mft-auto` 新增与 generation、卷身份和根边界绑定的名称/USN append-only delta WAL v2，持久化 durable cursor，并支持 checksum、write-through、`FlushFileBuffers`、撕裂尾部截断和 crash recovery。
- 新增真实多卷 NTFS reconciliation 基准入口 `esm_reconcile_benchmark`，分阶段观察 MFT 枚举、多卷 namespacing、索引构建、checkpoint 写入以及 Working Set/Private Bytes；无管理员权限的零记录运行不会作为性能结论。
- 多卷 `mft-auto` 新增与 snapshot generation 绑定的低内存元数据补齐 WAL，以及保存卷身份、root file ID、原始 USN journal boundary、补齐 cursor 和完成状态的 sidecar；恢复时会重新读取并校验当前 root file ID，健康重启会重放大小、修改时间和属性更新，并从已刷盘 cursor 继续后台补齐。
- 元数据 WAL 使用带校验事务、write-through append 和 `FlushFileBuffers`；恢复会拒绝 generation/path fingerprint 不匹配的旧更新，并自动截断不完整事务尾部。

- 文件名查询新增 Everything 风格的 `filelist:`：双引号中的 `|` 分隔完整文件名或完整路径候选，支持每项 `*`/`?` 锚定通配符、大小写/变音符号选项、路径分隔符归一化以及 base + overlay 增量视图。

- 文件名查询新增 Everything 风格的 `startwith:`、`endwith:`、`len:`、`depth:`/`parents:`、`parent:`/`infolder:`/`nosubfolders:`、`root:`、`count:`、`child:`、`empty:`、`childcount:`、`childfilecount:` 和 `childfoldercount:`；支持单个 `|`、`< >` 分组、`ext:` 分号扩展名列表、数字/大小范围、Everything 大小常量、`datemodified:`、下一个自然周/月/年、滚动 N 年/月/周/日/时/分/秒，以及英文月份/星期日期常量。高级搜索窗口新增文件名前缀、后缀和直接父文件夹条件。

- 内容服务新增整机内容索引第一阶段：显式 `--all-fixed` 固定卷发现、可重复 `--root`、每根独立 Xapian 分片数据库、全局结果聚合、默认系统/缓存目录排除与多根自动测试。
- 新增独立 Xapian 文件内容搜索原型：`esm_content_service.exe`、`esm_content.exe`、`esm_content_cli.exe`、独立 Named Pipe、纯文本提取、CJK n-gram、摘要与 UTF-16 高亮。
- 新增内容协议、Named Pipe、文本提取和 Xapian 生命周期自动测试；仓库固定包含 Xapian Core 1.4.31 上游源码，Windows CI 从源码构建静态库、运行测试并检查三个实验程序的动态依赖。

- 建立完整文档入口，包括用户手册、查询语法、开发指南、运维手册和当前状态说明。
- 增加仓库级文档同步规则、pre-commit Hook 和 GitHub Actions 文档检查。

### Changed

- 默认多卷 checkpoint consolidation 改为直接导出紧凑节点和字符串 arena，不再先物化完整 `vector<FileRecord>`、复制全部后代完整路径或在 checkpoint 成功后重建活动搜索索引；overlay 和每个卷根作为完整路径 anchor 写入 v3 snapshot。
- NSIS 覆盖升级改为在安装目录内使用持久 recovery 目录备份旧二进制、配置、卸载器和诊断脚本；新服务只有通过 SCM + Named Pipe 健康检查后才提交升级，失败时恢复旧文件并尝试重启旧服务。该实现是健康门控的部分事务回滚，不是掉电级多文件原子事务。
- 静默安装/卸载为所有交互错误和删除数据提示设置保守默认值；静默卸载默认保留索引与用户设置。

- 文件名搜索 GUI 对首个输入继续采用 15 ms 低延迟，对 150 ms 内的连续输入改用 60 ms 突发防抖，减少快速输入期间的中间 Named Pipe 查询；仍保持最多一个服务查询在途并丢弃过期 generation。
- 首个交互查询少于 200 条时现在直接视为完整响应，不再发送无意义的第二次最终查询；可立即安排缺失元数据补齐和历史写入。只有首屏刚好达到 200 条且设置的最终上限更大时才保留 `200+` 状态并执行 refinement。
- 文件名搜索的最终结果不再无条件访问文件系统补齐全部元数据；默认只补齐服务结果中修改时间仍未知的条目，按创建时间、访问时间或 NTFS Change 时间排序时才补齐全部结果。后台交接从深复制整个结果向量改为只复制所需结果的槽位和路径，最多 4 个 worker 读取，并在 UI 线程原位应用纯数值更新。

- 名称/USN WAL 恢复与 checkpoint consolidation 统一使用 generation 边界；不可信的卷、root、journal 或 cursor 状态不会静默重放，而会保留可用名称基线并安排 reconciliation。
- 健康启动恢复顺序调整为 snapshot、名称/USN WAL、元数据 hydration WAL、state sidecar 校验，再从 durable cursor 继续 USN catch-up，减少重复全盘文件 I/O。
- checkpoint consolidation 按 `base + overlay - tombstone` 生成新 generation，并原子切换 snapshot/WAL/state；已增加多轮与 100,000 delta 的合成 crash-recovery 覆盖。
- 默认多卷 `mft-auto` 服务改为两阶段首次建库和完整 reconciliation：MFT 路径重建后先保存并发布名称索引，使 Named Pipe 查询尽早可用；随后协调器按 ID 顺序每批检查 4,096 条基础记录、每批默认最多 4 个 worker 在索引锁外补齐大小/修改时间/属性，并在重新加锁时校验 ID 与路径仍匹配。USN 增量优先于下一批补齐；重命名、删除或全量替换造成的过期结果会被丢弃；每批之间检查停止请求，Event Log 每检查约 250,000 条报告一次进度并记录完成或提前停止摘要。
- CLI、前台 MFT server 和单卷 MFT 服务继续保留同步元数据补齐行为；多卷后台补齐只物化有限批次路径，不保留第二份全盘路径向量。多卷服务现在先把每批成功读取的元数据事务刷入 generation-bound WAL，再合并内存索引；卷集合、卷身份、root 和原始 USN boundary 全部有效时，健康重启可跳过立即完整 MFT reconciliation。完整 reconciliation 创建新 generation 时，只有当前 live USN 状态仍连续可信，并且 MFT 扫描完成后可把新旧共有卷再次追赶到当前 USN 边界，才会把新 MFT 基线按 ID 排序，并在当前 live base/overlay 的文件 ID、完整路径一致且修改时间已知时复用大小、修改时间和属性；新 snapshot 直接保存复用结果，后台只读取未知或 stale 项。启动 state 失效、扫描后的 USN 追赶失败、普通 USN 读取失败或 journal gap 会禁用旧元数据复用，避免遗漏路径不变的内容修改。直接 USN 变化会先清空旧大小和时间，读取失败时保持未知，防止 stale 值进入查询或后续 generation；名称/USN delta 现已由与 generation、卷身份和根边界绑定的 append-only WAL v2 持久化。

- 精确且仅含单个正向 `filelist:` 条件的查询改为复用现有 raw/accent-folded 名称前缀表：先按每个候选的 basename 缩小 base 记录范围，完整路径只为文件名精确命中的少量候选重建；overlay 继续完整求值。该优化不新增常驻文件名或路径哈希表，带 `*`/`?` 的列表仍走完整 evaluator。

- 目录直接子项匹配与统计仅在查询包含 `child:`/`empty:`/`child*count:` 时按当前 base + overlay 视图临时构建；`child:` 支持普通子串和通配符文件名，并同时检查直接子文件与子目录。普通文件名查询不承担该内存和遍历成本，增量创建/删除也会参与匹配与统计。

- 内容服务初次扫描改为每根后台工作线程，Named Pipe 在扫描开始后立即可用；每个根拥有独立递归 watcher，单根 `--db` 模式保持兼容，多根使用 `--db-root\volumes\<root-key>\xapian`。
- 文件内容索引保持为独立进程和独立数据库，现有 `esm_service.exe`、`MetadataIndex` 与文件名查询路径不链接 Xapian；内容原型改为显式 opt-in，启用后默认通过 `third_party/xapian-core` 可复现构建静态 Xapian，并保留 `SYSTEM` 开发回退。

- 重构根 `README.md`，使其作为项目入口而不是把所有实现细节堆在单一章节中。
- 同步架构、性能和路线图文档与当前代码实现。

### Fixed

- 修正 NSIS `MessageBox /SD` 参数顺序，使 `/WX` 编译能够正确解析静默默认返回值；同时处理安装文件解压失败、上次失败升级 recovery 目录未清理和诊断脚本回滚边界。
- 修正 metadata snapshot v3 writer 仍写 v2 版本号、mapped loader 只接受 v2，以及 v2 materialized loader 未恢复完整路径的问题。

- 修复初始 NTFS MFT 基线只有名称/父关系/属性、旧文件大小和修改时间长期为零的问题：CLI、前台 server 和单卷服务在路径重建后同步原地补齐；默认多卷服务先发布名称索引，再后台有界分批补齐紧凑索引保存的大小、修改时间、属性和目录状态。不可访问或枚举后瞬时消失的条目保留原值并计入 metadata errors，不会阻断名称搜索。

- 修正相对日期兼容语义：`pastweek` / `pastmonth` / `pastyear` 改为滚动下界，`last` / `prev` 保持上一个完整自然周期；周边界改为读取 Windows 当前用户“每周第一天”设置，`this*` 周期不再包含今天之后的未来日期。

- 修复多卷服务直接应用 USN 增量时只更新名称、路径和属性而没有刷新大小/时间的问题；创建、内容变化和基础信息变化现在会在路径解析后仅对直接变化项读取文件系统元数据，使实时 `size:` / `dm:` 条件和结果列使用新值。

- 修复 MinGW/UCRT 构建下 `esm_tests.exe` 未静态链接运行库的问题，避免系统 `PATH` 中混入 `mingw64` DLL 时以 `0xc0000139`（入口点不存在）退出；测试运行库策略现与发布程序一致。

- 修复内容服务在后台 watcher/初次扫描提交新 Xapian revision 时，并发查询可能抛出不继承 `std::exception` 的 `Xapian::DatabaseModifiedError` 并终止进程的问题：提交与查询快照使用共享/独占 revision 锁协调，数据库变化查询最多重新打开重试 3 次，Pipe、工作线程和服务入口增加非标准异常边界。
- 修复内容 Pipe 客户端 HANDLE 返回时被局部析构关闭的问题，并修正实验 UI 读取搜索框文本时的终止字符缓冲区越界；真实 status/search IPC、中文查询和 watcher 增量更新恢复可用。

- 文档明确多卷 NTFS 服务、数据存放位置、管理员权限要求和“只能搜索 C 盘”等常见问题的排查步骤。

### Performance

- checkpoint 峰值路径不再复制普通后代的完整路径和搜索加速器。本轮仅完成机制验证、MinGW/MSVC 单元测试和进程强杀恢复压力；尚未重新取得真实多卷百万级 checkpoint 峰值，不能据此宣称已经达到 Everything 的内存或 checkpoint 性能。

- 2026-07-28 在同一台机器、同一 `mft-index.snapshot`（1,022,362,538 字节）和同一已安装 Named Pipe 服务上，用正确保留双引号的参数分别对提交 `9e905a7` 与本轮 Release 二进制重复 5 次：精确文件名列表服务端中位数从 854.474 ms 降至 12.899 ms，精确完整路径列表从 4091.17 ms 降至 13.122 ms。该样本只覆盖服务端计时，不是 GUI p50/p95；通配符 `filelist:` 尚未使用本优化。

- 建立首个真实整机内容数据库查询基线：约 264,692～264,693 个文档、4.63 GiB Xapian 数据库、后台仍在扫描时，UI 等价 `limit=100` 的 10 个不同查询单次 IPC 延迟为 671.9～5807.3 ms；`limit=10` 的重复样本约 237.8～756.9 ms。当前内容匹配不能评价为“秒搜”，主要慢路径是读取完整 document data 并为每个返回项生成摘要。
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
