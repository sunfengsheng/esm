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
3. 第一次创建 MFT 索引可能需要几十秒，取决于文件数量和磁盘速度。
4. 后续启动会先读取 `C:\ProgramData\everything_sm\indexes\mft-index.snapshot`，再在后台校准当前卷状态。
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

连续输入时，GUI 先请求最多 200 条结果以保持响应；停止输入约 250 ms 后，再请求设置中的最终上限（默认 1000 条）。文件大小、修改时间和图标可能稍后由后台补齐。

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
- 内置筛选器和自定义筛选器。

书签保存查询文字以及相关搜索开关。查询语法详见 [QUERY_SYNTAX.md](QUERY_SYNTAX.md)。

## 8. 本地持久化位置

### 机器级索引

```text
C:\ProgramData\everything_sm\indexes\mft-index.snapshot
```

同目录还可能包含 WAL 或临时/checkpoint 文件。此目录由服务账户维护。

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
- 第一次显示某种扩展名图标或补齐大量文件元数据时可能有额外 Shell/文件系统开销。
- 如果服务正在首次建库、全量校准或写 checkpoint，等待后台工作结束后再比较。
- 使用 [PERFORMANCE.md](PERFORMANCE.md) 中的基准工具区分服务端查询延迟和 GUI 渲染延迟。

### 9.5 文件大小或修改时间没有显示

- 目录大小默认不计算。
- 结果首先来自轻量名称目录，大小和修改时间可能由后台延迟加载。
- 无权限、离线、已删除、断开的网络/云占位文件可能无法读取元数据。
- 如果长时间为空，刷新查询或确认文件仍存在；同时查看服务是否处于可用状态。

## 10. 当前限制

当前版本尚未提供完整 Everything 查询函数、ETP 协议、Windows 原生预览处理器、完整 Shell 右键扩展、按用户权限模拟查询、签名二进制和自动升级。详情见 [CURRENT_STATUS.md](CURRENT_STATUS.md)。

## 11. 实验性文件内容搜索

当前内容搜索没有集成到正式 `esm_gui.exe`、portable 包和 NSIS 安装流程，需要从源码构建目录手动启动。

```powershell
.\esm_content_service.exe `
  --root D:\work `
  --db "$env:LOCALAPPDATA\everything_sm\content\xapian"
.\esm_content_lab.exe
```

实验 UI 的“内容匹配”列显示 Xapian 摘要，黄色/橙色区域表示命中词；双击可打开文件。内容服务不可用时，状态栏会明确提示“独立内容服务不可用；现有文件名搜索不受影响”。

当前只索引白名单中的纯文本文件，默认跳过大于 4 MiB 的文件，不支持 PDF、Office 和 OCR。可重复使用 `--root`，或显式使用 `--all-fixed` 索引当前可访问的固定盘；多根应指定 `--db-root`，服务会为每根创建独立 Xapian 数据库并聚合查询。`--all-fixed` 不会由安装程序自动开启。默认跳过系统目录和 `.git`、`node_modules` 等缓存/依赖目录，可用 `--exclude` 增补。首次扫描在后台执行；如果 watcher 报告通知溢出，当前需要重启内容服务重新扫描。完整命令、格式列表和风险边界见 [CONTENT_SEARCH.md](CONTENT_SEARCH.md)。