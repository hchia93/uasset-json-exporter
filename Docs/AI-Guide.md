# AI Guide

给 agent 用的调用手册，覆盖日常调用。

本手册是参考。实际调用行为与这里的描述对不上时，以源码为准: 读 `Source/UAssetWorkbench/` 下对应组目录的 commandlet header，每份 header 顶部的块注释写了该 commandlet 完整的参数与退出码契约。

## 决策表

| 我要知道 / 我要做什么 | RunName | 组 |
| --- | --- | --- |
| 看 Blueprint 里的连线逻辑、节点、函数 | `BlueprintEdGraphExport` | Export |
| 看 Widget 的布局、控件树、动画 | `WidgetLayoutExport` | Export |
| 看 Montage 的 section、slot、notify 时间点 | `AnimMontageExport` | Export |
| 看 DataTable 的行结构与数值 | `DataTableExport` | Export |
| 看 DataAsset 上的属性配置 | `DataAssetExport` | Export |
| 看关卡里摆了什么、碰撞与静态网格配置 | `LevelExport` | Export |
| 看 AnimBP 的状态机与转换条件 | `AnimBlueprintExport` | Export |
| 看 BT 的树结构、节点参数、Blackboard key | `BehaviorTreeExport` | Export |
| 看材质表达式或 MI 的参数覆写 | `MaterialExport` | Export |
| 看 Niagara 的 emitter、script、renderer | `NiagaraSystemExport` | Export |
| 看贴图的压缩、sRGB、LOD group、源尺寸 | `TextureExport` | Export |
| 用代码搭 Widget 布局，或按 spec 重建控件树 | `WidgetLayoutImport` | Import |
| C++ 改名后 BP 事件不再触发 | `RedirectBlueprintEvent` | Migrate |
| delegate 参数改名后绑定处留下悬空连线 | `RedirectBlueprintPin` | Migrate |
| 批量改 Blueprint 的父类 | `ReparentBlueprint` | Migrate |
| 让 CoreRedirect 解析过的引用落盘，好撤掉那条 redirect | `ResaveAsset` | Migrate |
| 资产改名后，把别的 level 的 import 改指到新资产 | `SanitizeLevelReference` | Migrate |
| 想知道有没有 level 引用了已经不存在的资产 | `AuditLevelReference` | Audit |
| 提交前 gate 一次破损引用检查 | `AuditLevelReference` | Audit |
| 想知道哪个 level 是 persistent，哪些是 sublevel | `AuditLevelTopology` | Audit |
| 想测每个 sublevel 的加载卸载耗时 | `run_stream_metric.ps1` | 脚本 |
| 想看每个 level 的组件数是否超预算 | 先 `LevelExport`，再 `level_budget_audit.py` | Export + 脚本 |

拿不准 Blueprint 里的问题出在 C++ 还是图上时，先 `BlueprintEdGraphExport` 看图，再决定。

破损引用先 audit 后 sanitize: `AuditLevelReference` 拿到明细，再用 `SanitizeLevelReference` 逐对重指向。

## 调用模板

Export。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    BlueprintEdGraphExport \
    "/Game/Blueprints/BP_Foo,/Game/Blueprints/BP_Baz"
```

Import。目标资产路径放 `AssetList`，spec 走 `EXTRA_ARGS`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutImport \
    "/Game/UI/WBP_Bar" \
    10 600 \
    '-spec="C:/temp/WBP_Bar.spec.json"'
```

Migrate。两个 redirect 默认 dry run，确认输出无误后补 `-apply`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintEvent \
    "/Game/Blueprints/BP_Foo" \
    10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OldName" -NewEvent="NewName"'
```

Audit。不吃 `AssetList`，位置参数留空串。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelReference \
    "" \
    10 600 \
    '-scandir="/Game" -report="C:/temp/level_ref.json"'
```

## 结果怎么读

Export。

输出: `Intermediate/UAssetExport/<AssetPath>.json`
先 grep 定位关心的节点标题、函数名、控件名，再按行号区间读。

```bash
grep -n "MyFunctionName" Intermediate/UAssetExport/Game/Blueprints/BP_Foo.json
```

Import 与 Migrate。

判定依据: wrapper 退出码
细节: 编辑器开着看 Message Log 的 `UAsset Workbench` listing，编辑器关着看 `Saved/Logs/` 与 `Saved/UAssetExportQueue/last_commandlet.log`

Audit。

判定依据: 退出码 0 干净，3 有问题
明细: 报告 JSON，路径由 `-report` 决定，默认在 `Intermediate/<RunName>/` 下

## 坑

| 坑 | 处理 |
| --- | --- |
| 编辑器开着时直接起 commandlet | 一律走 wrapper。commandlet 检测到活的 heartbeat 会退出码 2 自保 |
| 导出的 JSON 可能上万行 | 先 grep 定位再按行号区间读，不要整份读进上下文 |
| `RedirectBlueprintEvent` 与 `RedirectBlueprintPin` 默认不写盘 | 先读 dry run 输出，确认命中的资产与节点，再加 `-apply` |
| `WidgetLayoutImport` 是整树替换 | spec 必须描述完整的树，不是增量补丁 |
| 导出产物不入版本控制 | `Intermediate/UAssetExport` 是临时目录，需要留证据就自行拷走 |
| 把 Audit 的退出码 3 当成失败 | 3 不是失败，运行本身成功，只是报告里有要处理的东西。跑不起来才是 1 |
| `AuditLevelReference` 报出一大堆破损 | 先确认项目的插件全部启用。未挂载 content root 下的依赖会被判成 missing，那是假破损 |
| `SanitizeLevelReference` 在旧资产删掉之后才跑 | 来不及了，换指针需要新旧两边都还在磁盘上。必须在删除旧资产之前跑 |
| 分不清 log 属于哪一组 | `LogUAssetWorkbenchExporter` / `LogUAssetWorkbenchImporter` / `LogUAssetWorkbenchMigrator` / `LogUAssetWorkbenchAuditor` 各对应一组，调度与公共部分在 `LogUAssetWorkbenchCore` |
| 编辑器开着就跑 stream metric | 跑之前编辑器必须关闭，脚本会直接报错退出 |
| 担心探针改 streaming class 污染 level | 只在内存里换成 `LevelStreamingDynamic`，不落盘 |
| `level_budget_audit.py` 结论对不上现状 | 它只读 `LevelExport` 的产物，产物过期就得到过期结论。脚本会打印导出日期，先看那个 |
