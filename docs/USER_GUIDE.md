# 用户手册

## 1. 系统要求

- Windows 10/11 64 位。
- 推荐使用 NTFS 本地磁盘；多卷快速索引依赖 NTFS MFT 和 USN Journal。
- 安装/卸载 Windows 服务、读取卷级 MFT 时需要管理员权限。
- 普通用户可以启动 GUI 并通过受保护的本地 Named Pipe 查询服务。

## 2. 安装

运行 `everything_sm-<版本>-setup.exe` 后会出现 UAC 窗口，这是因为安装程序需要：

- 写入 `C:\Program Files\everything_sm`；
- 注册并启动 `everything_sm` Windows 服务；
- 创建 `C:\ProgramData\everything_sm\indexes`；
- 创建开始菜单和桌面快捷方式。

推荐保持“安装多卷 NTFS 索引服务”选中。服务会发现带盘符的本地 NTFS 固定卷，将 `C:`、`D:`、`E:` 等卷合并为一个搜索目录。

## 3. 第一次启动

1. 安装完成后启动 `everything_sm`。
2. 等待状态栏显示搜索服务已连接。
3. 第一次创建 MFT 索引会先完成名称/路径枚举并发布名称索引；状态栏连接后即可搜索名称。大小、修改时间和属性随后在后台分批补齐，可能需要几十秒到数分钟，取决于文件数量、磁盘速度和缓存状态。
4. 补齐期间“大小”“修改时间”列以及 `size:` / `dm:` 过滤结果会逐步完善；停止服务时后台任务会在当前有限批次完成后退出。后续健康启动会先读取名称 snapshot、重放同代 metadata WAL，再从已刷盘 cursor 继续补齐；卷集合或 USN boundary 无效时仍先提供名称结果，并在后台执行完整 reconciliation。
5. 在搜索框输入内容，结果会随输入变化。

全局快捷键 `Ctrl+Alt+Space` 可显示搜索窗口。如果该组合键已被其他软件注册，当前版本不会强制抢占。

## 4. 搜索窗口

GUI 采用接近 Everything 的七菜单布局：`文件`、`编辑`、`查看`、`搜索`、`书签`、`工具`、`帮助`。

主要区域：

- **搜索框**：保留最近 100 条稳定查询历史。
- **筛选器**：内置文件类型筛选，并支持用户自定义筛选器。
- **结果列表**：虚拟列表，支持名称、路径、大小、修改时间和类型列。
- **预览面板**：显示当前选中项目的信息；尚未接入完整 Windows Preview Handler。
- **状态栏**：显示连接状态、查询状态和结果数量。

连续输入时，首个编辑约 15 ms 后可触发交互查询；150 ms 内的后续快速编辑采用约 60 ms 突发防抖，减少逐键中间请求。GUI 先请求最多 200 条结果：若首屏少于 200 条，该响应已经完整，不会再发送第二次查询，状态栏也不显示 `+`；只有首屏刚好达到 200 条且设置的最终上限更大时，停止输入约 250 ms 后才请求最终上限（默认 1000 条）。服务索引已知的文件大小和修改时间会随结果直接显示；若某条结果的修改时间仍未知，GUI 会只为这些缺失项后台读取文件系统。按创建时间、访问时间或 NTFS Change 时间排序时，GUI 才补齐全部完整结果的元数据。Shell 图标仍可能在首次遇到某种扩展名时稍后出现。

## 5. 结果操作

- 双击：打开文件或目录。
- 打开所在目录：在资源管理器中定位当前项目。
- 剪切、复制、粘贴：使用 Windows Shell 文件剪贴板。
- 复制/移动到：选择目标目录，并可设置冲突处理策略。
- 重命名：在 GUI 中修改文件名。
- 删除：默认使用回收站语义。
- 属性：打开 Windows 文件属性窗口。
- 拖放：可将多个结果拖到其他支持 Shell 拖放的应用。
- 复制文本：复制完整路径、名称、父目录或结果文本。
- 导出：支持 EFU、UTF-8 CSV 和 UTF-8 路径列表。

