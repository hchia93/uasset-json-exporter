# uasset-workbench

**English** | [中文](README_CN.md)

![Claude Code](https://img.shields.io/badge/Claude_Code-black?style=flat&logo=anthropic&logoColor=white)
![Unreal Engine 5](https://img.shields.io/badge/Unreal_Engine-5.7-blue?logo=unrealengine&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

An editor plugin that lets scripts and AI agents operate on Unreal Engine 5 uassets non-interactively.

## What problem it solves

| Problem | How it shows up |
| --- | --- |
| Can't read | Logic and configuration are locked inside binary `.uasset` files, and an AI handed the file has nowhere to start. An EventGraph with hundreds of nodes has no readable text form, and abandoned variables, broken connections, wrong defaults are impossible to fully audit by eye in the editor. Montage notify timing, UMG hierarchy and keyframes, Niagara and material parameters, DataTable values, level actor placement and streaming config all exist only in the editor UI |
| Can't write | Changing a UMG layout means dragging by hand in the editor, there is no version-controllable, replayable write path |
| Can't fix after a rename | After a C++ or asset rename, CoreRedirects only fixes the call side. It cannot fix the implementation side or the consumer side inside Blueprint graphs, nor another level's import of the old path |
| Can't audit | Broken references, level streaming topology, per-level component budget have no batch query entry point |

Four problems, four capability groups.

## The four groups

| Group | What it does | Output | Count |
| --- | --- | --- | --- |
| Export | Read uasset structure, write JSON | JSON under `Intermediate/UAssetExport` | 11 |
| Import | Read a JSON spec, write back to or create uassets | modified or new uassets | 3 |
| Migrate | Fix references after a C++ or asset rename | modified uassets | 5 |
| Audit | Read-only checks producing a report | report JSON | 2 |

The group is decided by the run name: suffix `Export` is the Export group, suffix `Import` and prefix `Create` are the Import group, prefix `Audit` is the Audit group, everything else is Migrate. Naming-wise `Import` and `Export` are nouns used as suffixes, every other verb leads.

## Routing and architecture

Every operation converges on the same calling contract. The heartbeat file `Saved/UAssetExportQueue/.alive` decides the route.

| Editor state | Which path | Feedback |
| --- | --- | --- |
| Open | Write a pending task, the in-editor subsystem runs it in-process | Export raises a bottom-right toast, the rest goes to the Message Log |
| Closed | Launch `UnrealEditor-Cmd` to run the commandlet | log |

Both paths produce identical output, the caller does not need to care whether the editor is open.

What the routing buys: the four groups share one call entry and one output convention, so workflows compose, export the structure, analyze offline, write back to the asset, audit to verify, instead of every added tool bringing its own way of being called. Adding a commandlet adds a capability, the contract does not change.

## Quick start

Wrapper: `src/scripts/run_commandlet.sh`, which sits at `Plugins/UAssetWorkbench/scripts/` once integrated into a project.

```
run_commandlet.sh <UE_PATH> <UPROJECT> <RunName> <AssetList> [IDLE_SEC] [MAX_SEC] [EXTRA_ARGS]
```

| Argument | Meaning |
| --- | --- |
| `UE_PATH` | Engine install root |
| `UPROJECT` | Absolute path to the `.uproject` |
| `RunName` | The commandlet's run name |
| `AssetList` | Comma-separated asset paths, the Audit group ignores it, pass an empty string |
| `IDLE_SEC` | Output mtime quiet seconds the Export group uses to decide completion, default 10 |
| `MAX_SEC` | Total wait cap, default 600 |
| `EXTRA_ARGS` | Passed through to the commandlet |

Under Git Bash prefix with `MSYS_NO_PATHCONV=1`, otherwise `/Game/...` gets rewritten into a Windows path.

Export.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    BlueprintEdGraphExport "/Game/Blueprints/BP_Foo"
```

Import.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutImport "/Game/UI/WBP_Foo" 10 600 \
    '-spec="C:/temp/WBP_Foo.spec.json"'
```

Migrate.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintEvent "/Game/Blueprints/BP_Foo" 10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OnPickedUp" -NewEvent="HandlePickedUp"'
```

Audit.

```bash
MSYS_NO_PATHCONV=1 bash src/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    AuditLevelReference "" 10 600 \
    '-scandir="/Game"'
```

## Groups in detail

<details>
<summary><b>Export</b>, 11 commandlets</summary>

| RunName | What it exports |
| --- | --- |
| `BlueprintEdGraphExport` | Blueprint graphs, nodes, pins, connections, variables, components, referenced assets |
| `AnimMontageExport` | Montage sections, slots, ANS/AN placement and duration, notify custom parameters |
| `WidgetLayoutExport` | Widget tree, slot layout properties, subclass properties, animation keyframes, EdGraph |
| `DataAssetExport` | All custom properties of DataAsset subclasses, array elements expanded |
| `DataTableExport` | DataTable row struct name and all row data, indexed by RowName |
| `NiagaraSystemExport` | Niagara emitter list, spawn/update script parameters, renderer properties |
| `MaterialExport` | Material expression connection chain and global settings, MaterialInstance parameter overrides |
| `TextureExport` | Texture properties, compression, sRGB, LOD group, mips, source dimensions |
| `BehaviorTreeExport` | BT tree structure, node parameters, Blackboard keys |
| `AnimBlueprintExport` | AnimBP EdGraph, state machine states, transitions, conditions, blend settings |
| `LevelExport` | Level actors / components, delta-from-archetype properties, collision / static mesh / ISM summary, streaming levels |

Output: `Intermediate/UAssetExport/<AssetPath>.json`, out of version control.

Every JSON carries the two common fields `ExporterVersion` and `ExportType`, the rest expands per asset type.

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

MaterialInstance exports the parameter override table.

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

Export strategy: every actor / component serializes only the properties that differ from its own archetype (`UObject::GetArchetype()`), mirroring how `.umap` itself persists, lossless and at the highest compression rate. Blueprint-spawned actors align correctly to the BPGC CDO, so blueprint defaults and instance overrides remain distinguishable.

ISM / HISM / Foliage components with more than 200 instances export only the count, the bounds, and the first 5 samples, so a single foliage component cannot blow up the file.
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
<summary><b>Import</b>, 3 commandlets</summary>

| RunName | What it does |
| --- | --- |
| `WidgetLayoutImport` | Rebuilds a Widget Blueprint's widget tree from a spec |
| `DataAssetImport` | Writes JSON into a DataAsset's properties |
| `CreateAsset` | Creates assets of any type from a spec |

All three take `-spec=` pointing at the spec's absolute path. `CreateAsset` additionally requires `-unattended`, because `FMessageDialog` does not check for commandlet mode and some creation paths would raise a blocking dialog.

**WidgetLayoutImport**

Top-level spec fields.

| Field | Meaning |
| --- | --- |
| `AssetPath` | Asset path of the target Widget Blueprint |
| `ClassDefaults` | Optional, lands on the generated class CDO |
| `WidgetTree` | Root node, recursive from there |

Node fields.

| Field | Meaning |
| --- | --- |
| `Class` | Bare UMG class name such as `HorizontalBox`, or the full object path of a Blueprint class such as `/Game/UI/WBP_Bar.WBP_Bar_C` |
| `Name` | Widget name, C++ `BindWidget` matches on it |
| `Properties` | Property name to string value, applied through reflection `ImportText` |
| `Slot` | The node's slot properties inside its parent container, also through `ImportText` |
| `Children` | Child node array, the parent must be a panel type |

Property value strings take the same form Export emits, e.g. `(Value=1.000000,SizeRule=Fill)`, `(Right=48.000000)`, `HAlign_Fill`.

The behavior is whole-tree replacement, not incremental merge, so the spec must describe the complete tree. A `WidgetLayoutExport` output feeds straight back in as Import input, and the usual way to change a layout is Export the current tree, edit the JSON, Import it back.

`ClassDefaults` lands on the generated class CDO, which is where `EditDefaultsOnly` properties live rather than in the widget tree. It is applied after the compile, because the compile rebuilds the CDO.

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

**DataAssetImport**

The inverse of `DataAssetExport`. Property values take either of two forms.

| Form | Goes through | Used for |
| --- | --- | --- |
| String | reflection `ImportText` | the struct literal form `DataAssetExport` emits, so an exported asset round-trips back in |
| Object or array | the json converter | nested structs and arrays in a hand-written spec, far more readable |

Only the named properties are written, everything else on the asset keeps its current value. If any single property fails to write, the whole save is abandoned, so no half-written asset is left behind.

```json
{
  "AssetPath": "/Game/Path/DA_Foo",
  "Properties": {
    "Scalar": "12.0",
    "Nested": [ { "Name": "A" }, { "Name": "B" } ]
  }
}
```

**CreateAsset**

The `Assets` array is created in order.

```json
{
  "Assets": [
    {
      "PackagePath": "/Game/Path",
      "AssetName": "DT_Foo",
      "Class": "/Script/Engine.DataTable",
      "FactoryProperties": { "Struct": "/Script/MyModule.MyRow" },
      "Properties": {}
    }
  ]
}
```

The two property blocks are separate because some types are only valid if their factory was configured first, otherwise creation fails or silently yields nothing. `FactoryProperties` is applied to the factory before creation, `Properties` is applied to the asset after creation and follows the same two forms as `DataAssetImport`.

| Asset type | Required in FactoryProperties | Consequence of omitting |
| --- | --- | --- |
| DataTable | `Struct`, the field name is `Struct`, not `RowStruct` | yields nothing |
| Blueprint | `ParentClass` plus `bSkipClassPicker` | blocking dialog |
| Texture2D | `Width` / `Height`, both must be powers of two | silently yields nothing |
| MaterialInstanceConstant | `InitialParent`, optional | an empty instance with no parent |

`Class` is best written as a full path such as `/Script/MediaAssets.MediaPlayer`, a short name resolves through a fuzzy engine lookup and warns, and a Blueprint generated class, which needs its package loaded, requires the full path. A later entry can reference in its `Properties` an asset path an earlier entry just created, so one spec wires up a group of mutually referencing assets. An asset already existing at the same path is skipped and reported, this never overwrites.

Material node graphs are out of reach. `CreateAsset` produces an empty material, but adding expressions such as `TextureSample` and wiring them up goes more directly through Python's `unreal.MaterialEditingLibrary`.

</details>

<details>
<summary><b>Migrate</b>, 5 commandlets</summary>

CoreRedirects covers the call side only, the implementation side and the consumer side inside Blueprint graphs are out of its range. A renamed interface event degrades a BP override into an orphaned custom event and the event stops firing, a renamed delegate parameter leaves a dangling pin on the binding node. Both still compile, and neither is findable by eye in a large project.

| RunName | What it does | Default |
| --- | --- | --- |
| `RedirectBlueprintEvent` | Reconnects a BP override that degraded into a custom event back to the new event, carrying its wiring across, and recognizes the `_N` dedup suffix UE appends | dry run |
| `RedirectBlueprintPin` | Moves a binding node's wiring from the old output pin to the new pin, then reconstructs the node to drop the old pin, touching only nodes that carry both the old and the new pin | dry run |
| `ReparentBlueprint` | Changes a Blueprint's parent class | writes directly |
| `ResaveAsset` | Forces load, compile, save so load-time fixups land on disk, after which that CoreRedirect can be dropped, supports Blueprint and map | writes directly |
| `SanitizeLevelReference` | Repoints every reference to an old asset inside a level at the new asset, then resaves that level | writes directly, `-dryrun` only counts |

Both redirects scan only by default, listing each Blueprint, event or node hit and how many wire groups would move, then take `-apply` to compile and save once it looks right. A scan with zero hits warns, check the `-OwnerClass` and the old name spelling first.

`SanitizeLevelReference` must run before the old asset is deleted. Repointing needs both the old and the new on disk, running it after the delete is too late.

Modified uassets show up in the version control working copy afterwards.

</details>

<details>
<summary><b>Audit</b>, 2 commandlets and 3 scripts</summary>

| RunName | What it checks |
| --- | --- |
| `AuditLevelReference` | References inside level packages pointing at assets that no longer exist |
| `AuditLevelTopology` | Streaming relationships between levels, which one is persistent, which one is a sublevel |

`AuditLevelReference` walks the Asset Registry dependency graph and checks package existence one by one, it does not load worlds and saves no package. Existence resolves against the mounted content roots, so every plugin of the project must be enabled before the run, otherwise dependencies under an unmounted root come back as false breakage. Its paired operation is `SanitizeLevelReference` in the Migrate group, audit finds the breakage, sanitize fixes it.

`AuditLevelTopology` assigns every level a role.

| Role | Test |
| --- | --- |
| `Standalone` | No streaming levels, and not referenced by another level |
| `PersistentHost` | Has streaming levels |
| `Sublevel` | No streaming levels, but referenced by another level |

Hosting outranks being hosted, a level that carries sublevels of its own is `PersistentHost` even when something else streams it in. The test is whether the level can be opened on its own to drive its sublevels, a copy of it living inside another level does not take that away, and `referenced_by` still records the nesting. Tools that need to drive streaming find the persistent level from this report instead of hardcoding a name into the script.

Three companion scripts.

| Script | What it does |
| --- | --- |
| `run_stream_metric.ps1` | Orchestration entry point for stream metric, one command covering topology, export, probe, report |
| `stream_metric_report.py` | Joins probe timings with `LevelExport` scale by level name into one table, including load milliseconds per thousand components |
| `level_budget_audit.py` | Reads `LevelExport` output offline into a component budget table, one row per level, flagging the ones over budget |

`run_stream_metric.ps1` covers four steps in one command.

1. Run `AuditLevelTopology` for the topology report, always executed, the target's sublevel list comes from it
2. Run `LevelExport` on those sublevels for scale data, `-SkipExport` skips it
3. Start an unattended editor measuring unload / gc / load wall-clock milliseconds and worst frame per sublevel
4. Run `stream_metric_report.py` to join timings with scale into one printed table

Without `-PersistentLevel` the script picks the single `PersistentHost` out of the topology report, and lists the candidates for an explicit pick when it finds more than one. The probe swaps `AlwaysLoaded` sublevels to `LevelStreamingDynamic` in memory so they can unload, and that step never saves. The editor must be closed before the run.

</details>

## Reading strategy

The exported JSON can be very large, a Blueprint of moderate complexity already reaches several thousand lines.

1. Grep first to locate the node titles, function names, widget names, or row names you care about
2. Read by line range once you have the line numbers, do not pull the whole file into context

```bash
grep -n "OnHealthChanged" Intermediate/UAssetExport/Game/Blueprints/BP_Foo.json
```

## Why not the official toolchain

The official entry points for AI and automation, MCP, Remote Control, the Python API, evolve along with the engine version, capability gaps open up, and some editor subsystems regress in particular versions. They cannot be assumed stable across versions, and UE6 is still some way off, the versions in between have to keep working.

The workbench goes through commandlets plus stable engine APIs, routing around the layer that is still moving. The output is JSON files, version-controllable, diffable, replayable. When a mechanism the official path does not offer is needed, this is the extension point, adding a commandlet adds a capability and the calling contract does not change.

Online interactive approaches, the kind driven at editor runtime, solve a different class of problem, scene building, PIE debugging, ad hoc parameter tweaks. The two are not mutually exclusive.

## Integrating into your project

Copy the contents of `src/` into the project's `Plugins/UAssetWorkbench/`, and add to the `Plugins` array in `.uproject`.

```json
{
    "Name": "UAssetWorkbench",
    "Enabled": true
}
```

Regenerate project files and build.

It can also live at `<UE_PATH>/Engine/Plugins/Editor/UAssetWorkbench/` to be shared by every project.

Prerequisites: Unreal Engine 5.7, and the plugin must be compiled with the project. Calling through the wrapper does not require closing the editor, launching the commandlet directly does.

## For AI agents

| Doc | Contents |
| --- | --- |
| `Docs/AI-Guide.md` | Call manual for AI agents, decision table plus call templates plus common pitfalls |
| `Docs/Export.md` | Export group detail |
| `Docs/Import.md` | Import group detail and spec format |
| `Docs/Migrate.md` | Migrate group detail |
| `Docs/Audit.md` | Audit group detail and the stream metric workflow |

These docs are reference material for agents to read. Where behavior and documentation disagree, the source wins, the block comment at the top of each commandlet header carries the full contract.

## How it generalizes

UE is only the proving ground, the three reusable parts do not depend on it.

| Reusable asset | What it is | Transfers to |
| --- | --- | --- |
| Pattern | bidirectional bridge between opaque binary and AI-readable structured text | any GUI-locked proprietary format, DCC / CAD / BIM / EDA / simulation |
| Architecture | heartbeat-routed adaptive dual pipeline, live in-process and headless producing identical output | any heavyweight host with both an interactive and a headless mode, Houdini / Maya / Blender / Revit / MATLAB |
| Serialization discipline | token-cost-aware export, delta-from-archetype, cap-and-sample on overflow, a grep plus range-read contract | context engineering for any LLM data pipeline |

## Version

Current version: **2.0.0**

Defined in `src/Source/UAssetWorkbench/Public/UAssetWorkbenchVersion.h`, and embedded in the `ExporterVersion` field of every exported JSON.

## License

[MIT](LICENSE) - Hyrex Chia
