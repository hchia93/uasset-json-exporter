# Edit

按意图修改既有资产。资产层面的 redirect / resave / replace 属于 [Migrate](Migrate.md)，从 spec 重新生成属于 [Import](Import.md)。

## Writer

`EditBlueprint` 一个面配一个 writer，划分对齐编辑器 Blueprint diff（`SBlueprintDiff.cpp`）。一个 target 只 load 一次资产，跑完 spec 点名的所有 writer，编译保存一次。任一 writer 失败整个 target 不落盘。

| Spec key | Writer | 对应 diff mode | 写什么 |
| --- | --- | --- | --- |
| `Components` | `FBlueprintComponentWriter` | `ComponentsMode` | SimpleConstructionScript 组件树 |
| `Variables` | `FBlueprintVariableWriter` | `MyBlueprintMode` | 成员变量 |
| `Defaults` | `FBlueprintDefaultsWriter` | `DefaultsMode` | CDO 与组件模板的属性值 |
| `Graph` | `FBlueprintGraphWriter` | `GraphMode` | 节点、pin 默认值、连线 |
| `Layout` | `FBlueprintLayoutWriter` | 无（纯外观） | 节点位置 |

执行顺序固定，与 spec 里的 key 顺序无关：

```
Components -> Variables -> Defaults -> Graph -> Layout
```

后一个依赖前一个的结果。`Graph` 能引用同一次运行里新建的组件和变量，`Layout` 能用 `Graph` 给节点的 `Id` 寻址，这是五个 writer 合成一个 commandlet 的原因。