文件列表编辑器可以打开或创建 EFU/CSV 文件，添加/重扫目录、删除项目、去重并保存，然后把该列表作为当前搜索数据源。

## 6. 列、排序和视图

在“查看”菜单中可以：

- 显示或隐藏列；
- 调整列宽和顺序；
- 按名称、路径、大小、修改时间、类型、运行次数或上次打开时间等字段排序；
- 切换升序/降序；
- 切换详细信息和图标视图；
- 调整窗口、字体、置顶、筛选器栏、状态栏和预览面板。

窗口位置、尺寸、列配置、排序和搜索选项会保存到当前用户目录。

## 7. 搜索选项、筛选器和书签

“搜索”菜单支持：

- 区分大小写；
- 全字匹配；
- 匹配完整路径；
- 匹配变音符号；
- 正则表达式模式；
- 高级搜索；

  高级搜索表单还可以生成文件名前缀（`startwith:`）、文件名后缀（`endwith:`）和直接父文件夹（`parent:`）条件；父文件夹条件不会包含更深层的子目录。
- 内置筛选器和自定义筛选器。

书签保存查询文字以及相关搜索开关。常用查询示例：

```text
ext:jpg;png
root:
count:100 report
size:1mb..10mb
dm:today
<report|invoice> !draft
```

`child:` 返回包含匹配名称直接子项的文件夹；需要查找不包含某类直接子项的目录时，建议与 `folder:` 组合，例如 `folder: !child:*.mp3`。查询内的 `count:` 只会缩小当前 GUI/IPC 结果上限。相对日期使用本地时区，周从星期一开始。完整语法和仍未兼容的边界详见 [QUERY_SYNTAX.md](QUERY_SYNTAX.md)。

## 8. 本地持久化位置

### 机器级索引

```text
C:\ProgramData\everything_sm\indexes\mft-index.snapshot
```

同目录还会保存 `mft-index.snapshot.metadata.wal` 和 `mft-index.snapshot.state`；三者属于同一 generation，应一起备份，不要手工混用不同代文件。目录中也可能出现临时/checkpoint 文件，均由服务账户维护。

### 当前用户 GUI 数据

```text
%LOCALAPPDATA%\everything_sm\gui-settings.conf
%LOCALAPPDATA%\everything_sm\gui-history.txt
%LOCALAPPDATA%\everything_sm\bookmarks.txt
%LOCALAPPDATA%\everything_sm\filters.txt
%LOCALAPPDATA%\everything_sm\run-history.txt
%LOCALAPPDATA%\everything_sm\service-pipe.txt
```

卸载程序可选择删除机器级索引和当前用户设置。

## 9. 常见问题

### 9.1 状态栏显示“服务不可用”或“信号灯超时时间已到”

这通常表示 GUI 未能在超时时间内完成 Named Pipe 请求，并不代表搜索语法错误。

1. 以普通 PowerShell 检查状态：
   ```powershell
   & 'C:\Program Files\everything_sm\esm_service.exe' status
   ```
2. 如果服务停止，以管理员 PowerShell 启动：
   ```powershell
   & 'C:\Program Files\everything_sm\esm_service.exe' start
   ```
3. 若服务配置损坏，先停止并重新安装多卷服务。
4. 检查 Windows 事件查看器中的 `everything_sm` 服务事件。
5. 确认 GUI 连接的是 `everything_sm_service`，而不是已经不存在的自定义 Pipe。

### 9.2 只能搜到 C 盘

- 确认安装时启用了多卷 NTFS 服务，而不是只启动了兼容扫描模式。
- `D:` 必须是已挂载、有盘符的本地 NTFS 卷；FAT/exFAT、网络共享和部分虚拟/云盘不会进入 MFT 多卷索引。
- 检查服务是否运行，以及 `C:\ProgramData\everything_sm\indexes` 是否可写。
- 服务启动后会自动发现卷；新接入磁盘可重启服务以立即重新发现。
- 启动器回退模式只扫描安装时配置的一个 `scan_root`，它不能替代多卷服务。

