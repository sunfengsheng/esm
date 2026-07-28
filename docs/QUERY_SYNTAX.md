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
| `ext:` | 匹配扩展名，可带或不带点；分号分隔多个扩展名 | `ext:jpg;png` |
| `file:` / `files:` | 只返回文件 | `file: ext:cpp` |
| `folder:` / `dir:` | 只返回目录 | `folder: name:src` |

`file:` 和 `folder:` 是独立指令，后面不接值。

### Everything 风格的名称与路径函数

| 语法 | 说明 | 示例 |
|---|---|---|
| `startwith:` | 文件名以指定文字开头 | `startwith:report` |
| `endwith:` | 文件名以指定文字结尾 | `endwith:.pdf` |
| `len:` | 按文件名 UTF-16 字符数比较 | `len:>=20` |
| `depth:` / `parents:` | 按相对卷根或共享根的父目录层数比较 | `depth:=0` |
| `parent:` / `infolder:` / `nosubfolders:` | 只匹配直接位于指定目录中的项目，不包含更深子目录 | `parent:"D:\Projects"` |
| `root:` | 只匹配直接位于盘符根目录或 UNC 共享根中的项目 | `root:` |
| `child:` | 匹配含有指定名称的直接子文件或子目录的文件夹 | `child:desktop.ini` |
| `empty:` | 只匹配没有直接子文件或子目录的空目录 | `empty:` |
| `childcount:` | 按直接子文件与子目录总数匹配目录 | `childcount:>=10` |
| `childfilecount:` | 按直接子文件数匹配目录 | `childfilecount:=0` |
| `childfoldercount:` | 按直接子目录数匹配目录 | `childfoldercount:1..3` |
| `count:` | 将本次查询结果上限限制为指定非负整数 | `count:100 report` |

`len:`、`depth:` 和三个 `child*count:` 函数支持 `=`、`<`、`<=`、`>`、`>=`，也支持两端包含的 `10..20` 或 `10-20` 范围。`child:`、`empty:` 和所有子项计数函数都只检查直接子级，不递归检查后代，并且自身不会匹配文件。`child:` 支持普通子串和 `*`/`?` 通配符，例如 `child:*.mp3`；`empty:` 等价于“目录且直接子项总数为 0”。`parent:` 比较完整直接父路径；路径包含空格时使用引号。`count:` 与 GUI/IPC 请求上限同时存在时取较小值。高级搜索窗口也可以生成 `startwith:`、`endwith:` 和 `parent:` 条件。

当前这些函数是兼容子集：尚未实现 Everything 在函数参数中的全部通配符、宏展开和范围语义。`child:`/`empty:`/`child*count:` 会在查询开始时根据一致的 base + overlay 视图临时匹配或统计目录直接子项；普通查询不会构建该结构，但在超大目录树上执行直接子项查询仍可能明显慢于 Everything。多个 `child:` 条件目前逐项判断每个直接子名称。

## 4. 布尔表达式

支持：

- `AND` 或 `&&`
- `OR`、`||` 或单个 `|`
- `NOT` 或独立的 `!`
- 圆括号或 Everything 风格的尖括号 `< >` 分组
- 词前 `!` 排除该词

优先级从高到低为 `NOT`、`AND`、`OR`。相邻词之间自动插入 AND。

```text
path:projects (ext:cpp OR ext:h) NOT path:build
```

```text
<report|invoice> !draft
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
ext:jpg;png;t*
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

不写比较符时按等于处理。还支持两端包含的范围：

```text
size:1mb..10mb
size:1mb-10mb
```

支持 Everything 常用大小常量：

| 常量 | 当前边界 |
|---|---|
| `empty` | `size = 0` |
| `tiny` | `0 < size <= 10 KiB` |
| `small` | `10 KiB < size <= 100 KiB` |
| `medium` | `100 KiB < size <= 1 MiB` |
| `large` | `1 MiB < size <= 16 MiB` |
| `huge` | `16 MiB < size <= 128 MiB` |
| `gigantic` | `size > 128 MiB` |

`size:unknown` 尚未实现。多卷服务的 USN 增量会为直接创建或变化的项目刷新大小和修改时间；当前初始全盘 `FSCTL_ENUM_USN_DATA` 基线仍不携带这两项元数据，因此仅依赖大小/日期的宽泛查询在完成低内存 NTFS 元数据基线前可能漏掉尚未被增量刷新过的旧条目。

## 8. 修改时间过滤

等价前缀：`date:`、`dm:`、`modified:`、`datemodified:`。

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
datemodified:today
dm:nextweek
dm:last24hours
dm:jan
dm:tuesday
```

