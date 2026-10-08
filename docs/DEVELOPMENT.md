# 开发指南

## 1. 技术栈

- C++20
- CMake 3.25+
- Windows API / NTFS API
- MinGW-w64 UCRT64（当前 CI 工具链）或 MSVC + Windows SDK（辅助兼容构建）
- NSIS 3.x（安装包）
- PowerShell（构建、检查和 GUI 冒烟测试）

项目当前以 Windows 为目标平台；Linux/macOS provider 尚未实现。

## 2. 目录结构

```text
include/esm/              公共 C++ 头文件
src/core/                 查询、索引、协议和持久化核心
src/platform/windows/     Windows/NTFS/Named Pipe 实现
src/apps/                 GUI、CLI、server、service、launcher
benchmarks/               容量、快照和真实搜索基准
tests/                    单元与集成测试
packaging/                NSIS 脚本和安装包构建脚本
assets/                   图标等产品资源
tools/                    开发、文档检查和 GUI 冒烟脚本
docs/                     长期项目文档
.github/workflows/         GitHub Actions
```

## 3. 配置与构建

### MinGW Makefiles

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DESM_BUILD_TESTS=ON `
  -DESM_BUILD_BENCHMARKS=ON
cmake --build build -j 4
```

### Ninja / MSYS2 UCRT64

```bash
cmake -S . -B build-ci -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DESM_BUILD_TESTS=ON \
  -DESM_BUILD_BENCHMARKS=OFF
cmake --build build-ci --parallel 4
```

主要 CMake 选项：

- `ESM_BUILD_TESTS`：构建 `esm_tests`。
- `ESM_BUILD_BENCHMARKS`：构建 benchmark 可执行文件。

## 4. 测试

```powershell
ctest --test-dir build --output-on-failure
```

查询语法、快照、WAL、IPC、目录扫描、NTFS 目录和 GUI 设置均应有回归测试。修改行为时先补测试，再修改实现。

GUI 菜单回归需要先启动 GUI，然后运行：

```powershell
.\tools\gui-menu-smoke.ps1 -BuildDir .\build
.\tools\gui-modal-menu-smoke.ps1 -BuildDir .\build
```

脚本覆盖菜单 check/radio 状态、动态筛选器和主要模态对话框。它们不是完整端到端 UI 自动化。

## 5. 基准

构建 benchmark 后可运行：

```powershell
.\build\esm_benchmark.exe 1000000
.\build\esm_catalog_benchmark.exe
.\build\esm_snapshot_benchmark.exe
.\build\esm_real_search_benchmark.exe `
  'C:\ProgramData\everything_sm\indexes\mft-index.snapshot'
