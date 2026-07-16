# uasset-json-exporter

**English** | [中文](README_CN.md)

![Claude Code](https://img.shields.io/badge/Claude_Code-black?style=flat&logo=anthropic&logoColor=white)
![Unreal Engine 5](https://img.shields.io/badge/Unreal_Engine-5.7-blue?logo=unrealengine&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

Binary files can't be edited as text and are therefore hard to handle. This plugin exports Unreal Engine 5 assets into a structure an LLM can read, extracting just the context it needs.

## What problem it solves

The general problem: the information lives inside an app's interface, out of reach of code review, AI analysis, and automation.

Where this shows up in a UE project today: a lot of logic and configuration is locked inside binary `.uasset` files. When you ask an AI to help debug or refactor, you typically hit these walls:

- **"I can't open binary files"**: the AI cannot do anything with `.uasset` and kicks the problem back to you
- **Blueprint is too large to C++-ify**: an EventGraph with hundreds of nodes has no readable text form, hand-translating node by node is slow and error-prone
- **Dead code and misconfigurations inside blueprints**: abandoned variables, broken connections, wrong defaults left by the original creator are hard to fully audit by eye in the editor
- **AnimMontage notify timing is hard to trace**: ANS trigger time, duration, parameters are scattered along the timeline and invisible from the code side
- **Widget layout and animation live only in the editor**: UMG hierarchy, slot settings, keyframe sequences have no textual review surface
- **VFX and material parameters are scattered across the editor**: Niagara emitter config, material node connections and parameter overrides have no unified textual review
- **DataTable and DataAsset configuration is not searchable**: scrolling through a several-dozen-row data table in the editor is miserable
- **Level (.umap) content is entirely binary**: actor layout, static mesh references, collision config, streaming levels, per-instance overrides have no readable text view

## How it works

The same design works whether or not the host tool is open, and it keeps the exported text small enough for an AI to read quickly.

The plugin walks each asset's internal structure and serializes it to JSON, which the AI reads as text. It is read-only and never modifies assets.

v1.5.0 ships a two-pipeline design. The wrapper checks the heartbeat file `Saved/UAssetExportQueue/.alive` and routes automatically:

- **Editor open**: writes `pending/<uuid>.json`; an in-editor `UEditorSubsystem` consumes it in-process, drops the result in `done/<uuid>.json`, and surfaces a Slate toast in the editor. No need to close the editor.
- **Editor closed**: launches `UnrealEditor-Cmd.exe` for the commandlet path. After `Main` returns the process often lingers (shader compile workers, DDC commit, module teardown); the wrapper watches output mtime and force-kills via `taskkill` once all files have been stable for N seconds (default 10).

Both paths produce identical output regardless of editor state. The exported JSON carries the full structure: nodes, connections, properties, defaults, and timeline markers. AI tools can grep and offset-read it on demand.

## How it generalizes

UE is the proving ground; the three reusable parts don't depend on it.

| Reusable asset | What it is | Transfers to |
|---|---|---|
| Pattern | opaque binary -> AI-consumable structured text bridge | any GUI-locked proprietary format (DCC, CAD/BIM, EDA, simulation) |
| Architecture | heartbeat-routed adaptive dual pipeline (live in-process vs headless) | any heavyweight host with both live and headless modes (Houdini/Maya/Blender/Revit/MATLAB) |
| Serialization discipline | token-economy-aware export (delta-from-archetype, cap-and-sample, grep+offset reading contract) | context engineering for any LLM data pipeline |

## Comparison with UE MCP approaches

The community offers another approach, UE MCP (e.g. `kvick-games/UnrealMCP`, `chongdashu/unreal-mcp`): it uses Remote Control / Python bridges to let AI manipulate assets and scenes at editor **runtime**. The two solve different problems and are not mutually exclusive.

| Dimension | This plugin (Commandlet) | UE MCP (editor runtime) |
|---|---|---|
| Working mode | Offline JSON export, AI reads text | Online RPC, AI drives the editor directly |
| Call paradigm | File-queue RPC (subsystem mode, MCP-aligned in principle) / process spawn (commandlet mode) | Online RPC (HTTP / Python bridge) |
| Editor state | Adaptive: in-editor subsystem when open, commandlet when closed | Must be open |
| Asset structure readout | Strong, full EdGraph / Pin / connection serialization | Weak, Remote Control cannot reach EdGraph detail |
| Runtime state | None | Strong, can read PIE actors, selection, live properties |
| Token cost | Controllable, grep + offset reads on demand | Uncontrollable, driven by RPC response size |
| Suitable tasks | Static analysis, blueprint review, batch config audit | Scene building, PIE debugging, ad hoc parameter tweaks |
| Reproducibility | High, JSON can be version-controlled and diffed | Low, depends on runtime context |

**Selection guidance**:

- Need AI to understand blueprint logic, trace notify timing, audit DataTable / DataAsset config → use this plugin
- Need AI to spawn actors in PIE, change live parameters, read the current selection → use UE MCP
- They can coexist: commandlet handles static structure, MCP handles dynamic state

## Supported exporters

| Commandlet | `-run=` name | What it exports |
|---|---|---|
| `BlueprintEdGraphExportCommandlet` | `BlueprintEdGraphExport` | Blueprint EdGraph nodes, pins, connections, variables, components, referenced assets |
| `AnimMontageExportCommandlet` | `AnimMontageExport` | Montage sections, slots, ANS/AN placement and duration, notify custom parameters |
| `WidgetLayoutExportCommandlet` | `WidgetLayoutExport` | Widget tree hierarchy, slot layout properties, subclass properties, animation keyframes, EdGraph |
| `DataAssetExportCommandlet` | `DataAssetExport` | All custom properties of DataAsset subclasses, array elements expanded |
| `DataTableExportCommandlet` | `DataTableExport` | DataTable row struct name, all row data (indexed by RowName) |
| `NiagaraSystemExportCommandlet` | `NiagaraSystemExport` | Niagara system emitter list, spawn/update script parameters, renderer properties |
| `MaterialExportCommandlet` | `MaterialExport` | Material node graph (expression connection chain), global settings; MaterialInstance parameter overrides |
| `TextureExportCommandlet` | `TextureExport` | Texture asset properties via reflection (compression, sRGB, LOD group, mip settings, imported/source dimensions) |
| `BehaviorTreeExportCommandlet` | `BehaviorTreeExport` | BT tree structure (Composite/Task/Decorator/Service), node parameters, Blackboard keys |
| `AnimBlueprintExportCommandlet` | `AnimBlueprintExport` | AnimBP EdGraph, StateMachine (states/transitions/conditions/blend settings) |
| `LevelExportCommandlet` | `LevelExport` | Level (.umap) actors / components, delta-from-archetype properties, collision / static mesh / ISM summary, streaming level configuration |

## Usage

### Recommended: invoke via wrapper script

```bash
bash scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/YourProject.uproject" \
    BlueprintEdGraphExport \
    "/Game/Path/BP_A,/Game/Path/BP_B"
```

The wrapper routes automatically based on the heartbeat (see [How it works](#how-it-works) above).

Optional arguments: `[IDLE_SEC] [MAX_SEC]`, default `10` and `600`. Exit code `0` = success, `1` = missing outputs or dispatch failure, `2` = argument error or editor conflict.

### Native invocation

```bash
"<UE_PATH>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
    "<PROJECT_DIR>/YourProject.uproject" \
    -run=BlueprintEdGraphExport \
    -assets="/Game/Path/BP_A,/Game/Path/BP_B" \
    -nullrhi -nosplash -nosound
```

Replace the name after `-run=` with the exporter you need. All exporters share the same `-assets=` argument format.

If running under Git Bash, prefix with `MSYS_NO_PATHCONV=1` to prevent `/Game/...` from being rewritten as a Windows path.

With native invocation, if the process does not exit on its own, you must `taskkill` it manually. Otherwise the lingering process keeps `.uproject` locked and blocks subsequent operations.

### Troubleshooting

If the wrapper's heartbeat path hangs and the fallback commandlet process won't die after the wrapper's `taskkill`, Visual Studio is almost certainly holding the process. Don't tiptoe around it — kill it directly:

```bash
taskkill /F /IM UnrealEditor-Cmd.exe
```

Kill the offending Visual Studio process too if needed. A stuck `UnrealEditor-Cmd.exe` keeps `.uproject` locked and blocks every subsequent run.

### Output

Files are written to `<ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json`, which stays out of version control.

Every JSON file contains `ExporterVersion` and `ExportType` fields identifying the exporter version and asset type.

### Output examples

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

MaterialInstance exports the parameter override table:

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

Export strategy: every actor / component only serializes **properties that differ from its archetype (`UObject::GetArchetype()`)**, mirroring how `.umap` itself persists, for zero loss and maximum compression. Actors spawned from blueprints correctly align to the BPGC CDO, so "BP default" and "instance override" stay distinguishable.

For ISM / HISM / Foliage components with instance count > 200, only count + bounds + the first 5 samples are exported, to prevent a single foliage component from blowing up the file size.
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

### Reading strategy

The exported JSON can be very large (e.g. a complex PlayerCharacter blueprint can exceed 8000 lines). Recommended:

1. Use grep to locate the node names, function names, or pin connections you care about
2. Read the relevant range by line numbers, do not load the entire file at once

## Integrating into your project

### Option 1: as a project plugin

1. Copy the contents of `src/` into your project's `Plugins/UAssetJsonExporter/`
2. Add to the `Plugins` array in `.uproject`:

```json
{
    "Name": "UAssetJsonExporter",
    "Enabled": true
}
```

3. Regenerate project files and build

### Option 2: as an engine plugin

Copy the contents of `src/` into `<UE_PATH>/Engine/Plugins/Editor/UAssetJsonExporter/`, shared by all projects.

### Prerequisites

- Unreal Engine 5.7
- The project must be compiled with the plugin included
- Calling via the wrapper does not require closing the editor. Calling the native commandlet directly still requires it, so the editor can release the `.uproject` lock

## Using with Claude Code

If you use [Claude Code](https://claude.ai/code) as your AI collaborator, add the following block to your project's `.claude/CLAUDE.md` so the AI knows this tool exists and how to use it:

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
| TextureExportCommandlet | `TextureExport` | Texture asset properties via reflection (compression, sRGB, LOD group, mips, source dimensions) |
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

Once this block is added, the AI invokes the commandlet automatically during relevant tasks to export and analyze the assets.

## Version

Current version: **v1.5.0**

The version is defined in `src/Source/UAssetJsonExporter/Public/UAssetJsonExporterVersion.h`, and is embedded in every exported JSON's `ExporterVersion` field.

## License

[MIT](LICENSE) - Hyrex Chia
