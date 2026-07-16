# uasset-json-exporter

[English](README.md) | **中文**

![Claude Code](https://img.shields.io/badge/Claude_Code-black?style=flat&logo=anthropic&logoColor=white)
![Unreal Engine 5](https://img.shields.io/badge/Unreal_Engine-5.7-blue?logo=unrealengine&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

二进制文件没法当文本编辑，处理起来很麻烦。这个插件把 Unreal Engine 5 的资产导出成 LLM 能读懂的结构，只抽取它需要的那部分上下文。

## 它解决什么问题

通用问题：信息锁在 GUI 工具里，代码审查、AI 分析和自动化都够不到。

具体到当下的 UE 项目：大量逻辑和配置锁在二进制 `.uasset` 文件里。当你让 AI 协助调试或重构时，通常会遇到：

- **"我无法打开二进制文件"**，AI 对 `.uasset` 束手无策，只能把问题丢回给你
- **蓝图过大，难以 C++ 化**，数百个节点的 EventGraph 没有可读的文本表示，人工逐节点翻译既慢又容易遗漏
- **蓝图中的死代码和错误配置**，创建者留下的废弃变量、断开的连接、错误的默认值，肉眼在编辑器里很难全面排查
- **AnimMontage 的 Notify 时序难以追踪**，ANS 的触发时间、持续时长、参数散落在时间轴上，代码侧看不到全貌
- **Widget 布局和动画只存在于编辑器里**，UMG 的层级结构、Slot 设置、关键帧序列没有文本化的审查手段
- **特效和材质参数分散在编辑器各处**，Niagara 的 Emitter 配置、Material 的节点连接和参数覆盖，没有统一的文本审查方式
- **DataTable 和 DataAsset 的配置数据不可搜索**，几十行的数据表在编辑器里逐行翻看效率极低
- **关卡（.umap）内容完全是二进制**，Actor 布局、StaticMesh 引用、碰撞配置、Streaming Level、per-instance 覆盖等没有可读的文本视图

## 它如何工作

同一套设计，编辑器开着或关着都能用，并且把导出文本压得足够小，让 AI 很快读完。

插件遍历资产内部结构并序列化为 JSON，AI 直接读文本。只读取，不修改资产。

v1.5.0 双管线设计，wrapper 通过心跳文件 `Saved/UAssetExportQueue/.alive` 自动路由：

- **编辑器开启**：写入 `pending/<uuid>.json`，由 in-editor `UEditorSubsystem` 同进程消化，结果落 `done/<uuid>.json`，编辑器右下角弹 Slate 通知反馈进度。无需关编辑器。
- **编辑器关闭**：启动 `UnrealEditor-Cmd.exe` 走 commandlet 路径。`Main` 返回后进程常因 shader compile worker、DDC commit、模块卸载继续停留几十秒，wrapper 监控输出 JSON 的 mtime，所有文件稳定 N 秒（默认 10）后通过 `taskkill` 强制结束。

两条路径输出格式完全一致，调用方无需关心走哪条。导出 JSON 包含节点、连接、属性、默认值、时间轴标记等完整结构信息，AI 工具可以 grep + offset 按需读取。

## 可迁移性

UE 只是首个落地场景，下面三项可复用资产并不依赖它。

| 可复用资产 | 是什么 | 迁移到 |
|---|---|---|
| 模式 | 不透明二进制 -> AI 可消费结构化文本的桥接 | 任何被 GUI 锁定的私有格式（DCC、CAD/BIM、EDA、仿真） |
| 架构 | 心跳路由的自适应双管线（存活同进程 vs 无头） | 任何同时具备存活与无头模式的重量级宿主（Houdini/Maya/Blender/Revit/MATLAB） |
| 序列化纪律 | token 经济意识的导出（delta-from-archetype、cap-and-sample、grep+offset 读取契约） | 任何 LLM 数据管线的上下文工程 |

## 与 UE MCP 方案的对比

社区中存在另一类方案：UE MCP（如 `kvick-games/UnrealMCP`、`chongdashu/unreal-mcp`），通过 Remote Control / Python 打通，让 AI 在 Editor **运行时** 操作资产和场景。两类方案解决不同问题，并非互斥。

| 维度 | 本插件（Commandlet） | UE MCP（Editor 运行时） |
|---|---|---|
| 工作模式 | 离线导出 JSON，AI 读文本 | 在线 RPC，AI 直接操作 Editor |
| 调用范式 | 文件队列 RPC（subsystem 模式，理论上契合 MCP）/ 进程启动（commandlet 模式） | 在线 RPC（HTTP / Python bridge） |
| 编辑器状态 | 自适应：开启走 in-editor subsystem，关闭走 commandlet | 必须开启 |
| 资产结构读取 | 强，EdGraph/Pin/连接完整序列化 | 弱，Remote Control 拿不到 EdGraph 细节 |
| 运行时状态 | 无 | 强，可读 PIE 中的 actor、selection、live property |
| Token 成本 | 可控，grep + offset 按需读取 | 不可控，取决于 RPC 返回体 |
| 适合任务 | 静态分析、蓝图审查、批量配置核对 | 场景搭建、PIE 调试、临时调参验证 |
| 可重复性 | 高，JSON 可入版本控制做 diff | 低，依赖运行时上下文 |

**选型建议**：

- 需要让 AI 理解蓝图逻辑、追踪 Notify 时序、核对 DataTable / DataAsset 配置 → 用本插件
- 需要让 AI 在 PIE 中 spawn actor、改 live 参数、读当前选中对象 → 用 UE MCP
- 两者可以并存，commandlet 负责静态结构，MCP 负责动态状态

## 支持的 Exporter

| Commandlet | `-run=` 名称 | 导出内容 |
|---|---|---|
| `BlueprintEdGraphExportCommandlet` | `BlueprintEdGraphExport` | Blueprint 的 EdGraph 节点、Pin、连接、变量、组件、引用资产 |
| `AnimMontageExportCommandlet` | `AnimMontageExport` | Montage 的 Section、Slot、ANS/AN 位置与时长、Notify 自定义参数 |
| `WidgetLayoutExportCommandlet` | `WidgetLayoutExport` | Widget 的树形层级、Slot 布局属性、子类属性、动画关键帧、EdGraph |
| `DataAssetExportCommandlet` | `DataAssetExport` | DataAsset 子类的所有自定义属性，含数组元素展开 |
| `DataTableExportCommandlet` | `DataTableExport` | DataTable 的行结构名称、所有行数据（按 RowName 索引） |
| `NiagaraSystemExportCommandlet` | `NiagaraSystemExport` | Niagara System 的 Emitter 列表、Spawn/Update 脚本参数、Renderer 属性 |
| `MaterialExportCommandlet` | `MaterialExport` | Material 节点图（Expression 连接链）、全局设置；MaterialInstance 参数覆盖表 |
| `TextureExportCommandlet` | `TextureExport` | 通过反射导出 Texture 资产属性（压缩设置、sRGB、LOD 组、mip 设置、导入/源尺寸） |
| `BehaviorTreeExportCommandlet` | `BehaviorTreeExport` | BT 树结构（Composite/Task/Decorator/Service）、节点参数、Blackboard Keys |
| `AnimBlueprintExportCommandlet` | `AnimBlueprintExport` | AnimBP 的 EdGraph、StateMachine（States/Transitions/条件规则/Blend 设置） |
| `LevelExportCommandlet` | `LevelExport` | Level（.umap）中的 Actor / Component、delta-from-archetype 属性、碰撞 / StaticMesh / ISM 摘要、Streaming Level 配置 |

## 使用方法

### 推荐：通过 wrapper 脚本调用

```bash
bash scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/YourProject.uproject" \
    BlueprintEdGraphExport \
    "/Game/Path/BP_A,/Game/Path/BP_B"
```

Wrapper 按心跳自动路由（详见前文 [它如何工作](#它如何工作)）。

可选参数：`[IDLE_SEC] [MAX_SEC]`，默认 `10` 和 `600`。退出码 `0` 表示成功，`1` 表示输出缺失或 dispatch 失败，`2` 表示调用参数错误或与编辑器冲突。

### 原生调用

```bash
"<UE_PATH>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
    "<PROJECT_DIR>/YourProject.uproject" \
    -run=BlueprintEdGraphExport \
    -assets="/Game/Path/BP_A,/Game/Path/BP_B" \
    -nullrhi -nosplash -nosound
```

将 `-run=` 后的名称替换为你需要的 Exporter。所有 Exporter 共享相同的 `-assets=` 参数格式。

如果在 Git Bash 中运行，需要加 `MSYS_NO_PATHCONV=1` 前缀，防止 `/Game/...` 被转换为 Windows 路径。

使用原生调用时，若发现进程未自行退出，需要自行通过 `taskkill` 清理，否则会锁住 `.uproject` 文件影响后续操作。

### 故障处理

若 wrapper 的心跳路径无响应，且 fallback commandlet 进程在 wrapper `taskkill` 后仍然杀不掉，几乎可以确定是 Visual Studio 持有该进程。不要犹豫，直接强杀：

```bash
taskkill /F /IM UnrealEditor-Cmd.exe
```

必要时连同相关 Visual Studio 进程一起杀。卡死的 `UnrealEditor-Cmd.exe` 会持续锁住 `.uproject`，影响后续所有调用。

### 输出

文件输出到 `<ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json`，不会进入版本控制。

每个 JSON 文件都包含 `ExporterVersion` 和 `ExportType` 字段，标识导出器版本和资产类型。

### 输出示例

<details>
<summary>Blueprint EdGraph</summary>

```json
{
    "ExporterVersion": "1.0.0",
    "ExportType": "BlueprintEdGraph",
    "Blueprint": "BP_PlayerController",
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
    "ExporterVersion": "1.0.0",
    "ExportType": "AnimMontage",
    "MontageName": "AM_Player_DashStrike_01",
    "SequenceLength": 0.543,
    "Sections": [
        { "Name": "Default", "StartTime": 0 }
    ],
    "SlotTracks": [
        {
            "SlotName": "DefaultSlot",
            "Segments": [
                { "AnimSequence": "AS_Player_DashStrike_01", "AnimPlayRate": 2 }
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
    "ExporterVersion": "1.0.0",
    "ExportType": "WidgetLayout",
    "WidgetBlueprint": "WBP_PauseMenu",
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
    "ExporterVersion": "1.0.0",
    "ExportType": "DataTable",
    "DataTableName": "DT_PlayerAttributeInit",
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
    "ExporterVersion": "1.0.0",
    "ExportType": "Material",
    "MaterialName": "M_BatParticles",
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

MaterialInstance 导出参数覆盖表：

```json
{
    "ExportType": "MaterialInstance",
    "Parent": "M_BaseMaterial",
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
    "ExporterVersion": "1.1.0",
    "ExportType": "Level",
    "MapName": "L_Playground",
    "WorldSettings": {
        "Class": "WorldSettings",
        "DeltaProperties": {
            "DefaultGameMode": "/Script/Engine.BlueprintGeneratedClass'/Game/Core/BP_GameMode.BP_GameMode_C'"
        }
    },
    "StreamingLevels": [
        {
            "PackageName": "/Game/Maps/L_PlaygroundMetrics",
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
                    "StaticMesh": "/Game/Core/World/LevelPrototype/Meshes/SM_Cube.SM_Cube",
                    "CollisionProfile": "BlockAll",
                    "CollisionEnabled": "QueryAndPhysics",
                    "DeltaProperties": { "bUseDefaultCollision": "False" }
                }
            ]
        }
    ]
}
```

导出策略：每个 Actor / Component 只序列化 **相对其 Archetype (`UObject::GetArchetype()`) 有差异的属性**，镜像 `.umap` 自身的持久化契约，零漏失 + 最大压缩。BP 生成的 Actor 会正确对齐到 BPGC CDO，区分"BP 默认"和"实例 override"。

ISM / HISM / Foliage 组件若实例数 > 200，只导出 count + bounds + 前 5 个采样，避免单个 foliage component 爆炸文件体积。
</details>

<details>
<summary>Niagara System</summary>

```json
{
    "ExporterVersion": "1.0.0",
    "ExportType": "NiagaraSystem",
    "SystemName": "FXS_Alert",
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

### 读取策略

导出的 JSON 可能非常大（例如一个复杂的 PlayerCharacter 蓝图可以超过 8000 行）。建议：

1. 用 grep 定位你关心的节点名称、函数名或 Pin 连接
2. 按行号范围读取相关段落，不要一次性加载整个文件

## 集成到你的项目

### 方式 1：作为项目插件

1. 将 `src/` 目录下的内容复制到你项目的 `Plugins/UAssetJsonExporter/`
2. 在 `.uproject` 的 `Plugins` 数组中添加：

```json
{
    "Name": "UAssetJsonExporter",
    "Enabled": true
}
```

3. 重新生成项目文件并编译

### 方式 2：作为引擎插件

将 `src/` 目录下的内容复制到 `<UE_PATH>/Engine/Plugins/Editor/UAssetJsonExporter/`，所有项目共享。

### 前置条件

- Unreal Engine 5.7
- 项目必须已编译且包含该插件
- 通过 wrapper 调用时无需关闭编辑器；直接调原生 commandlet 仍需关闭编辑器以释放 `.uproject` 锁

## 配合 Claude Code 使用

如果你使用 [Claude Code](https://claude.ai/code) 作为 AI 协作工具，可以在项目的 `.claude/CLAUDE.md` 中添加以下段落，让 AI 知道这个工具的存在和用法：

```markdown
## Tooling: UAsset Json Exporter

Plugin: `Plugins/UAssetJsonExporter` (Editor-only)

### Available Commandlets

| Commandlet | Run Name | Description |
|---|---|---|
| BlueprintEdGraphExportCommandlet | `BlueprintEdGraphExport` | Blueprint graphs, nodes, pins, connections |
| AnimMontageExportCommandlet | `AnimMontageExport` | Montage sections, slots, ANS/AN placement and parameters |
| WidgetLayoutExportCommandlet | `WidgetLayoutExport` | Widget tree, layout, animations, EdGraph |
| DataAssetExportCommandlet | `DataAssetExport` | DataAsset subclass properties |
| DataTableExportCommandlet | `DataTableExport` | DataTable row struct and all row data |
| NiagaraSystemExportCommandlet | `NiagaraSystemExport` | Niagara emitters, scripts, renderers |
| MaterialExportCommandlet | `MaterialExport` | Material expressions and connections; MI parameter overrides |
| TextureExportCommandlet | `TextureExport` | 通过反射导出 Texture 资产属性（压缩设置、sRGB、LOD 组、mip、源尺寸） |
| BehaviorTreeExportCommandlet | `BehaviorTreeExport` | BT tree structure, node parameters, Blackboard keys |
| AnimBlueprintExportCommandlet | `AnimBlueprintExport` | AnimBP EdGraph, StateMachines (states, transitions, blend settings) |
| LevelExportCommandlet | `LevelExport` | Level (.umap) actors / components, delta-from-archetype properties, collision / static mesh / ISM summary, streaming levels |

### Usage

bash Plugins/UAssetJsonExporter/scripts/run_commandlet.sh "<UE_PATH>" "Project.uproject" <RunName> "/Game/Path/Asset"

Wrapper routes automatically by `Saved/UAssetExportQueue/.alive` heartbeat: editor open → in-editor subsystem (Slate toast feedback), editor closed → UnrealEditor-Cmd commandlet. Output format is identical either way.

### Output

`Intermediate/UAssetExport/<AssetPath>.json`

### Reading Strategy

Do NOT read the entire file at once. Instead:
1. Use Grep to locate relevant node titles, function names, or pin connections.
2. Use Read with offset/limit to inspect only the relevant sections.

### When to Use

- A task references a Blueprint/Widget and its internal logic is relevant
- A bug may be in Blueprint wiring rather than C++
- Need to verify variable defaults, component setup, or event flow
- Need to check AnimMontage notify timing, DataAsset configuration, or material setup
- Need to inspect Niagara emitter parameters or DataTable values
- Need to understand BehaviorTree logic flow or AnimBP state machine transitions
- Need to audit a Level: actor placements, static mesh / collision setup, streaming level config, per-instance overrides
```

AI 会在相关任务中自动调用 Commandlet 导出并分析资产内容。

## 版本

当前版本：**v1.5.0**

版本号定义在 `src/Source/UAssetJsonExporter/Public/UAssetJsonExporterVersion.h`，同时嵌入在每个导出 JSON 的 `ExporterVersion` 字段中。

## License

[MIT](LICENSE) - Hyrex Chia
