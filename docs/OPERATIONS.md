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
- 使用 `mft-index.snapshot` 加速重启；
- 约每 250 ms 轮询 live 更新；
- 约每 250 ms 读取 USN 增量，约每分钟重新发现挂载的本地 NTFS 卷；
- 不再每 30 分钟无条件重建完整索引；只在 snapshot 启动后、USN checkpoint 失效/读取失败或卷集合变化时执行完整 reconciliation 并刷新 snapshot；
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
```

snapshot 使用校验和和临时文件替换，避免把未完成写入直接当成有效基线。服务启动时先尝试加载兼容 snapshot，再后台重新发现和协调卷。

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
- 精确读写、超时和重试；
- SYSTEM/Administrators 完全控制；
- Authenticated Users 可进行本地 Pipe 读写查询。

尚未实现 per-request impersonation 和按用户 ACL 过滤结果。因此该服务不应部署为远程多用户文件权限边界。

## 5. 日志和诊断

服务使用 Windows Event Log 写入错误、警告和主要生命周期事件。可在事件查看器的 Windows 日志中查找来源 `everything_sm`，或用 PowerShell：

```powershell
Get-WinEvent -LogName Application -MaxEvents 200 |
  Where-Object ProviderName -eq 'everything_sm'
```

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
4. 服务会重新枚举已发现 NTFS 卷并生成新 snapshot。

重建期间查询可能暂时不可用或结果不完整。

### live WAL 尾部损坏

恢复器会忽略不完整事务并截断撕裂尾部。若 checkpoint、metadata snapshot 和 WAL 三者无法建立一致状态，停止服务并备份全部三个文件，然后执行全量重建。

## 7. 升级和卸载

NSIS 安装程序在覆盖二进制前会停止并卸载旧服务，再安装新服务。当前没有后台自动升级和数据库 schema 自动迁移承诺，因此升级前应备份 ProgramData 索引目录。

卸载：

- Windows 设置 > 应用；或
- 开始菜单中的卸载快捷方式；或
- 管理员 PowerShell 执行 `esm_service.exe stop`、`esm_service.exe uninstall`。

卸载程序可选择删除机器级索引和当前用户 GUI 数据。

## 8. 发布前运维检查

- 干净机器安装会触发 UAC；
- 服务被正确注册为 delayed-auto；
- C/D 等 NTFS 卷均可搜索；
- 重启 Windows 后可从 snapshot 快速可用；
- 新建、重命名、移动和删除可由 USN 增量反映；
- 服务停止期间的变化可在重启协调后恢复；
- 卸载不会遗留运行中的进程或服务；
- 安装包和 portable zip 的 SHA-256 已生成。

## 9. 实验内容服务运行说明

`esm_content_service.exe` 当前是控制台服务进程原型，不是 SCM 服务。启动示例：

```powershell
.\esm_content_service.exe `
  --root D:\work `
  --db "$env:LOCALAPPDATA\everything_sm\content\xapian" `
  --pipe everything_sm_content `
  --max-mib 4
```

停止时使用 `Ctrl+C`，不要把它注册成正式系统服务。默认 `%PROGRAMDATA%\everything_sm\content\xapian` 与主文件名 snapshot/WAL 无关；开发环境建议显式使用独立 `--db`，避免不同 root 共用数据库。

当前恢复方式是停止进程、保留或移走独立 Xapian 数据库后重新启动扫描。服务停止期间删除的文件可能形成 stale 文档，目录通知溢出也需要重启校准；正式运维前必须实现自动 reconciliation、日志、SCM recovery、配置和卸载数据策略。