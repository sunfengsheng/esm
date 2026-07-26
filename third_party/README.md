# Third-party source / 第三方源码

## Xapian Core

- Directory / 目录：`third_party/xapian-core`
- Upstream version / 上游版本：`1.4.31`
- Release archive：`xapian-core-1.4.31.tar.xz`
- Upstream URL：`https://oligarchy.co.uk/xapian/1.4.31/xapian-core-1.4.31.tar.xz`
- SHA-256：`FECF609EA2EFDC8A64BE369715AAC733336A11F7480A6545244964AE6BC80811`
- License / 许可证：GPL-2.0-or-later；详见 `xapian-core/COPYING`

该目录来自官方发布归档，当前不包含本地源码补丁。为保持上游文件逐字节不变，
`.gitattributes` 会禁用该目录的换行转换和 Git 空白错误检查；项目自有代码仍保持
正常检查。项目构建脚本只为 MinGW 构建静态 `libxapian.a`。若将链接 Xapian 的
二进制对外分发，必须单独完成 GPL 兼容性和对应源码发布审查。

This directory is imported from the official release archive without local source patches.
Git line-ending conversion, whitespace-error checks, and short conflict-marker detection are disabled only for this pristine vendor tree.
The project build invokes the upstream Autotools build to produce a static `libxapian.a`.
Distribution of binaries linked with Xapian requires a separate GPL compliance review.
