#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "EditBlueprintCommandlet.generated.h"

class FJsonObject;

// Edits a Blueprint through one writer per facet, split the way the editor's Blueprint diff splits
// it: components, variables, class defaults, graph, plus layout. One target loads the asset once,
// runs every writer the spec names, then compiles and saves once. A writer that fails aborts the
// whole target before anything is written, so a Blueprint never lands half-edited.
//
// Writers run in a fixed order regardless of key order in the spec, because each depends on the last:
//   Components -> Variables -> Defaults -> Graph -> Layout
// Graph can reference components and variables the earlier writers made, and Layout addresses nodes
// by the Id Graph gave them, which is the whole reason these are one commandlet instead of five.
//
// Usage:
//   UnrealEditor-Cmd.exe Project.uproject -run=EditBlueprint -spec="C:/path/spec.json" [-apply]
//   Without -apply: dry run, reports only. With -apply: compile + save each modified Blueprint.
//
// Spec:
//   {
//     "Targets": [
//       {
//         "AssetPath": "/Game/Path/BP_Foo",
//         "Components": [
//           { "Op": "Add", "Class": "/Script/Engine.PointLightComponent", "Name": "Lamp" },
//           { "Op": "Add", "Class": "/Script/Engine.StaticMeshComponent", "Name": "Lid",
//             "Parent": "Lamp", "Asset": "/Game/Path/SM_Lid" },
//           { "Op": "Rename", "Name": "Lamp", "NewName": "KeyLight" },
//           { "Op": "Reparent", "Name": "Lid", "Parent": "KeyLight" },
//           { "Op": "Remove", "Name": "Obsolete" }
//         ],
//         "Variables": [
//           { "Op": "Add", "Name": "Armed", "Type": "bool", "Default": "true", "Category": "Probe" },
//           { "Op": "Add", "Name": "Targets", "Type": "object", "SubType": "/Script/Engine.Actor",
//             "Container": "Array" },
//           { "Op": "Rename", "Name": "Armed", "NewName": "bArmed" },
//           { "Op": "Remove", "Name": "Legacy" }
//         ],
//         "Defaults": [
//           { "Properties": { "InitialLifeSpan": "5.0" } },
//           { "Component": "KeyLight", "Properties": { "Intensity": "2000.0" } }
//         ],
//         "Graph": {
//           "Name": "EventGraph",
//           "Nodes": [
//             { "Id": "ev", "Type": "CustomEvent", "EventName": "RunProbe", "PosX": 0, "PosY": 0 },
//             { "Id": "br", "Type": "Branch", "PosX": 300, "PosY": 0 }
//           ],
//           "PinDefaults": [ { "Node": "br", "Pin": "Condition", "Value": "true" } ],
//           "Unlink": [ { "Node": "A1B2...", "Pin": "then" } ],
//           "Links": [ { "FromNode": "ev", "FromPin": "then", "ToNode": "br", "ToPin": "execute" } ]
//         },
//         "Layout": [
//           { "Op": "StackHorizontal", "Nodes": ["ev", "br"], "Spacing": 120 }
//         ]
//       }
//     ]
//   }
//
// Components ops: Add, Remove, Rename, Reparent. Add attaches to the scene root unless Parent names
// an existing component. Adding a name that already exists is an error, not a no-op, so a re-run
// cannot quietly produce a second copy under an engine-suffixed name. Asset is optional and only
// means anything for components that take one.
//
// Variables ops: Add, Remove, Rename. Type is the word the editor's type dropdown shows: bool, byte,
// enum, int, int64, float, double, string, name, text, object, class, softobject, softclass, struct.
// SubType is required for enum / object / class / softobject / softclass / struct and names the enum,
// class or script struct. Container is None, Array, Set or Map.
//
// Defaults entries write CDO property values. An entry without Component targets the actor CDO
// itself, otherwise Component names a sub-object on it.
//
// Graph node types: CallFunction, VariableGet, VariableSet, Branch, Sequence, MultiGate, Select,
// MathExpression, CustomEvent, Self, MacroInstance. CallFunction resolves Target as a component name
// on the Blueprint first, falling back to a class path for static and library calls.
//
// Gate, DoOnce, ForEachLoop and "Do N" are macro graphs, not native nodes. Any Type that is not one
// of the above is looked up as a graph name in StandardMacros, so "Gate" works as written. Macros
// from another library go through Type MacroInstance with a full "Package.Asset:GraphName" Macro.
//
// Select takes either Enum (an enum path, option pins follow the enum entries) or OptionCount (2 is
// a bool ternary, more is an integer index). Neither leaves the index pin wildcard, which compiles
// only if the spec wires something typed into it.
//
// Sequence takes OutputCount. Two outputs (then_0, then_1) come for free, more are added on top.
//
// Nodes are addressed by Id. A node the Graph writer creates gets the Id the spec gave it; a node
// that already exists is addressed by the 32-hex NodeId that BlueprintEdGraphExport -graphs prints.
// Both live in one namespace, so a new node can be wired straight onto an existing one. Pin names
// accept either the internal name or the friendly name the editor displays.
//
// Links go through UEdGraphSchema_K2::TryCreateConnection, which validates and inserts conversion
// nodes the same way dragging a wire in the editor does. Connections the schema rejects are errors.
//
// Layout ops: Arrange, Straighten, AlignLeft, AlignRight, AlignTop, AlignBottom, AlignCenterX,
// AlignCenterY, StackHorizontal, StackVertical, Move. An op may name a Graph of its own, otherwise
// it uses the one Graph ran on.
//
// Arrange lays a whole graph out from topology, so no coordinate has to be written by hand. Nodes
// names the entry points to walk from, not the nodes to move. The exec chain runs left to right on
// one row, each further exec output off a node starts a new row below everything placed so far, and
// data feeders sit to the left of whatever consumes them at the Y that leaves the wire flat, each
// sibling stepping past the previous one's subtree. Anything the entry points never reach is parked
// below the result, which is where the disabled event nodes of a fresh Blueprint end up. Spacing is
// the gap between columns, RowSpacing between rows.
//
// Straighten is the editor's Q on a selection: the leftmost of the named nodes holds still and the
// rest slide vertically until the pins at each end of a wire share a line. A node with several links
// lands on the average of them, since one Y cannot satisfy all.
//
// Stack ops keep the first node where it is and lay the rest after it in spec order.
//
// Exit codes: 0 success, 1 bad args / unresolvable name / rejected connection, 2 live editor.
UCLASS()
class UEditBlueprintCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UEditBlueprintCommandlet();

    virtual int32 Main(const FString& Params) override;

private:
    // Returns false once any writer fails, leaving the Blueprint unsaved.
    bool ApplyTarget(const TSharedPtr<FJsonObject>& Entry, bool bApply, TMap<UBlueprint*, bool>& OutTouched, int32& OutOps) const;
};