不带比较符的 `YYYY-MM-DD` 会匹配该本地自然日；带比较符时按该本地时间点比较。当前支持以下 Everything 风格日期常量：

- `today`、`yesterday`；
- `thisweek` / `currentweek`、`thismonth` / `currentmonth`、`thisyear` / `currentyear`：从当前自然周期起点到今天结束；
- `lastweek` / `prevweek`、`lastmonth` / `prevmonth`、`lastyear` / `prevyear`：上一个完整自然周期；
- `comingweek` / `nextweek`、`comingmonth` / `nextmonth`、`comingyear` / `nextyear`：下一个完整自然周期；
- `pastweek`、`pastmonth`、`pastyear`：从当前时刻向前滚动一个单位形成下界；
- `<last|past|prev|coming|next><N><years|months|weeks|days>`，例如 `last3weeks`、`next2months`；
- `<last|past|prev|coming|next><N><hours|minutes|mins|seconds|secs>`，例如 `last24hours`、`next30mins`；
- 英文月份全名和缩写：`january`…`december`、`jan`…`dec`，匹配当前年份中的对应完整月份；
- 英文星期全名和缩写：`sunday`…`saturday`、`sun`…`sat`，匹配当前周中的对应完整自然日。

周边界读取 Windows 当前用户的“每周第一天”区域设置，不再固定为星期一。自然日、周、月和年按本地日历边界计算，因此跨夏令时的一周按 UTC 时间衡量可能是 167、168 或 169 小时。

为贴近 Everything 1.4，`last/past/prev<N>...` 以及 `pastweek` / `pastmonth` / `pastyear` 只设置滚动下界，不设置上界，所以时间戳位于未来的异常文件也可能命中；`coming/next<N>...` 使用“当前时刻到未来边界”的左闭右开区间。单数单位形式（如 `last1hour`）当前不接受，应写为 `last1hours`。

尚未实现 `datecreated:` / `dc:`、`dateaccessed:` / `da:`、`recentchange:` / `rc:`、`unknown`、完整区域化日期输入和 Everything 的全部日期范围组合。初始 MFT 基线仍可能缺少旧条目的修改时间，详见《当前状态》。

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

## 10. 结果数量限制

```text
count:25 alpha
```

`count:` 接受非负整数，并将本次查询结果数限制为该值；它不会提高 GUI 或 IPC 已设置的最大结果上限。

## 11. 查询内选项

```text
case:on
case:off
wholeword:on
wholeword:off
```

布尔值还接受 `1/0`、`yes/no`、`true/false`。GUI 中的大小写、全字、路径和变音符号开关也会进入查询请求。

## 12. 重复项

```text
dupe:name
dupe:size
dupe:name-size
```

分别按名称、大小、名称+大小筛选重复项。这只是当前支持的基础 duplicate mode，不是 Everything 全部 duplicate functions。

## 13. 组合示例

```text
file: path:work <ext:cpp|ext:h> !path:build
```

```text
folder: name:backup* dm:<2026-01-01
```

```text
case:on wholeword:on name:README ext:md
```

```text
file: size:1mb..10mb ext:jpg;png dm:today
```

```text
count:100 root:
```

## 14. 当前未兼容内容

包括但不限于：

- Everything 的其余函数、宏和预处理规则；
- 完整属性、创建时间、访问时间等函数族；
- 创建/访问/最近变化时间字段、完整日期范围、任意 N 单位相对日期以及其余单位别名；
- 完整正则、通配符和转义兼容细节；
- ADS 内容、文件内容和 Xapian 内容查询；
- ETP 查询协议兼容；
- Everything 全部重复项函数和自然排序边界。

新增或修改语法时，必须同步更新本文件、`CHANGELOG.md` 和查询单元测试。
