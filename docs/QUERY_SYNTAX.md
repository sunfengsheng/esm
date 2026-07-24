# 查询语法

本文只描述当前代码实际支持的语法。`everything_sm` 的查询语言是 Everything 风格的兼容子集，不保证所有 Everything 表达式可直接使用。

## 1. 普通词与隐式 AND

空格分隔的多个词默认使用 AND：

```text
annual report
```

等价于：

```text
annual AND report
```

默认普通词匹配文件名；GUI 开启“匹配路径”后，普通词也可匹配完整路径。

## 2. 引号和转义

双引号把包含空格的文本作为一个词：

```text
"annual report"
```

引号内可使用 `\"` 表示双引号。未闭合引号会返回查询错误。

## 3. 字段限定

| 语法 | 说明 | 示例 |
|---|---|---|
| `name:` | 只匹配名称 | `name:report` |
| `path:` | 匹配完整路径 | `path:projects` |
| `ext:` | 匹配扩展名，可带或不带点 | `ext:pdf` |
| `file:` / `files:` | 只返回文件 | `file: ext:cpp` |
| `folder:` / `dir:` | 只返回目录 | `folder: name:src` |

`file:` 和 `folder:` 是独立指令，后面不接值。

## 4. 布尔表达式

支持：

- `AND` 或 `&&`
- `OR` 或 `||`
- `NOT` 或独立的 `!`
- 圆括号
- 词前 `!` 排除该词

优先级从高到低为 `NOT`、`AND`、`OR`。相邻词之间自动插入 AND。

```text
path:projects (ext:cpp OR ext:h) NOT path:build
```

```text
report !draft
```

## 5. 通配符

普通词和 `name:`、`path:`、`ext:` 支持：

- `*`：零个或多个字符；
- `?`：一个字符。

```text
name:report-*.pdf
ext:jp?
```

当前通配符实现是项目自有匹配器，并非 Everything 全部通配符语义。

## 6. 正则表达式

支持以下显式作用域：

```text
regex:^report-[0-9]+\.pdf$
name-regex:^IMG_[0-9]{4}$
path-regex:\\archive\\202[0-9]\\
```

GUI 的“正则表达式”开关会把搜索文字作为正则查询发送。正则表达式通常不能使用名称 trigram 快速候选路径，宽泛表达式可能较慢。

## 7. 大小过滤

格式：

```text
size:<比较符><整数><单位>
```

比较符：`=`、`<`、`<=`、`>`、`>=`。单位：`b`、`k/kb/kib`、`m/mb/mib`、`g/gb/gib`、`t/tb/tib`，按 1024 进位。

```text
size:>10mb
size:<=512k
file: size:=0
```

不写比较符时按等于处理。

## 8. 修改时间过滤

等价前缀：`date:`、`dm:`、`modified:`。

接受本地时间格式：

```text
YYYY-MM-DD
YYYY-MM-DDTHH:MM
YYYY-MM-DD HH:MM
```

示例：

```text
dm:>=2026-01-01
modified:<2026-07-01T12:30
```

当前未实现 Everything 的相对日期词、日期区间函数和全部时间字段。

## 9. 属性过滤

等价前缀：`attr:`、`attrib:`。支持属性：

- `readonly` / `ro`
- `hidden` / `h`
- `system` / `s`
- `archive` / `a`
- `compressed` / `c`
- `encrypted` / `e`
- `offline` / `o`
- `reparse` / `junction` / `symlink`

多个属性用逗号分隔：

```text
attr:hidden,system
```

在值前使用 `!` 表示要求属性不存在：

```text
attr:!hidden
```

## 10. 查询内选项

```text
case:on
case:off
wholeword:on
wholeword:off
```

布尔值还接受 `1/0`、`yes/no`、`true/false`。GUI 中的大小写、全字、路径和变音符号开关也会进入查询请求。

## 11. 重复项

```text
dupe:name
dupe:size
dupe:name-size
```

分别按名称、大小、名称+大小筛选重复项。这只是当前支持的基础 duplicate mode，不是 Everything 全部 duplicate functions。

## 12. 组合示例

```text
file: path:work (ext:cpp OR ext:h) !path:build
```

```text
folder: name:backup* dm:<2026-01-01
```

```text
case:on wholeword:on name:README ext:md
```

```text
file: size:>100mb attr:!offline
```

## 13. 当前未兼容内容

包括但不限于：

- Everything 的全部函数、宏和预处理规则；
- 完整属性、创建时间、访问时间等函数族；
- 全部相对日期、范围和单位别名；
- 完整正则、通配符和转义兼容细节；
- ADS 内容、文件内容和 Xapian 内容查询；
- ETP 查询协议兼容；
- Everything 全部重复项函数和自然排序边界。

新增或修改语法时，必须同步更新本文件、`CHANGELOG.md` 和查询单元测试。