### 9.3 D 盘新建文件搜索不到

以 `D:\123456789.txt` 为例：

1. 搜索 `123456789`，不要先启用不匹配的筛选器。
2. 确认 D 盘是 NTFS：
   ```powershell
   Get-Volume -DriveLetter D | Select-Object DriveLetter,FileSystem,HealthStatus
   ```
3. 确认服务运行：
   ```powershell
   & 'C:\Program Files\everything_sm\esm_service.exe' status
   ```
4. 等待 USN 增量处理；若 Journal 丢失或卷刚重新挂载，后台全量校准可能更久。
5. 重启服务可触发重新加载和卷发现；仍缺失时使用 `esm_cli.exe mft D: "123456789"` 做卷级诊断。

### 9.4 连续输入时感觉卡顿

- 默认每次稳定查询最多返回 1000 条；可在选项中降低最终结果上限。
- `path:`、复杂正则、重复项函数和宽泛单字符查询通常比纯名称查询更贵。
- 第一次显示某种扩展名图标可能有额外 Shell 开销；默认查询只为修改时间未知的结果读取文件系统，按创建时间、访问时间或 NTFS Change 时间排序时才运行全结果补齐。
- 首个编辑保持低延迟，快速连续编辑会被约 60 ms 的突发防抖合并；少于 200 条的首屏会直接完成，`200+` 才表示仍在等待最终 refinement。
- 如果服务正在首次建库、全量校准或写 checkpoint，等待后台工作结束后再比较。
- 使用 [PERFORMANCE.md](PERFORMANCE.md) 中的基准工具区分服务端查询延迟和 GUI 渲染延迟。

### 9.5 文件大小或修改时间没有显示

- 目录大小默认不计算，目录“大小”列保持为 0/空白是预期行为。
- 默认多卷服务会在路径重建后先发布名称索引，再按批次补齐普通文件的大小和修改时间；因此名称可能已经搜到，而“大小”“修改时间”列或 `size:` / `dm:` 结果仍在逐步出现。健康重启会 replay metadata WAL 并从 durable cursor 续跑；state/journal 验证失败时先提供已有名称结果，再后台 reconciliation。新的完整 reconciliation 创建 generation 时，如果旧 live USN 状态仍连续可信，并且 MFT 扫描后新旧共有卷可再次完成 USN 追赶，会复用文件 ID、完整路径一致且已经具有修改时间的元数据，因此不会无条件重读所有文件；启动 state 失效、扫描后追赶失败、普通 USN 读取失败或 journal gap 时会安全地禁用复用。新文件、重命名、删除竞争、旧读取失败和其他未知项仍会继续后台补齐。
- 无权限、离线、枚举后已删除、特殊 NTFS 条目以及断开的网络/云占位文件可能无法读取元数据，此时名称仍可搜索，但大小/修改时间可能为空或未知。直接 USN 变化后的重新读取若失败，服务会使旧大小/时间失效，而不是继续用 stale 值参与 `size:` / `dm:` 过滤。
- 服务 Event Log 的 `Background metadata hydration progress/completed` 会报告 `examined`、`attempted`、`hydrated`、`errors`、`applied`、`stale` 和 `elapsed`；出现 completed 表示本轮遍历结束。大量 errors 时先确认卷在线、SYSTEM 账户可访问且文件没有被批量删除；少量 stale 通常表示补齐期间发生了重命名或删除。
- 创建时间、访问时间和 NTFS Change/最近变化时间尚未作为完整全盘基线提供。

## 10. 当前限制

