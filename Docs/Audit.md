# Audit

只读检查资产，产出报告 JSON，不改任何包。本文覆盖两个 commandlet 与三个配套脚本。

## Commandlet

| RunName | 检查什么 |
| --- | --- |
| `AuditLevelReference` | level 包里指向已不存在资产的引用 |
| `AuditLevelTopology` | level 之间的 streaming 关系，谁是 persistent，谁是 sublevel |

## AuditLevelReference

用途: 审计 level（`.umap`）包里的破损资产引用，也就是依赖的包在磁盘上已经不存在。典型场景是资产被改名或删除（例如分支部分同步拉进来的状态），而别的 level 还在 import 旧路径。

机制: 只读。不 load world，不保存任何包。走 Asset Registry 的依赖图，逐个依赖用 `FPackageName::DoesPackageExist` 判断。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-levels=` | 逗号分隔的 level 包路径，给了它就不看 `-scandir` |
| `-scandir=` | 要枚举 level 的内容根目录，两个都不给时默认 `/Game` |
| `-report=` | 报告 JSON 输出路径，默认在 `Intermediate/AuditLevelReference/` 下 |

### 调用

扫整个 `/Game`。Audit 组不吃 `AssetList`，位置参数留空串。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelReference "" 10 600 \
    '-scandir="/Game"'
```

只查指定的几张图，报告写到指定路径。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelReference "" 10 600 \
    '-levels="/Game/Maps/L_Foo,/Game/Maps/L_Bar" -report="C:/temp/level_ref.json"'
```

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 没有破损引用 |
| 3 | 有破损引用，明细在报告里 |
| 1 | 参数错误，或报告写失败 |
| 2 | 编辑器在运行 |

3 不是失败。运行本身是成功的，只是查出了要处理的东西，CI 靠它 gate 提交。

### 报告 JSON

顶层字段。

| 字段 | 含义 |
| --- | --- |
| `phase` | 固定 `audit-level-reference`，标识产出这份报告的运行 |
| `timestamp_utc` | 运行时刻，ISO 8601 UTC |
| `levels_scanned` | 本次扫过的 level 数 |
| `levels_with_broken_refs` | 其中含破损引用的 level 数 |
| `total_broken_refs` | 破损引用总条数 |
| `results` | 逐 level 明细，只列有破损的那些 |

`results` 每项。

| 字段 | 含义 |
| --- | --- |
| `level` | level 包路径 |
| `broken_count` | 该 level 的破损引用条数 |
| `broken_refs` | 破损依赖的包路径数组 |

## 挂载 root 约束

包是否存在按已挂载的 content root 解析。位于未挂载 root 下的依赖，例如被禁用的插件里的资产，会被判成 missing。

跑之前必须让项目的插件全部启用，否则报出来的是假破损。

## 配对操作

审计只找问题，不修。修在 Migrate 组的 `SanitizeLevelReference`，它把 level 里对旧资产的引用重指到新资产，见 [Migrate.md](Migrate.md)。

## AuditLevelTopology

用途: 按 streaming 关系给每个 level 分角色。需要驱动 streaming 的工具，性能测量与预算审计都算，必须知道该把哪个 level 当 persistent 打开。把那个名字硬编码进脚本，正是这类工具变成项目专用的原因，这个 commandlet 就是为了消掉那个硬编码。

机制: 只读。逐个 load level 包读它的 streaming levels，不保存任何东西。不需要更新组件，streaming 条目是 authored data。

### 角色

| 角色 | 判定 |
| --- | --- |
| `Standalone` | 没有 streaming levels，也不被别的 level 引用 |
| `PersistentHost` | 有 streaming levels |
| `Sublevel` | 没有 streaming levels，但被别的 level 引用 |

hosting 优先于被 hosted。一个 level 只要自己挂着 sublevel 就是 `PersistentHost`，哪怕它同时被别的 level 引用。

判据是这个 level 能不能被独立打开来驱动它的 sublevel。一个 level 被复制一份塞进别的 level 里，例如做概念验证或代理关卡，不影响它自己仍然可以独立打开测量。反过来让被引用优先，真正有内容的关卡会被判成 `Sublevel`，只有一层壳的代理关卡反而被判成 `PersistentHost`，正好判反。

嵌套关系不丢，`referenced_by` 照常记录谁把它 stream 了进去。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-levels=` | 逗号分隔的 level 包路径，给了它就不看 `-scandir` |
| `-scandir=` | 要枚举 level 的内容根目录，两个都不给时默认 `/Game` |
| `-report=` | 报告 JSON 输出路径，默认在 `Intermediate/AuditLevelTopology/` 下 |

### 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelTopology "" 10 600 \
    '-scandir="/Game"'
```

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelTopology "" 10 600 \
    '-levels="/Game/Maps/L_Foo,/Game/Maps/L_Bar" -report="C:/temp/level_topology.json"'
```

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 报告已写出 |
| 1 | 参数错误，或报告写失败 |
| 2 | 编辑器在运行 |

本 commandlet 不返回 3，拓扑没有"发现问题"的概念。

### 报告 JSON

顶层字段。