## 调用

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    EditBlueprint "" 10 600 "-spec=C:/path/spec.json -apply"
```

无 `-apply` 是 dry run，只报告。AssetList 传空字符串，target 写在 spec 里。

## Spec

```json
{
  "Targets": [
    {
      "AssetPath": "/Game/Path/BP_Foo",
      "Components": [
        { "Op": "Add", "Class": "/Script/Engine.PointLightComponent", "Name": "Lamp" },
        { "Op": "Rename", "Name": "Lamp", "NewName": "KeyLight" },
        { "Op": "Reparent", "Name": "Lid", "Parent": "KeyLight" },
        { "Op": "Remove", "Name": "Obsolete" }
      ],
      "Variables": [
        { "Op": "Add", "Name": "BaseIntensity", "Type": "float", "Default": "500.0" },
        { "Op": "Add", "Name": "Watchers", "Type": "object",
          "SubType": "/Script/Engine.Actor", "Container": "Array" }
      ],
      "Defaults": [
        { "Properties": { "InitialLifeSpan": "12.0" } },
        { "Component": "KeyLight", "Properties": { "Intensity": "2400.0" } }
      ],
      "Graph": {
        "Name": "EventGraph",
        "Nodes": [
          { "Id": "ev", "Type": "CustomEvent", "EventName": "RunProbe" },
          { "Id": "gate", "Type": "Gate" }
        ],
        "PinDefaults": [ { "Node": "gate", "Pin": "bStartClosed", "Value": "true" } ],
        "Unlink": [ { "Node": "A1B2...", "Pin": "then" } ],
        "Links": [ { "FromNode": "ev", "FromPin": "then", "ToNode": "gate", "ToPin": "Enter" } ]
      },
      "Layout": [
        { "Op": "StackHorizontal", "Nodes": ["ev", "gate"], "Spacing": 140 }
      ]
    }
  ]
}
```

## Components

Op: `Add` / `Remove` / `Rename` / `Reparent`，按 spec 顺序执行，后面的能看到前面的结果。

`Add` 默认挂到场景根，`Parent` 指定则挂到该组件下。`Asset` 可选，只对吃资产的组件有意义。名字已存在是错误而不是 no-op，重跑不会悄悄多出一个带引擎后缀的副本。

## Variables

Op: `Add` / `Remove` / `Rename`。

`Type` 用编辑器类型下拉框显示的词：`bool` `byte` `enum` `int` `int64` `float` `double` `string` `name` `text` `object` `class` `softobject` `softclass` `struct`。

`SubType` 在 `enum` / `object` / `class` / `softobject` / `softclass` / `struct` 下必填，写枚举、类或 script struct 的路径。`Container` 取 `None` / `Array` / `Set` / `Map`。`Default` 与 `Category` 可选。

## Defaults

每条写一个作用域。不带 `Component` 写 actor CDO 本身，带则写该组件。

BP 自己声明的组件属性挂在 SCS node 的 template 上，继承来的 native 组件则以 CDO 子对象的形式保存 override，两条路径都覆盖。

## Graph

节点类型：`CallFunction` `VariableGet` `VariableSet` `Branch` `Sequence` `MultiGate` `Select` `MathExpression` `CustomEvent` `Self` `MacroInstance`。

`CallFunction` 的 `Target` 先当组件名解析，找不到再当类路径，静态函数和函数库走后者。

`Gate` / `DoOnce` / `ForEachLoop` / `Do N` 是 StandardMacros 里的宏图不是原生节点。上表以外的 `Type` 一律去 StandardMacros 里按图名查，所以 `Gate` 直接写就行。别的宏库走 `MacroInstance` 加完整的 `"Package.Asset:GraphName"`。

`Select` 二选一：`Enum` 给枚举路径，option pin 跟枚举条目走；`OptionCount` 给数量，2 是 bool 三元，更多是整数索引。两者都不给则 index pin 保持 wildcard，除非 spec 往里接了有类型的东西，否则编译不过。

`Sequence` 用 `OutputCount` 给出口数，`then_0` `then_1` 自带，更多的往上加。

节点用 `Id` 寻址。本次新建的节点用 spec 给的 `Id`，已存在的节点用 `BlueprintEdGraphExport -graphs` 打印的 32 位 `NodeId`，两者同一命名空间，新节点可以直接接到旧节点上。Pin 名内部名和编辑器显示的 friendly name 都认。

执行顺序：建节点 → pin 默认值 → `Unlink` → `Links`。`Unlink` 在 `Links` 之前，所以一次 pass 就能改道既有的 exec 链。

连线走 `UEdGraphSchema_K2::TryCreateConnection`，与编辑器里拖线同路，会校验并在需要时插转换节点。schema 拒绝的连线是错误。

## Layout

Op: `Arrange` `Straighten` `AlignLeft` `AlignRight` `AlignTop` `AlignBottom` `AlignCenterX` `AlignCenterY` `StackHorizontal` `StackVertical` `Move`。

op 可以自带 `Graph` 指定别的图，不写就用 `Graph` writer 用的那张。

### Arrange

按拓扑铺整张图，一行坐标都不用手写。

```json
{ "Op": "Arrange", "Nodes": ["ev"], "Spacing": 80, "RowSpacing": 48, "PosX": 0, "PosY": 0 }
```

`Nodes` 在这个 op 里是**入口点**不是操作对象。规则：

| | |
| --- | --- |
| exec 链 | 从入口往右平铺，共用一行 |
| 一个节点的第二条及以后的 exec 出口 | 在已排内容下方另起一行，Branch 与 Sequence 都走这条 |
| 数据 feeder | 摆在消费者左边，Y 取「让这条线拉平」的值 |
| 同一个节点的多个 feeder | 依次往左错开，跨过前一个的整棵子树，谁都不用让出自己的水平线 |
| 入口走不到的节点 | 兜底停在结果下方，新建 BP 自带的禁用事件节点就在这里 |

`Spacing` 是列间距，`RowSpacing` 是行间距。

### Straighten

对应编辑器里选中节点按 Q。

```json
{ "Op": "Straighten", "Nodes": ["setInt", "sel", "math", "getBase"] }
```

最左边的节点当锚点不动，其余顺连线滑动到两端 pin 同一水平线。一个节点有多条连线时落在平均值上，因为一个 Y 满足不了全部。

### 尺寸估算

`NodeWidth` / `NodeHeight` 要 Slate 画过一次才有值，headless 下新建的节点全是 0，靠尺寸和 pin 位置的 op 都得估：高度是 `标题栏 48 + 可见 pin 行数 × 24`，宽度按标题长度撑、下限 200，pin 的纵向位置是 `48 + 同向 pin 序号 × 24 + 半行`。只有一个可见 pin 的紧凑节点（`Get` 那种）没有标题行，pin 走中线。

固定常量兜底不行：`Set Collision Profile Name` 有 4 行 pin 实际约 144 高，按 100 排必然重叠。

### Stack / Align

Stack 保持第一个节点不动，其余按 spec 顺序排在它后面。

## 约束

commandlet 没有 UI，没有 selection 状态。任何依赖"当前选中"的编辑器操作在这里不成立，对齐一类必须由 spec 显式点名节点。

Python 侧对 EdGraph 暴露极少（`UbergraphPages` / `parent_class` / `Notifies` 均不可读），这一组只能是 C++ commandlet。
