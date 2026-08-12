# Export

读 uasset 结构，导出成可 grep 的 JSON。

## Commandlet

| RunName | 导出内容 |
| --- | --- |
| `BlueprintEdGraphExport` | Blueprint 图、节点、pin、连线 |
| `AnimMontageExport` | Montage section、slot、ANS/AN 位置与参数 |
| `WidgetLayoutExport` | Widget 树、布局、动画、EdGraph |
| `DataAssetExport` | DataAsset 子类属性 |
| `DataTableExport` | DataTable 行结构与全部行数据 |
| `NiagaraSystemExport` | Niagara emitter、script、renderer |
| `MaterialExport` | Material 表达式与连线，MI 的参数覆写 |
| `TextureExport` | Texture 属性，压缩、sRGB、LOD group、mip、源尺寸 |
| `BehaviorTreeExport` | BT 树结构、节点参数、Blackboard key |
| `AnimBlueprintExport` | AnimBP EdGraph、状态机的状态、转换、blend 设置 |
| `LevelExport` | Level 的 actor / component、与 archetype 的差异属性、碰撞与静态网格与 ISM 摘要、streaming level |

## 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutExport \
    "/Game/UI/WBP_Bar,/Game/UI/WBP_Baz"
```

一次可以传多个资产，逗号分隔，同一次运行共用一个 RunName。

## 输出

路径: `Intermediate/UAssetExport/<AssetPath>.json`
版本控制: 不入库

每份 JSON 都带这两个通用字段，其余按资产类型展开。

| 字段 | 含义 |
| --- | --- |
| `ExporterVersion` | 产出这份 JSON 的插件版本 |
| `ExportType` | 资产类型标识，决定后面的结构怎么读 |

完成判定看输出文件: 全部存在，且 mtime 稳定 `IDLE_SEC` 秒。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部输出就位 |
| 1 | 输出缺失或执行失败，wrapper 打印 log 尾部 |
| 2 | 编辑器在运行 |

Export 组不会返回 3，3 是 Audit 组专用。

## 读取策略

导出的 JSON 可能非常大，一个中等复杂度的 Blueprint 就能到几千行。

1. 先 grep 定位关心的节点标题、函数名、控件名、行名
2. 拿到行号后按区间读，不要整份读进上下文

```bash
grep -n "OnHealthChanged" Intermediate/UAssetExport/Game/Blueprints/BP_Foo.json
```
