# Migrate

C++ 或资产改名之后，修复留在 uasset 里的旧引用。

## 为什么需要 Blueprint 侧的 redirect

CoreRedirects 只覆盖调用侧: call 节点、变量引用、类引用。Blueprint 图里的实现侧与消费侧不在它的射程内，会留下两类症状。

| 症状 | 发生条件 | 表现 |
| --- | --- | --- |
| override 静默失联 | 改名的 interface event，`BlueprintNativeEvent` 或 `BlueprintImplementableEvent` | BP 里的 override 退化成一个孤立的 custom event，编译只给 warning，事件不再触发 |
| 悬空 pin | 改名的 delegate 参数 | 绑定节点上留着过期的输出 pin 与连线，指向一个已经不存在的参数 |

两类都编译得过去，靠人眼很难在大项目里扫出来。

## Commandlet

| RunName | 做什么 | 默认行为 |
| --- | --- | --- |
| `RedirectBlueprintEvent` | 把退化成 custom event 的 BP override 重新接回新事件，连线一并搬过去。能识别 UE 加的 `_N` 去重后缀 | dry run |
| `RedirectBlueprintPin` | 把绑定节点的连线从旧输出 pin 移到新 pin，然后重建节点丢掉旧 pin。只处理同时带有新旧两个 pin 的节点 | dry run |
| `DeleteBlueprintNode` | 按 node id 删图节点，图逻辑搬进 C++ 之后的清理步骤。删除会切断该节点所有连线，不重新接线。schema 拒删的节点，function entry 与 result，报出后跳过 | dry run |
| `ReparentBlueprint` | 改 Blueprint 的父类 | 直接执行并保存 |
| `ResaveAsset` | 强制 load、compile、save，让 load 期的 fixup 落盘，例如已被 CoreRedirect 解析的引用。之后就能撤掉那条 redirect。支持 Blueprint 和 map | 直接保存 |
| `SanitizeLevelReference` | 把 level 里对旧资产的每一处引用换成新资产，然后 resave 这个 level | 直接保存，`-dryrun` 只统计 |

执行后被改动的 uasset 会出现在版本控制的工作副本里。

## 调用

`RedirectBlueprintEvent`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintEvent "/Game/Blueprints/BP_Foo" 10 600 \
    '-OwnerClass="/Script/MyModule.MyActor" -OldEvent="OnPickedUp" -NewEvent="HandlePickedUp"'
```

`RedirectBlueprintPin`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    RedirectBlueprintPin "/Game/Blueprints/BP_Foo" 10 600 \
    '-OldPin="Amount" -NewPin="DeltaAmount"'
```

`DeleteBlueprintNode`。node id 从 `BlueprintEdGraphExport` 加 `-graphs` 的导出产物里的 `NodeId` 原样抄，纯十六进制加逗号，不需要引号。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    DeleteBlueprintNode "/Game/Blueprints/BP_Foo" 10 600 \
    '-nodes=A1B2C3D4E5F64A7B8C9D0E1F2A3B4C5D,0F1E2D3C4B5A69788796A5B4C3D2E1F0'
```

没命中任何节点的 id 会在结尾统一报出来，打错字不会静默通过。因删除而失去引用的 Blueprint 变量不动，要另外处理。

`ReparentBlueprint`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    ReparentBlueprint "/Game/Blueprints/BP_Foo" 10 600 \
    '-blueprints="/Game/Blueprints/BP_Foo" -oldclass="AOldActor" -newclass="ANewActor"'
```

`ResaveAsset`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    ResaveAsset "/Game/Blueprints/BP_Foo,/Game/Maps/MyLevel" 10 600 \
    '-assets="/Game/Blueprints/BP_Foo,/Game/Maps/MyLevel"'
```

`-nocompile` 可以跳过编译，只做 load 与 save。

`SanitizeLevelReference`。level 路径同时放 `AssetList` 与 `-levels`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" "<PROJECT_DIR>/MyProject.uproject" \
    SanitizeLevelReference "/Game/Maps/L_Foo" 10 600 \
    '-levels="/Game/Maps/L_Foo" -replace="/Game/Old/SM_Arch=/Game/New/SM_Foundation" -dryrun'
```

## SanitizeLevelReference

用途: 重指向 level 包内部的资产引用，把每一处对旧资产的引用换成新资产，然后 resave 这个 level。场景是资产改了名但旧的那份还留着，别的 level 仍然 import 旧路径，先用这个把 import 改指到新资产，之后才能安全删掉旧的。

它是 `AuditLevelReference` 的配对操作，audit 找出破损，sanitize 修，见 [Audit.md](Audit.md)。

### 参数

| 参数 | 说明 |
| --- | --- |
| `-levels=` | 逗号分隔的 level 包路径 |
| `-replace=` | 逗号分隔的 `old=new` 资产路径对 |
| `-report=` | 报告 JSON 输出路径，默认在 `Intermediate/SanitizeLevelReference/` 下 |
| `-dryrun` | 统计会改动的引用数并写报告，但不修改也不保存 |

关键约束: 新旧资产都必须还在磁盘上，`FArchiveReplaceObjectRef` 要把两边都 load 起来才能换指针。必须在删除旧资产之前跑，删完再跑就来不及了。

每个 level 独立处理。某个 level load 或 save 失败只记录该 level 失败然后继续，内存里的改动随 GC 丢弃，磁盘上的 `.umap` 不受影响。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 完成，或没有需要替换的 |
| 1 | 至少一个 level 失败，或参数错误 |
| 2 | 编辑器在运行 |

## dry run 与 apply

默认行为按 commandlet 分两种。

| RunName | 默认 | 落盘方式 |
| --- | --- | --- |
| `RedirectBlueprintEvent` | 只扫描 | 补 `-apply` |
| `RedirectBlueprintPin` | 只扫描 | 补 `-apply` |
| `DeleteBlueprintNode` | 只扫描 | 补 `-apply` |
| `SanitizeLevelReference` | 直接落盘 | 想先看结果就加 `-dryrun` |

两个 redirect 的扫描输出里逐条列出命中的 Blueprint、事件或节点、以及会搬多少组连线，确认无误再补 `-apply` 编译并保存。

扫描阶段一条都没命中时会给 warning，先核对 `-OwnerClass` 与旧名拼写，再考虑扩大 `-assets` 范围。