当前版本尚未提供完整 Everything 查询函数、ETP 协议、Windows 原生预览处理器、完整 Shell 右键扩展、按用户权限模拟查询、签名二进制和自动升级。详情见 [CURRENT_STATUS.md](CURRENT_STATUS.md)。

## 11. 独立文件内容搜索

内容搜索不集成到文件名搜索窗口，而是独立程序和独立数据库：

```powershell
.\esm_content.exe
```

首次启动会在 `%LOCALAPPDATA%\everything_sm_content\content.ini` 创建配置，默认索引当前用户目录，并把 Xapian 数据库放到 `%LOCALAPPDATA%\everything_sm_content\index`。GUI 会在需要时隐藏启动同目录的 `esm_content_service.exe`。显式 `--config` 路径必须已经存在。

### 11.1 搜索窗口操作

- 在搜索框输入内容关键词，停顿约 160 ms 后自动查询；
- “清空”按钮或 `Esc` 清空查询；
- `Ctrl+L` 聚焦并全选搜索框；
- `Enter` 立即查询；已有选中结果时打开该文件；
- 双击结果或点击“打开”使用系统默认程序打开；
- “打开目录”在资源管理器中选中文件；
- 右键结果可打开、打开所在目录或复制完整路径；
- 内容摘要中的黄色/橙色片段是匹配高亮；
- 查询返回结果时默认选中第一项，并在右侧预览窗格显示文件名、完整路径和带黄色粗体命中的索引摘要；
- 点击“隐藏预览/显示预览”、选择“查看 → 预览窗格”，或按 `Ctrl+Shift+P` 可切换预览窗格；关闭后结果列表会扩展到可用宽度；
- 红灯表示内容服务不可用，黄灯表示启动、索引或查询中，绿灯表示就绪；结果数量与服务状态分开显示。

### 11.2 内容预览窗格

当前预览是 **Xapian 索引摘要预览**，不是文件页面渲染器。选择结果后，GUI 直接使用内容服务返回的 `snippet + UTF-16 highlights`，不会再次读取或解析原始文件。因此 DOCX/PDF 预览显示的是建库时提取并写入索引的文本片段，响应路径较短，也不会因为切换选择而重复启动 Office/PDF 提取器。

尚未接入 Windows Preview Handler，也不显示 PDF 页面布局、Word 排版、图片或 OCR 结果。RichEdit 不可用时正文仍能显示，但匹配背景高亮可能不可用；正常支持的 Windows 系统会加载系统 `Msftedit.dll`。

### 11.3 PDF 和 Word

当前支持：

- `.docx`：优先使用 Windows IFilter；没有可用 IFilter 时使用内置 ZIP/XML 正文提取；
- `.pdf`：优先使用 Windows IFilter；没有可用 IFilter 时尝试受限的基础 PDF 文本流提取；
- `.doc`：依赖本机安装的 Windows IFilter；没有对应 IFilter 时不会建立内容索引；
- 纯文本和常见源码/配置格式继续按白名单索引。

不支持扫描版 PDF OCR、加密 PDF、复杂字体映射的完整兼容、完整 Office 对象/批注/宏语义。文件或提取文本超过配置中的 `maximum_bytes` 时会跳过。安装 Office 或 PDF 软件可能增加系统 IFilter，但第三方提取器的兼容性取决于本机环境。

### 11.4 建库和整机扫描

建库期间可以搜索已经提交的文件，尚未提交的文件暂时不会命中。默认配置只扫描当前用户目录，不会自动读取所有固定盘。需要整机内容索引时显式配置多个 `root` 或启动服务时使用 `--all-fixed`；这会产生明显的 CPU、磁盘读取和 Xapian 数据库写入负载。

独立开发安装包按当前用户安装，不弹 UAC；它与需要管理员权限的文件名搜索主安装包无关。开发包尚未完成 Xapian GPL/source-distribution 公开分发审查，只能用于本地开发验证。更多格式和风险边界见 [CONTENT_SEARCH.md](CONTENT_SEARCH.md)。
