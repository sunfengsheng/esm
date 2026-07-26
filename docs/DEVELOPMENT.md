# 开发指南

## 1. 技术栈

- C++20
- CMake 3.25+
- Windows API / NTFS API
- MinGW-w64 UCRT64（当前 CI 工具链）
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
5. 构建 NSIS 安装包和 portable zip；
6. 上传 artifact；
7. `vMAJOR.MINOR.PATCH` tag 发布 GitHub Release。

当前未完成代码签名、自动升级和稳定 SDK 兼容承诺。

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

`esm_content_tests` 覆盖协议 round-trip、UTF-16 高亮范围、文本编码/大小/二进制过滤以及 Xapian upsert/search/delete 生命周期。CI 的 UCRT64 job 从仓库内源码冷构建 Xapian，测试三个实验程序，并检查内容服务没有动态依赖 Xapian DLL。portable ZIP 和 NSIS 包暂不分发内容搜索二进制，待 Xapian 许可证兼容和分发材料审查完成后再决定发布方式。

由于 Xapian 使用 GPL-2.0-or-later，发布负责人在把该原型纳入正式安装包前必须完成许可证兼容、对应源码、许可证文本和构建材料审查。仅把源码放入 `third_party` 不等于已经完成二进制分发合规。