| 字段 | 含义 |
| --- | --- |
| `phase` | 固定 `audit-level-topology` |
| `timestamp_utc` | 运行时刻，ISO 8601 UTC |
| `levels_scanned` | 本次扫过的 level 数 |
| `standalone_count` | `Standalone` 数 |
| `persistent_host_count` | `PersistentHost` 数 |
| `sublevel_count` | `Sublevel` 数 |
| `results` | 逐 level 明细 |

`results` 每项。

| 字段 | 含义 |
| --- | --- |
| `level` | level 包路径 |
| `role` | `Standalone` / `PersistentHost` / `Sublevel` |
| `streaming_levels` | 该 level 挂的 sublevel 包路径数组 |
| `referenced_by` | 把它 stream 进去的 level 数组 |

扫描范围之外的 streamed level 没有对应节点可以标注 `referenced_by`，host 侧仍然会把它记进 `streaming_levels`。

## Stream metric 工作流

逐 sublevel 测加载卸载耗时，并把耗时对上规模。入口 `run_stream_metric.ps1`，一条命令跑完四步。

```bash
powershell -File Plugins/UAssetWorkbench/scripts/run_stream_metric.ps1
powershell -File Plugins/UAssetWorkbench/scripts/run_stream_metric.ps1 -PersistentLevel "/Game/Maps/L_Foo"
```

### 参数

| 参数 | 说明 |
| --- | --- |
| `-PersistentLevel` | 可选，直接指定 persistent level 的包路径，不给就从拓扑报告里推 |
| `-ScanDir` | 枚举 level 的内容根，默认 `/Game` |
| `-SkipExport` | 跳过 `LevelExport` 那一步，导出没变化时用 |
| `-TimeoutSec` | 默认 1800 |

### 流程

1. 跑 `AuditLevelTopology` 出拓扑报告。这一步总是执行，即使显式给了 `-PersistentLevel`，下一步要靠报告拿目标 level 的 sublevel 列表
2. 对目标 level 的 sublevel 跑 `LevelExport` 拿规模数据，`-SkipExport` 跳过这一步
3. 起 unattended 编辑器跑 `stream_metric_probe.py` 测耗时
4. 跑 `stream_metric_report.py` 把耗时与规模合并成表并打印

挑 persistent level 的逻辑在 ps1 里: 读拓扑报告，没给 `-PersistentLevel` 时找唯一的 `PersistentHost`，找到多个就报错并列出候选，要求显式指定。

探针把 `AlwaysLoaded` 的 sublevel 在内存里换成 `LevelStreamingDynamic`，再起 Simulate 逐 sublevel 测量。换类这一步永远不保存，`AlwaysLoaded` 的 `ShouldBeLoaded` 恒为真，不换就没法卸载。耗时写 `Saved/StreamMetric/stream_metric_result.json`，增量写入，编辑器崩了也留得住。

前提: 不能有 `UnrealEditor` 正在运行，脚本会直接报错退出。

测量项: 每个 sublevel 的 unload 墙钟毫秒、gc 毫秒、load 墙钟毫秒、最差帧毫秒。

### 环境变量契约

探针读这两个，手动设置后也可以脱离 ps1 单独跑探针。

| 变量 | 作用 |
| --- | --- |
| `UAW_PERSISTENT_LEVEL` | 直接指定 persistent level 的包路径，优先级最高 |
| `UAW_TOPOLOGY_REPORT` | `AuditLevelTopology` 报告路径，探针从里面找唯一的 `PersistentHost` |

## stream_metric_report.py

把 stream metric 的耗时与 `LevelExport` 的规模按 level 名合并，出一张能看出趋势的表。`run_stream_metric.ps1` 最后一步会自动调它，也可以单独跑。

单看毫秒数说明不了问题，跟规模对起来才知道耗时是随内容量线性长，还是被别的因素主导。

```bash
python Plugins/UAssetWorkbench/scripts/stream_metric_report.py
```

输入: `Saved/StreamMetric/stream_metric_result.json` 与 `Intermediate/UAssetExport` 下的 `LevelExport` 产物。

### 参数

| 参数 | 说明 |
| --- | --- |
| `--project` | 项目目录，默认按脚本位置往上推三级 |
| `--result` | stream metric 结果 JSON，默认按 `--project` 推导 |
| `--csv` | 额外写一份 CSV |

输出: 每个 sublevel 一行，列 actors、components、unique static meshes、load_ms、ms/1k（每千组件的加载毫秒）、unload_ms、gc_ms，按组件数排序，末行给合计。缺某个 level 的 `LevelExport` 产物时会把它列出来，提示先跑 `LevelExport`。

规模解析直接复用 `level_budget_audit.py`，没有重复实现。

## level_budget_audit.py

离线读 `LevelExport` 的 JSON，出组件预算表。

```bash
python Plugins/UAssetWorkbench/scripts/level_budget_audit.py --budget 2000
```

前置: 先跑过 `LevelExport`，这个脚本只读那些产物。

### 参数

| 参数 | 说明 |
| --- | --- |
| `--project` | 项目目录，默认按脚本位置往上推三级 |
| `--levels` | `Intermediate/UAssetExport` 下的相对 glob，默认 `Game/**/*.json` |
| `--budget` | 每个 level 的组件预算，默认 2000 |

输出: 每个 level 一行，列 actors、components、lights、spline mesh components、unique static meshes，以及组件数与预算的比值，超预算的行会标出来。
