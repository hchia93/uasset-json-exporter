# uasset-workbench

[English](README.md) | **中文**

![Claude Code](https://img.shields.io/badge/Claude_Code-black?style=flat&logo=anthropic&logoColor=white)
![Unreal Engine 5](https://img.shields.io/badge/Unreal_Engine-5.7-blue?logo=unrealengine&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

让脚本与 AI agent 能对 Unreal Engine 5 的 uasset 做非交互操作的编辑器插件。

## 它解决什么问题

| 问题 | 具体表现 |
| --- | --- |
| 读不了 | 逻辑与配置锁在二进制 `.uasset` 里，AI 拿到文件也无从下手。上百节点的 EventGraph 没有可读的文本形态，蓝图里的废弃变量、断掉的连线、错误的默认值靠肉眼在编辑器里审不完。Montage 的 notify 时间点、UMG 的层级与关键帧、Niagara 与材质参数、DataTable 数值、level 的 actor 摆放与 streaming 配置，全都只在编辑器界面里存在 |
| 改不了 | 要改 UMG 布局只能在编辑器里手工拖，没有可版本控制、可重放的写入路径 |
| 改名后修不了 | C++ 或资产改名后，CoreRedirects 只能修调用侧，修不了 Blueprint 图里的实现侧与消费侧，也修不了别的 level 对旧路径的 import |
| 审计不了 | 破损引用、level 的 streaming 拓扑、每个 level 的组件预算，没有批量查询的入口 |

四类问题对应四组能力。

## 四组能力

| 组 | 做什么 | 产出 | 数量 |
| --- | --- | --- | --- |
| Export | 读 uasset 结构导出 JSON | `Intermediate/UAssetExport` 下的 JSON | 11 |
| Import | 读 JSON spec 写回 uasset | 被修改的 uasset | 1 |
| Migrate | C++ 或资产改名后修复引用 | 被修改的 uasset | 5 |
| Audit | 只读检查产出报告 | 报告 JSON | 2 |

组别由 run 名决定: 后缀 `Export` 是 Export 组，后缀 `Import` 是 Import 组，前缀 `Audit` 是 Audit 组，其余是 Migrate 组。

## 通道与架构

所有操作收敛成同一套调用契约。heartbeat 文件 `Saved/UAssetExportQueue/.alive` 决定路由。

| 编辑器状态 | 走哪条路 | 反馈 |
| --- | --- | --- |
| 开着 | 写 pending task，编辑器内的 subsystem 进程内执行 | Export 弹右下角 toast，其余进 Message Log |
| 关着 | 起 `UnrealEditor-Cmd` 跑 commandlet | log |

两条路产出一致，调用方不需要关心编辑器开没开。

这套路由的意义: 四组能力共享同一个调用入口和同一套产出约定，工作流可以按组合拼装，导出结构，离线分析，写回资产，审计验证，而不是每加一个工具就多一种调用方式。加一个 commandlet 就是加一个能力，契约不变。

## 快速开始

Wrapper: `src/scripts/run_commandlet.sh`，集成进项目后位于 `Plugins/UAssetWorkbench/scripts/`。

```
run_commandlet.sh <UE_PATH> <UPROJECT> <RunName> <AssetList> [IDLE_SEC] [MAX_SEC] [EXTRA_ARGS]
```

| 参数 | 说明 |
| --- | --- |
| `UE_PATH` | 引擎安装根目录 |
| `UPROJECT` | `.uproject` 的绝对路径 |
| `RunName` | commandlet 的 run 名 |
| `AssetList` | 逗号分隔的资产路径，Audit 组不吃它，传空串 |
| `IDLE_SEC` | Export 组判定完成用的输出 mtime 静默秒数，默认 10 |
| `MAX_SEC` | 总等待上限，默认 600 |
| `EXTRA_ARGS` | 透传给 commandlet 的参数 |

Git Bash 下要加 `MSYS_NO_PATHCONV=1` 前缀，否则 `/Game/...` 会被改写成 Windows 路径。

Export。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    BlueprintEdGraphExport "/Game/Blueprints/BP_Foo"
```

Import。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutImport "/Game/UI/WBP_Foo" 10 600 \
    '-spec="C:/temp/WBP_Foo.spec.json"'
```

Migrate。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintEvent "/Game/Blueprints/BP_Foo" 10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OnPickedUp" -NewEvent="HandlePickedUp"'
```

Audit。

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelReference "" 10 600 \
    '-scandir="/Game"'
```

## 各组详解

<details>
<summary><b>Export</b>，11 个 commandlet</summary>

| RunName | 导出内容 |
| --- | --- |
| `BlueprintEdGraphExport` | Blueprint 图、节点、pin、连线、变量、组件、引用资产 |
| `AnimMontageExport` | Montage section、slot、ANS/AN 位置与时长、notify 自定义参数 |
| `WidgetLayoutExport` | Widget 树、slot 布局属性、子类属性、动画关键帧、EdGraph |
| `DataAssetExport` | DataAsset 子类的全部自定义属性，数组元素展开 |
| `DataTableExport` | DataTable 行结构名与全部行数据，按 RowName 索引 |
| `NiagaraSystemExport` | Niagara emitter 列表、spawn/update script 参数、renderer 属性 |
| `MaterialExport` | Material 表达式连线链与全局设置，MaterialInstance 的参数覆写 |
| `TextureExport` | Texture 属性，压缩、sRGB、LOD group、mip、源尺寸 |
| `BehaviorTreeExport` | BT 树结构、节点参数、Blackboard key |
| `AnimBlueprintExport` | AnimBP EdGraph、状态机的状态、转换、条件、blend 设置 |
| `LevelExport` | Level 的 actor / component、与 archetype 的差异属性、碰撞与静态网格与 ISM 摘要、streaming level |

输出: `Intermediate/UAssetExport/<AssetPath>.json`，不入版本控制。

每份 JSON 都带 `ExporterVersion` 与 `ExportType` 两个通用字段，其余按资产类型展开。

<details>
<summary>Blueprint EdGraph</summary>

```json
{
    "ExporterVersion": "2.0.0",
    "ExportType": "BlueprintEdGraph",
    "Blueprint": "BP_Foo",
    "ParentClass": "PlayerController",
    "Variables": [
        { "Name": "DebugTimerHandle", "Type": "FTimerHandle" }
    ],
    "Graphs": [
        {
            "GraphType": "EventGraph",
            "Nodes": [
                {
                    "Class": "K2Node_CallFunction",
                    "Title": "Open Level (by Object Reference)",
                    "FunctionName": "OpenLevelBySoftObjectPtr",
                    "Pins": [ ... ]
                }
            ]
        }
    ]
}
```
</details>

<details>
<summary>AnimMontage</summary>

```json
{
    "ExporterVersion": "2.0.0",
    "ExportType": "AnimMontage",
    "MontageName": "AM_Foo_Attack_01",
    "SequenceLength": 0.543,
    "Sections": [
        { "Name": "Default", "StartTime": 0 }
    ],
    "SlotTracks": [
        {
            "SlotName": "DefaultSlot",
            "Segments": [
                { "AnimSequence": "AS_Foo_Attack_01", "AnimPlayRate": 2 }
            ]
        }
    ],
    "Notifies": [
        {
            "NotifyName": "ANS_Example",
            "TriggerTime": 0.0001,
            "Duration": 0.122,
            "IsState": true,
            "NotifyClass": "AnimNotifyState_Example",
            "Parameters": {
                "Speed": "2000.000000",
                "Curve": "/Script/Engine.CurveFloat'.../Falloff.Falloff'"
            }
        }
    ]
}
```
</details>

<details>
<summary>Widget Layout</summary>

```json
{
    "ExporterVersion": "2.0.0",
    "ExportType": "WidgetLayout",
    "WidgetBlueprint": "WBP_Foo",
    "WidgetTree": {
        "Name": "CanvasPanel_36",
        "Class": "CanvasPanel",
        "Visibility": "SelfHitTestInvisible",
        "Children": [
            {
                "Name": "Tint",
                "Class": "Image",
                "Properties": {
                    "Brush": "(TintColor=...)"
                },
                "Slot": {
                    "SlotClass": "CanvasPanelSlot",
                    "LayoutData": "(Anchors=(Minimum=(X=0,Y=0),Maximum=(X=1,Y=1)))"
                }
            }
        ]
    },
    "Animations": [ ... ],
    "Graphs": [ ... ]
}
```
</details>

<details>
<summary>DataTable</summary>

```json
{
    "ExporterVersion": "2.0.0",
    "ExportType": "DataTable",
    "DataTableName": "DT_Foo",
    "RowStruct": "AttributeMetaData",
    "RowCount": 5,
    "Rows": {
        "AttributeSet.Health": {
            "BaseValue": "100.000000",
            "MinValue": "0.000000",
            "MaxValue": "1.000000",
            "bCanStack": "False"
        },
        "AttributeSet.Mana": {
            "BaseValue": "0.000000",
            "MinValue": "0.000000",
            "MaxValue": "1.000000"
        }
    }
}
```
</details>

<details>
<summary>Material</summary>

```json
{
    "ExporterVersion": "2.0.0",
    "ExportType": "Material",
    "MaterialName": "M_Foo",
    "ShadingModel": "MSM_DefaultLit",
    "BlendMode": "BLEND_Translucent",
    "TwoSided": false,
    "MaterialDomain": "MD_Surface",
    "Expressions": [
        {
            "Class": "MaterialExpressionMaterialFunctionCall",
            "Description": "MaterialFunctionCall (FlipBook)",
            "Inputs": [
                {
                    "InputName": "Animation Phase (0-1) (S)",
                    "ConnectedTo": "MaterialExpressionFrac_0"
                }
            ]
        }
    ],
    "OutputConnections": {
        "EmissiveColor": { "Expression": "MaterialExpressionMultiply_0" },
        "Opacity": { "Expression": "MaterialExpressionMultiply_1" }
    }
}
```

MaterialInstance 导出参数覆写表。

```json
{
    "ExportType": "MaterialInstance",
    "Parent": "M_Base",
    "ScalarParameters": [
        { "Name": "Roughness", "Value": 0.8 }
    ],
    "VectorParameters": [
        { "Name": "BaseColor", "Value": "(R=0.5,G=0.1,B=0.1,A=1.0)" }
    ],
    "TextureParameters": [
        { "Name": "Albedo", "Texture": "T_Stone_D", "TexturePath": "/Game/Textures/T_Stone_D" }
    ]
}
```
</details>

<details>
<summary>Level</summary>

```json
{
    "ExporterVersion": "2.0.0",
    "ExportType": "Level",
    "LevelName": "L_Foo",
    "WorldSettings": {
        "Class": "WorldSettings",
        "DeltaProperties": {
            "DefaultGameMode": "/Script/Engine.BlueprintGeneratedClass'/Game/Core/BP_GameMode.BP_GameMode_C'"
        }
    },
    "StreamingLevels": [
        {
            "PackageName": "/Game/Maps/L_FooProps",
            "Class": "LevelStreamingAlwaysLoaded",
            "ShouldBeLoaded": true,
            "ShouldBeVisible": true
        }
    ],
    "ActorCount": 181,
    "Actors": [
        {
            "Name": "StaticMeshActor_2",
            "Label": "SM_Cube",
            "Class": "/Script/Engine.StaticMeshActor",
            "Transform": { "Loc": "(-9740.000,-4450.000,-0.100)", "Scale": "(184.750,210.000,1.000)" },
            "Components": [
                {
                    "Name": "StaticMeshComponent0",
                    "Class": "/Script/Engine.StaticMeshComponent",
                    "Mobility": "Static",
                    "StaticMesh": "/Game/Meshes/SM_Cube.SM_Cube",
                    "CollisionProfile": "BlockAll",
                    "CollisionEnabled": "QueryAndPhysics",
                    "DeltaProperties": { "bUseDefaultCollision": "False" }
                }
            ]
        }
    ]
}
```

导出策略: 每个 actor / component 只序列化与自身 archetype（`UObject::GetArchetype()`）不同的属性，跟 `.umap` 本身的持久化方式一致，无损且压缩率最高。蓝图生成的 actor 会正确对齐到 BPGC CDO，蓝图默认值与实例覆写因此仍然可以区分。

ISM / HISM / Foliage 组件的实例数超过 200 时只导出数量、包围盒与前 5 个采样，避免单个植被组件撑爆文件。
</details>

<details>
<summary>Niagara System</summary>

```json
{
    "ExporterVersion": "2.0.0",
    "ExportType": "NiagaraSystem",
    "SystemName": "NS_Foo",
    "ExposedParameters": [],
    "Emitters": [
        {
            "Name": "DirectionalBurst",
            "Enabled": true,
            "SimTarget": "CPU",
            "SpawnScript": {
                "Parameters": [
                    { "Name": "DirectionalBurst.SpawnRate", "Type": "NiagaraFloat" }
                ]
            },
            "Renderers": [
                {
                    "RendererClass": "NiagaraSpriteRendererProperties",
                    "Properties": { ... }
                }
            ]
        }
    ]
}
```
</details>

</details>

<details>
<summary><b>Import</b>，1 个 commandlet</summary>

`WidgetLayoutImport`，读 JSON spec 把控件树写回 Widget Blueprint，spec 路径走 `-spec=`。

spec 顶层两个字段。

| 字段 | 含义 |
| --- | --- |
| `AssetPath` | 目标 Widget Blueprint 的资产路径 |
| `WidgetTree` | 根节点，往下递归 |

节点字段。

| 字段 | 含义 |
| --- | --- |
| `Class` | 裸 UMG 类名如 `HorizontalBox`，或 Blueprint 类的完整对象路径如 `/Game/UI/WBP_Bar.WBP_Bar_C` |
| `Name` | 控件名，C++ 的 `BindWidget` 靠它匹配 |
| `Properties` | 属性名到字符串值，走反射 `ImportText` |
| `Slot` | 该节点在父容器里的 slot 属性，同样走 `ImportText` |
| `Children` | 子节点数组，父节点必须是 panel 类型 |

属性值的字符串形态就是 Export 导出的那种，例 `(Value=1.000000,SizeRule=Fill)`、`(Right=48.000000)`、`HAlign_Fill`。

行为是整树替换，不是增量合并，spec 必须描述完整的树。`WidgetLayoutExport` 的产物可以直接当 Import 的输入，改布局的常规做法是先 Export 拿到当前树，改 JSON，再 Import 写回。

```json
{
  "AssetPath": "/Game/UI/WBP_Foo",
  "WidgetTree": {
    "Class": "HorizontalBox",
    "Name": "RootBox",
    "Children": [
      {
        "Class": "SizeBox",
        "Name": "IconBox",
        "Properties": {
          "bOverride_WidthOverride": "True",
          "WidthOverride": "64.000000"
        },
        "Slot": {
          "Padding": "(Right=48.000000)",
          "VerticalAlignment": "VAlign_Center"
        },
        "Children": [
          {
            "Class": "Image",
            "Name": "Icon",
            "Properties": {
              "ColorAndOpacity": "(R=1.000000,G=1.000000,B=1.000000,A=1.000000)"
            }
          }
        ]
      },
      {
        "Class": "TextBlock",
        "Name": "Label",
        "Properties": {
          "Text": "INVTEXT(\"Label\")"
        },
        "Slot": {
          "Size": "(Value=1.000000,SizeRule=Fill)",
          "HorizontalAlignment": "HAlign_Fill"
        }
      }
    ]
  }
}
```

</details>

<details>
<summary><b>Migrate</b>，5 个 commandlet</summary>

CoreRedirects 只覆盖调用侧，Blueprint 图里的实现侧与消费侧不在它的射程内。改名的 interface event 会让 BP override 退化成孤立的 custom event，事件不再触发；改名的 delegate 参数会在绑定节点上留下悬空 pin。两类都编译得过去，靠人眼在大项目里扫不出来。

| RunName | 做什么 | 默认行为 |
| --- | --- | --- |
| `RedirectBlueprintEvent` | 把退化成 custom event 的 BP override 重新接回新事件，连线一并搬过去，能识别 UE 加的 `_N` 去重后缀 | dry run |
| `RedirectBlueprintPin` | 把绑定节点的连线从旧输出 pin 移到新 pin，再重建节点丢掉旧 pin，只处理同时带有新旧两个 pin 的节点 | dry run |
| `ReparentBlueprint` | 改 Blueprint 的父类 | 直接落盘 |
| `ResaveAsset` | 强制 load、compile、save，让 load 期的 fixup 落盘，之后就能撤掉那条 CoreRedirect，支持 Blueprint 与 map | 直接落盘 |
| `SanitizeLevelReference` | 把 level 里对旧资产的每一处引用换成新资产，然后 resave 这个 level | 直接落盘，`-dryrun` 只统计 |

两个 redirect 默认只扫描，逐条列出命中的 Blueprint、事件或节点、以及会搬多少组连线，确认无误再补 `-apply` 编译并保存。扫描一条都没命中时会给 warning，先核对 `-OwnerClass` 与旧名拼写。

`SanitizeLevelReference` 必须在删除旧资产之前跑。换指针需要新旧两边都还在磁盘上，删完再跑就来不及了。

执行后被改动的 uasset 会出现在版本控制的工作副本里。

</details>

<details>
<summary><b>Audit</b>，2 个 commandlet 与 3 个脚本</summary>

| RunName | 检查什么 |
| --- | --- |
| `AuditLevelReference` | level 包里指向已不存在资产的引用 |
| `AuditLevelTopology` | level 之间的 streaming 关系，谁是 persistent，谁是 sublevel |

`AuditLevelReference` 走 Asset Registry 的依赖图逐个判断包是否存在，不 load world，不保存任何包。包是否存在按已挂载的 content root 解析，所以跑之前必须让项目的插件全部启用，否则未挂载 root 下的依赖会被判成假破损。它的配对操作是 Migrate 组的 `SanitizeLevelReference`，audit 找出破损，sanitize 修。

`AuditLevelTopology` 给每个 level 分角色。

| 角色 | 判定 |
| --- | --- |
| `Standalone` | 没有 streaming levels，也不被别的 level 引用 |
| `PersistentHost` | 有 streaming levels |
| `Sublevel` | 没有 streaming levels，但被别的 level 引用 |

hosting 优先于被 hosted，一个 level 只要自己挂着 sublevel 就是 `PersistentHost`，哪怕它同时被别的 level 引用。判据是它能不能被独立打开来驱动自己的 sublevel，被复制一份塞进别的 level 里不影响这一点，嵌套关系由 `referenced_by` 照常记录。需要驱动 streaming 的工具靠这份报告找 persistent level，不必再把名字硬编码进脚本。

配套脚本三个。

| 脚本 | 做什么 |
| --- | --- |
| `run_stream_metric.ps1` | stream metric 的编排入口，一条命令跑完拓扑、导出、探针、出表 |
| `stream_metric_report.py` | 把探针耗时与 `LevelExport` 的规模按 level 名合并成表，附每千组件的加载毫秒 |
| `level_budget_audit.py` | 离线读 `LevelExport` 的产物出组件预算表，每个 level 一行，标出超预算的 |

`run_stream_metric.ps1` 一条命令跑完四步。

1. 跑 `AuditLevelTopology` 出拓扑报告，这一步总是执行，目标的 sublevel 列表要从报告里拿
2. 对那些 sublevel 跑 `LevelExport` 拿规模数据，`-SkipExport` 可跳过
3. 起 unattended 编辑器逐 sublevel 测 unload / gc / load 墙钟毫秒与最差帧
4. 跑 `stream_metric_report.py` 把耗时与规模合并成表打印

没给 `-PersistentLevel` 时脚本从拓扑报告里找唯一的 `PersistentHost`，找到多个就列出候选要求显式指定。探针把 `AlwaysLoaded` 的 sublevel 在内存里换成 `LevelStreamingDynamic` 才能卸载，这一步永远不保存。跑之前编辑器必须关闭。

</details>

## 读取策略

导出的 JSON 可能非常大，一个中等复杂度的 Blueprint 就能到几千行。

1. 先 grep 定位关心的节点标题、函数名、控件名、行名
2. 拿到行号后按区间读，不要整份读进上下文

```bash
grep -n "OnHealthChanged" Intermediate/UAssetExport/Game/Blueprints/BP_Foo.json
```

## 为什么不依赖官方工具链

官方给 AI 与自动化的入口，MCP、Remote Control、Python API，随引擎版本演进，能力有空窗，某些编辑器子系统在特定版本会出现回归。跨版本不能假设它们稳定，而 UE6 还有一段距离，中间的版本要照常干活。

workbench 走 commandlet 加引擎稳定 API，绕开正在演进的那一层。产出是 JSON 文件，可版本控制、可 diff、可重放。需要官方没有提供的机制时，这里就是扩展点，加一个 commandlet 就是加一个能力，调用契约不变。

在线交互式方案，也就是在编辑器运行时驱动的那一类，解决的是另一类问题，场景搭建、PIE 调试、即时参数微调。两者不互斥。

## 集成到你的项目

把 `src/` 的内容复制到项目的 `Plugins/UAssetWorkbench/`，在 `.uproject` 的 `Plugins` 数组加。

```json
{
    "Name": "UAssetWorkbench",
    "Enabled": true
}
```

重新生成工程文件并编译。

也可以放 `<UE_PATH>/Engine/Plugins/Editor/UAssetWorkbench/` 让所有项目共享。

前置: Unreal Engine 5.7，插件必须随项目编译。走 wrapper 调用不需要关编辑器，直接起 commandlet 才需要。

## 给 AI agent 用

| 文档 | 内容 |
| --- | --- |
| `Docs/AI-Guide.md` | 给 AI agent 的调用手册，决策表加调用模板加常见坑 |
| `Docs/Export.md` | Export 组明细 |
| `Docs/Import.md` | Import 组明细与 spec 格式 |
| `Docs/Migrate.md` | Migrate 组明细 |
| `Docs/Audit.md` | Audit 组明细与 stream metric 工作流 |

这些文档是给 agent 读的参考。调用行为与文档描述不符时以源码为准，每个 commandlet header 顶部的块注释写了完整契约。

## 泛化

UE 只是验证场，三样可复用的东西不依赖它。

| 可复用的东西 | 是什么 | 可迁移到 |
| --- | --- | --- |
| 模式 | 不透明二进制与 AI 可读结构化文本之间的双向桥 | 任何被 GUI 锁住的专有格式，DCC / CAD / BIM / EDA / 仿真 |
| 架构 | heartbeat 路由的自适应双管线，live 进程内与 headless 两条路产出一致 | 任何同时有交互模式与无头模式的重型宿主，Houdini / Maya / Blender / Revit / MATLAB |
| 序列化纪律 | 顾及 token 成本的导出，与 archetype 求差、超量采样截断、grep 加区间读的读取契约 | 任何 LLM 数据管线的上下文工程 |

## 版本

当前版本: **2.0.0**

定义在 `src/Source/UAssetWorkbench/Public/UAssetWorkbenchVersion.h`，同时嵌进每份导出 JSON 的 `ExporterVersion` 字段。

## License

[MIT](LICENSE) - Hyrex Chia