```

性能修改必须在 `docs/PERFORMANCE.md` 中记录：

- 数据集和记录数；
- Debug/Release、编译器和硬件环境；
- 查询文本、排序、结果上限；
- warm-up、样本数和 p50/p95；
- 服务端时间、IPC 往返和 GUI 调度是否分开。

不要把合成 UI 消息测试表述成真实键盘端到端延迟。

## 6. 构建安装包

```powershell
.\packaging\build-installer.ps1
```

脚本会进行隔离的 Release 构建，查找或下载固定 NSIS 工具链，执行 NSIS 编译，并把安装包与 SHA-256 文件写入 `dist/`。

使用已有构建目录：

```powershell
.\packaging\build-installer.ps1 -BuildDirectory build-release -SkipBuild
```

安装包需要管理员权限，因为它安装到 Program Files 并管理 Windows 服务。

## 7. 文档驱动的修改流程

每次修改：

1. 明确受影响的长期文档；
2. 修改代码与测试；
3. 更新 `CHANGELOG.md` 的 `Unreleased`；
4. 更新至少一份 `README.md`、`CONTRIBUTING.md` 或 `docs/**`；
5. 运行文档检查、构建和测试；
6. 查看 diff，确认文档没有超前于实现。

安装 Hook：

```powershell
.\tools\install-git-hooks.ps1
```

检查暂存区：

```powershell
.\tools\check-docs-updated.ps1 -Staged
```

检查工作区相对 HEAD 的全部修改：

```powershell
.\tools\check-docs-updated.ps1 -BaseRef HEAD -HeadRef WORKTREE
```

## 8. 提交前检查清单

```powershell
cmake --build build -j 4
ctest --test-dir build --output-on-failure
.\tools\check-docs-updated.ps1 -BaseRef HEAD -HeadRef WORKTREE
git diff --check
git status --short
```

如果修改 GUI 菜单、安装包、服务或真实磁盘索引，还应执行对应冒烟/安装验证，并在提交说明中记录人工验证范围。

## 9. CI 和发布

`.github/workflows/windows-build.yml` 在 push、pull request、手动触发和版本 tag 上运行：

1. 检查文档同步；
2. 安装 MSYS2 UCRT64；
3. 配置和编译 Release；
4. 运行测试；
5. 构建 NSIS 安装包；
6. 上传安装包及 SHA-256 artifact；
7. `vMAJOR.MINOR.PATCH` tag 发布 GitHub Release。

当前未完成代码签名、自动升级和稳定 SDK 兼容承诺。

主程序暂不生成 portable ZIP。原因是 portable 不注册多卷 SCM 服务，而当前 launcher/server 回退只能表达单个 `scan_root`；恢复 portable 发布前必须先实现明确的多根 provider/配置和对应自动测试，不能用默认 `C:\` 冒充全机索引。

CMake 对全部 MSVC C++ target 统一应用 `/permissive-` 和 `/utf-8`；核心库及独立内容搜索 target 另外启用高警告级别。缺少 `/utf-8` 时 GUI 中的 UTF-8 中文字符串可能被 ACP 误解析，Windows SDK 中的旧 `small` 定义也不得用作源代码标识符。Windows EXE 版本资源由 `cmake/version.rc.in` 从 CMake 项目版本统一生成。`esm_service.exe` 还依赖 Windows message compiler（MSYS2 `windmc` 或 Windows SDK `mc.exe`）从 `src/apps/service/service_messages.mc` 生成 message table；缺少 message compiler 时配置应失败，而不是产出 Event Viewer 无法解释的服务二进制。Windows SDK `mc.exe` 与 `windmc` 都必须生成 `service_messages.h`、`service_messages.rc` 和 `MSG00409.bin`；修改 `.mc` 后至少用当前发布工具链验证一次生成和链接。目录 watcher 自动测试在 Windows 杀毒或索引器短暂占用刚创建文件时，仅对 `ERROR_SHARING_VIOLATION`/`ERROR_ACCESS_DENIED` 做最长 2 秒的有界重命名重试；这只是测试环境去抖，不应解释为 watcher 或搜索链路的性能指标。

Windows 测试的临时目录清理使用相同错误白名单和 2 秒上限；scanner 测试还会在扫描前显式关闭并检查输出流。超过上限、持续句柄占用或其他错误仍必须使测试失败，不能用无限重试掩盖产品句柄泄漏。这些措施只是测试环境去抖，不应解释为 watcher、scanner 或搜索链路的性能指标。

## 10. 构建独立内容搜索原型

内容搜索默认不参与普通构建，必须显式传入 `-DESM_BUILD_CONTENT_SEARCH=ON`。完整 Xapian Core 1.4.31 官方发布源码保存在 `third_party/xapian-core`，来源、归档 SHA-256 和许可证记录在 `third_party/README.md`。启用后默认 `ESM_XAPIAN_PROVIDER=BUNDLED` 会通过 `cmake/BuildXapian.cmake` 和 `cmake/build-xapian-mingw.sh` 调用上游 Autotools，生成独立静态 `libxapian.a`；不需要安装 MSYS2 的 `xapian-core` 二进制包。

MinGW64 依赖：

```powershell
C:\msys64\usr\bin\pacman.exe -S --needed --noconfirm `
  mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake `
  mingw-w64-x86_64-zlib make diffutils gawk grep sed perl
```

Release 构建和测试：

```powershell
cmake -S . -B build-content -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DESM_BUILD_TESTS=ON `
  -DESM_BUILD_BENCHMARKS=OFF `
  -DESM_BUILD_CONTENT_SEARCH=ON `
  -DESM_XAPIAN_PROVIDER=BUNDLED `
  -DESM_XAPIAN_BUILD_JOBS=4
cmake --build build-content --parallel 4
ctest --test-dir build-content --output-on-failure
```

第一次构建会在构建目录的 `_deps/xapian-1.4.31-build` 中配置并编译静态库。`ESM_XAPIAN_BUILD_JOBS` 只控制 Xapian 子构建并行度。若开发机已经有 ABI 匹配的静态库，可使用 `-DESM_XAPIAN_PROVIDER=SYSTEM`；非 MinGW 编译器目前也必须使用该模式。

`esm_content_tests` 覆盖协议 round-trip、UTF-16 高亮范围、文本编码/大小/二进制过滤、Xapian upsert/search/delete 生命周期、多 shard 聚合路由、全局 limit、状态汇总、路径排除和数据库 root key。CI 的 UCRT64 job 从仓库内源码冷构建 Xapian，测试三个实验程序，并检查内容服务没有动态依赖 Xapian DLL。开发时可用两个临时 `--root` 配合唯一 `--pipe` 做多根 E2E；不要在自动测试或普通开发启动中使用 `--all-fixed`，避免未经确认触发整机扫描。portable ZIP 和 NSIS 包暂不分发内容搜索二进制，待 Xapian 许可证兼容和分发材料审查完成后再决定发布方式。

由于 Xapian 使用 GPL-2.0-or-later，发布负责人在把该原型纳入正式安装包前必须完成许可证兼容、对应源码、许可证文本和构建材料审查。仅把源码放入 `third_party` 不等于已经完成二进制分发合规。
## MinGW/UCRT 测试运行库

`esm_tests.exe`、服务、CLI 和 GUI 在 MinGW 构建中统一使用静态 GCC/libstdc++/winpthread 运行库。不要移除测试目标的 `-static`：开发机同时安装 `mingw64` 与 `ucrt64` 时，动态测试程序可能从 `PATH` 加载错误 ABI 的 DLL，并在进入 `main` 前以 `0xc0000139`（入口点不存在）退出。

验证命令：

```powershell
cmake --build build-ucrt-vendor-final --config Release --target esm_tests
ctest --test-dir build-ucrt-vendor-final -C Release -R '^esm_tests$' --output-on-failure
```

## 独立内容应用与开发安装包

正式内容 GUI 构建目标为 `esm_content`：

```powershell
cmake -S . -B build-content -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DESM_BUILD_CONTENT_SEARCH=ON `
  -DESM_BUILD_TESTS=ON `
  -DESM_BUILD_BENCHMARKS=OFF
cmake --build build-content --target `
  esm_content esm_content_service esm_content_cli esm_content_tests
ctest --test-dir build-content -R esm_content_tests --output-on-failure
```

`esm_content_tests` 现在覆盖 `ContentAppSettings` 的 INI 保存/加载、独立默认数据根、统一提取调度、DOCX ZIP/XML/实体提取、基础 PDF 文本流提取和损坏文档诊断。测试样本在运行时自行生成，因此 CI 不依赖安装 Microsoft Office、PDF 阅读器或系统第三方 IFilter。Windows IFilter 仍是环境相关的扩展路径，不能把某台开发机上的可用性当作发布保证。旧目标 `esm_content_lab` 不再生成。

内容应用图标由仓库脚本生成：

```powershell
python tools\generate_content_icon.py
```

生成的 `assets\icon\content_search.ico` 同时用于 `esm_content.exe` 的 Windows 资源和独立 NSIS 安装/卸载界面。修改图标源或生成脚本后，应提交 SVG、预览 PNG、各尺寸 PNG 和 ICO，并重新构建 GUI 与安装包。

本地开发安装包：

```powershell
.\packaging\build-content-installer.ps1 `
  -BuildDirectory build-content `
  -SkipBuild `
  -AllowDevelopmentPackage
```

脚本输出 `dist\everything-sm-content-<version>-dev-setup.exe` 和 SHA-256 文件。它是按用户、无管理员权限的独立包，不包含文件名搜索二进制。`-AllowDevelopmentPackage` 是强制的误发布保护：Xapian 静态链接的 GPL/source-distribution 方案尚未审查完成，不能把该产物上传到公开 release。

## 11. P0 发布加固验证与签名

本地无副作用验证：

```powershell
cmake --build build-release-hardening --parallel 4
ctest --test-dir build-release-hardening --output-on-failure
.\build-release-hardening\esm_tests.exe --recovery-fault-stress 100

cmake --build build-msvc18-hardening --config Release --parallel 4
ctest --test-dir build-msvc18-hardening -C Release --output-on-failure
.\build-msvc18-hardening\Release\esm_tests.exe --recovery-fault-stress 20

# 只编译安装器，不安装
.\packaging\build-installer.ps1 -BuildDirectory build-release-hardening -SkipBuild
```

安装器 smoke 会创建/删除 SCM 服务，只允许在干净 VM 或 CI runner 中运行：

```powershell
.\tools\windows-install-smoke.ps1 -InstallerPath .\dist\everything_sm-0.1.0-setup.exe
```

`build-installer.ps1` 支持 Authenticode 参数或同名环境变量：证书路径、证书密码、时间戳 URL 和 `-RequireSignature`。构建顺序为签名全部发布 EXE → 生成并签名内嵌 `Uninstall.exe` → 编译最终 NSIS → 签名最终安装器 → 校验签名。生产标签 CI 使用：

- `WINDOWS_SIGNING_CERTIFICATE_BASE64`；
- `WINDOWS_SIGNING_CERTIFICATE_PASSWORD`；
- 可访问的 RFC 3161 时间戳服务（由构建参数/环境配置）。

标签缺少证书时仍会生成安装包，但流水线同时发布 `UNSIGNED-PRERELEASE.txt`，并把 GitHub Release 标记为 **unsigned prerelease**；它只能用于测试，不能冒充生产实签版本。配置证书后标签构建会启用 `-RequireSignature`，任何 EXE、内嵌卸载器或最终安装器签名/校验失败都会阻断发布。真实正式发布前还要在隔离 VM 检查安装后的 `Uninstall.exe`，确认其 Authenticode 链和时间戳有效。

unsigned prerelease 只要求 MinGW/MSVC 构建和自动测试通过；若隔离 runner 的安装 smoke 失败，Release 仍可保留用于下载诊断，但 `UNSIGNED-PRERELEASE.txt` 会记录 smoke 结果。生产签名标签不同：安装 smoke 不成功时 release job 必须失败。`windows-install-smoke.ps1` 会输出执行命令、退出码、安装目录文件清单和 SCM 查询结果，但仍在 `finally` 中清理临时服务和文件。

MSVC job 不固定 Visual Studio 主版本，而由 GitHub Windows runner 的 CMake 选择当前默认 VS 生成器，以兼容 runner 从 VS 2022 升级到 VS 2026。安装 smoke 显式使用 `$RUNNER_TEMP\everything_sm-ci` 这类无空格目录，避免 `Start-Process`/NSIS `/D=` 参数在带空格路径上产生歧义；产品默认安装路径仍由独立发布矩阵覆盖。
