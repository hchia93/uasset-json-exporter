#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_BlendSpaceSampleResult.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"

namespace
{
    // NodeWidth / NodeHeight fill in only once Slate has drawn a node, so a headless run reads zero on
    // everything the Graph writer just made. Extent is estimated from a title bar plus one row per visible
    // pin, fitted against a graph the editor straightened. A title's second line is worth half a row.
    constexpr int32 kTitleLineHeight = 16;
    constexpr int32 kTitleBarHeight = 32;
    constexpr int32 kPinRowHeight = 32;
    constexpr int32 kCompactNodeHeight = 52;
    constexpr int32 kMinNodeWidth = 200;
    constexpr int32 kTitleCharWidth = 8;

    // A state machine box carries a name and nothing else: no title bar, no pin rows, one rounded
    // rectangle the name can push past a floor.
    constexpr int32 kStateBoxHeight = 60;
    constexpr int32 kMinStateBoxWidth = 160;
    constexpr int32 kStateBoxPadding = 40;
    constexpr int32 kStateCharWidth = 9;

    bool IsPinVisible(const UEdGraphPin* Pin)
    {
        return Pin && !Pin->bHidden && !Pin->bAdvancedView;
    }

    // A transition draws as an arrow on the wire between two states, so its NodePos never reaches the
    // reader and moving it only dirties the asset.
    bool IsTransitionNode(const UEdGraphNode* Node)
    {
        return Node && Node->IsA<UAnimStateTransitionNode>();
    }

    bool IsStateBoxNode(const UEdGraphNode* Node)
    {
        return Node && (Node->IsA<UAnimStateNodeBase>() || Node->IsA<UAnimStateEntryNode>());
    }

    // A bare Get or a knot draws as a small pill: no title row, its one pin on the centre line.
    bool IsCompactNode(const UEdGraphNode* Node)
    {
        int32 VisibleCount = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (IsPinVisible(Pin))
            {
                ++VisibleCount;
            }
        }

        return VisibleCount <= 1;
    }

    int32 TitleHeightOf(const UEdGraphNode* Node)
    {
        const FString FullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

        TArray<FString> TitleLines;
        FullTitle.ParseIntoArrayLines(TitleLines);

        return kTitleBarHeight + FMath::Max(0, TitleLines.Num() - 1) * kTitleLineHeight;
    }

    int32 StateBoxWidthOf(const UEdGraphNode* Node)
    {
        const int32 NameWidth = Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Len() * kStateCharWidth + kStateBoxPadding;
        return FMath::Max(kMinStateBoxWidth, NameWidth);
    }

    int32 WidthOf(const UEdGraphNode* Node)
    {
        if (IsTransitionNode(Node))
        {
            return 0;
        }

        if (IsStateBoxNode(Node))
        {
            return StateBoxWidthOf(Node);
        }

        // A comment carries the box its author drew, which is what NodeWidth already holds.
        if (Node->NodeWidth > 0)
        {
            return Node->NodeWidth;
        }

        const int32 TitleWidth = Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Len() * kTitleCharWidth;
        return FMath::Max(kMinNodeWidth, TitleWidth);
    }

    // A bound property swaps its pin row for a binding widget, and hidden optional properties draw a row
    // of their own. Neither shows up in the visible pin count.
    int32 AnimExtraRowsOf(const UEdGraphNode* Node)
    {
        const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
        if (!AnimNode)
        {
            return 0;
        }

        int32 Rows = 0;

        // The map lives on an instanced binding object whose class sits in a private header, so it is
        // reached the way AnimBlueprintExport reaches it.
        const FObjectProperty* BindingProperty = FindFProperty<FObjectProperty>(AnimNode->GetClass(), TEXT("Binding"));
        const UObject* BindingObject = BindingProperty ? BindingProperty->GetObjectPropertyValue(BindingProperty->ContainerPtrToValuePtr<void>(AnimNode)) : nullptr;
        const FMapProperty* MapProperty = BindingObject ? FindFProperty<FMapProperty>(BindingObject->GetClass(), TEXT("PropertyBindings")) : nullptr;
        if (MapProperty)
        {
            FScriptMapHelper MapHelper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(BindingObject));
            if (MapHelper.Num() > 0)
            {
                ++Rows;
            }
        }

        for (const FOptionalPinFromProperty& OptionalPin : AnimNode->ShowPinForProperties)
        {
            if (!OptionalPin.PropertyName.IsNone() && !OptionalPin.bShowPin)
            {
                ++Rows;
                break;
            }
        }

        return Rows;
    }

    int32 HeightOf(const UEdGraphNode* Node)
    {
        if (IsTransitionNode(Node))
        {
            return 0;
        }

        if (IsStateBoxNode(Node))
        {
            return kStateBoxHeight;
        }

        if (Node->NodeHeight > 0)
        {
            return Node->NodeHeight;
        }

        if (IsCompactNode(Node))
        {
            return kCompactNodeHeight;
        }

        // Input and output pins share rows, so the taller side sets the height.
        int32 InputRows = 0;
        int32 OutputRows = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinVisible(Pin))
            {
                continue;
            }

            Pin->Direction == EGPD_Input ? ++InputRows : ++OutputRows;
        }

        return TitleHeightOf(Node) + (FMath::Max(InputRows, OutputRows) + AnimExtraRowsOf(Node)) * kPinRowHeight;
    }

    // Where a pin sits inside its node, which is what a straightened connection has to match. The
    // engine reads this off the live SGraphPin, headless it comes from the pin's row instead.
    int32 PinOffsetY(const UEdGraphNode* Node, const UEdGraphPin* Target)
    {
        if (IsCompactNode(Node))
        {
            return kCompactNodeHeight / 2;
        }

        int32 Row = 0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinVisible(Pin) || Pin->Direction != Target->Direction)
            {
                continue;
            }

            if (Pin == Target)
            {
                break;
            }

            ++Row;
        }

        return TitleHeightOf(Node) + Row * kPinRowHeight + kPinRowHeight / 2;
    }

    enum class EFlowKind : uint8
    {
        Data,
        Exec,
        Pose,
        Transition
    };

    // The state machine schema spells its exec category with the same FName K2 uses, so the graph, not
    // the pin, is what tells a transition wire apart from an exec wire.
    EFlowKind ClassifyPin(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return EFlowKind::Data;
        }

        if (UAnimationGraphSchema::IsPosePin(Pin->PinType))
        {
            return EFlowKind::Pose;
        }

        if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            return EFlowKind::Data;
        }

        const UEdGraphNode* Owner = Pin->GetOwningNodeUnchecked();
        const UEdGraph* OwningGraph = Owner ? Owner->GetGraph() : nullptr;
        return OwningGraph && OwningGraph->IsA<UAnimationStateMachineGraph>() ? EFlowKind::Transition : EFlowKind::Exec;
    }

    bool IsExecPin(const UEdGraphPin* Pin)
    {
        return ClassifyPin(Pin) == EFlowKind::Exec;
    }

    // Data is what a feeder column carries. Every other kind is a spine its own graph kind walks.
    bool IsDataPin(const UEdGraphPin* Pin)
    {
        return ClassifyPin(Pin) == EFlowKind::Data;
    }

    // Nodes wired into this one's data inputs. Spine links are walked separately.
    void GatherFeeders(UEdGraphNode* Node, TArray<UEdGraphNode*>& OutFeeders)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinVisible(Pin) || Pin->Direction != EGPD_Input || !IsDataPin(Pin))
            {
                continue;
            }

            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (Linked && Linked->GetOwningNode())
                {
                    OutFeeders.AddUnique(Linked->GetOwningNode());
                }
            }
        }
    }

    struct FArrangeState
    {
        TSet<UEdGraphNode*> Placed;
        TSet<UEdGraphNode*> Measuring;
        int32 ColumnGap = 80;
        int32 RowGap = 48;
    };

    // Horizontal room the feeder subtree needs to the left of Node. The exec spine advances by this
    // much on top of the node's own width, which is what keeps a long data chain from being sat on.
    int32 FeederSpanOf(UEdGraphNode* Node, FArrangeState& State)
    {
        if (State.Measuring.Contains(Node))
        {
            return 0;
        }

        State.Measuring.Add(Node);

        TArray<UEdGraphNode*> Feeders;
        GatherFeeders(Node, Feeders);

        // Siblings are laid out one past the other going left, so their spans add rather than compete.
        int32 Span = 0;
        for (UEdGraphNode* Feeder : Feeders)
        {
            if (State.Placed.Contains(Feeder))
            {
                continue;
            }

            Span += FeederSpanOf(Feeder, State) + WidthOf(Feeder) + State.ColumnGap;
        }

        State.Measuring.Remove(Node);
        return Span;
    }

    // Every feeder takes the Y that puts its own pin on the line of the pin it feeds, so the wire comes
    // out flat, and siblings step further left past each other's whole subtree instead of stacking down.
    // Returns the lowest Y consumed.
    int32 PlaceFeeders(UEdGraphNode* Node, FArrangeState& State)
    {
        int32 Bottom = Node->NodePosY + HeightOf(Node);
        int32 LeftEdge = Node->NodePosX;

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinVisible(Pin) || Pin->Direction != EGPD_Input || !IsDataPin(Pin))
            {
                continue;
            }

            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Feeder = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Feeder || State.Placed.Contains(Feeder))
                {
                    continue;
                }

                State.Placed.Add(Feeder);
                Feeder->NodePosX = LeftEdge - WidthOf(Feeder) - State.ColumnGap;
                Feeder->NodePosY = Node->NodePosY + PinOffsetY(Node, Pin) - PinOffsetY(Feeder, Linked);

                // Measured before the subtree is placed, since a placed feeder reports no span.
                const int32 SubtreeSpan = FeederSpanOf(Feeder, State);
                Bottom = FMath::Max(Bottom, PlaceFeeders(Feeder, State));
                LeftEdge = Feeder->NodePosX - SubtreeSpan;
            }
        }

        return Bottom;
    }

    // First exec output continues the current row, every further output starts a new row under everything
    // placed so far. Continuing a row still straightens, a node whose title carries a second line puts its
    // exec pin half a row lower, so a flat run is not a shared NodePosY.
    int32 PlaceExecChain(UEdGraphNode* Node, int32 PosX, int32 PosY, FArrangeState& State)
    {
        State.Placed.Add(Node);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;

        int32 Bottom = PlaceFeeders(Node, State);
        bool bFirstBranch = true;

        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinVisible(Pin) || Pin->Direction != EGPD_Output || !IsExecPin(Pin))
            {
                continue;
            }

            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Next = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Next || State.Placed.Contains(Next))
                {
                    continue;
                }

                const int32 NextX = PosX + WidthOf(Node) + State.ColumnGap + FeederSpanOf(Next, State);
                const int32 StraightY = PosY + PinOffsetY(Node, Pin) - PinOffsetY(Next, Linked);
                const int32 NextY = bFirstBranch ? StraightY : Bottom + State.RowGap;
                bFirstBranch = false;

                Bottom = FMath::Max(Bottom, PlaceExecChain(Next, NextX, NextY, State));
            }
        }

        return Bottom;
    }

    // Pose runs into the sink, so the graph is walked right to left, pose inputs taking the next column
    // left with siblings stacked. RightEdge is where the node's right side lands, not its origin.
    int32 PlacePoseChain(UEdGraphNode* Node, int32 RightEdge, int32 TopY, FArrangeState& State)
    {
        State.Placed.Add(Node);
        Node->NodePosX = RightEdge - WidthOf(Node);
        Node->NodePosY = TopY;

        // Measured before the subtree is placed, since a placed feeder reports no span.
        const int32 DataSpan = FeederSpanOf(Node, State);
        int32 Bottom = PlaceFeeders(Node, State);
        const int32 ColumnRight = Node->NodePosX - DataSpan - State.ColumnGap;

        int32 RowTop = TopY;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinVisible(Pin) || Pin->Direction != EGPD_Input || ClassifyPin(Pin) != EFlowKind::Pose)
            {
                continue;
            }

            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Feeder = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Feeder || State.Placed.Contains(Feeder))
                {
                    continue;
                }

                const int32 FeederBottom = PlacePoseChain(Feeder, ColumnRight, RowTop, State);
                RowTop = FeederBottom + State.RowGap;
                Bottom = FMath::Max(Bottom, FeederBottom);
            }
        }

        return Bottom;
    }

    enum class EGraphFlow : uint8
    {
        Exec,
        Pose,
        StateMachine
    };

    EGraphFlow FlowOfGraph(const UEdGraph* Graph)
    {
        if (Graph && Graph->IsA<UAnimationStateMachineGraph>())
        {
            return EGraphFlow::StateMachine;
        }

        if (Graph && Graph->IsA<UAnimationGraph>())
        {
            return EGraphFlow::Pose;
        }

        return EGraphFlow::Exec;
    }

    // One terminal per pose graph, and the four terminal classes share no base, so they are named.
    UEdGraphNode* FindPoseSink(UEdGraph* Graph)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const bool bIsTerminal = Node && (Node->IsA<UAnimGraphNode_Root>() || Node->IsA<UAnimGraphNode_StateResult>() || Node->IsA<UAnimGraphNode_TransitionResult>() || Node->IsA<UAnimGraphNode_BlendSpaceSampleResult>());
            if (bIsTerminal)
            {
                return Node;
            }
        }

        return nullptr;
    }

    UEdGraphNode* FindMachineEntry(UEdGraph* Graph)
    {
        UAnimationStateMachineGraph* MachineGraph = Cast<UAnimationStateMachineGraph>(Graph);
        if (MachineGraph && MachineGraph->EntryNode)
        {
            return MachineGraph->EntryNode;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->IsA<UAnimStateEntryNode>())
            {
                return Node;
            }
        }

        return nullptr;
    }

    bool HasLinkedPoseOutput(UEdGraphNode* Node)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            const bool bFeedsPose = IsPinVisible(Pin) && Pin->Direction == EGPD_Output && ClassifyPin(Pin) == EFlowKind::Pose;
            if (bFeedsPose && Pin->LinkedTo.Num() > 0)
            {
                return true;
            }
        }

        return false;
    }

    // A state's wire leaves through a transition, so its successor is two hops away. The entry node
    // wires straight into its first state instead.
    void GatherStateSuccessors(UEdGraphNode* Node, TArray<UEdGraphNode*>& OutSuccessors)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }

            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Hop = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Hop)
                {
                    continue;
                }

                UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Hop);
                if (!Transition)
                {
                    OutSuccessors.AddUnique(Hop);
                    continue;
                }

                if (UAnimStateNodeBase* Next = Transition->GetNextState())
                {
                    OutSuccessors.AddUnique(Next);
                }
            }
        }
    }

    struct FLayoutOpParams
    {
        UEdGraph* Graph = nullptr;
        int32 Spacing = 80;
        int32 RowSpacing = 48;
        int32 PosX = 0;
        int32 PosY = 0;
    };

    // Every sink starts on the same left edge, so together they read as the output column.
    void PlacePoseRow(UEdGraphNode* Sink, FArrangeState& State, const FLayoutOpParams& Params, int32& InOutRowTop)
    {
        InOutRowTop = PlacePoseChain(Sink, WidthOf(Sink), InOutRowTop, State) + Params.RowSpacing;
    }

    // Each tree opens a row under the last one. Laying out from the output column going left leaves the
    // graph in negative space, so one pass slides all of it onto PosX.
    void ArrangePose(const TArray<UEdGraphNode*>& Sinks, const FLayoutOpParams& Params, FArrangeState& State)
    {
        int32 RowTop = Params.PosY;
        for (UEdGraphNode* Sink : Sinks)
        {
            if (State.Placed.Contains(Sink))
            {
                continue;
            }

            PlacePoseRow(Sink, State, Params, RowTop);
        }

        if (Params.Graph)
        {
            // A cached pose is a sink no walk from the root reaches. Seeding a node that still feeds a pose
            // would place it ahead of its consumer, so terminals go first and the leftovers follow.
            for (UEdGraphNode* Stray : Params.Graph->Nodes)
            {
                if (!Stray || State.Placed.Contains(Stray) || HasLinkedPoseOutput(Stray))
                {
                    continue;
                }

                PlacePoseRow(Stray, State, Params, RowTop);
            }

            for (UEdGraphNode* Stray : Params.Graph->Nodes)
            {
                if (!Stray || State.Placed.Contains(Stray))
                {
                    continue;
                }

                PlacePoseRow(Stray, State, Params, RowTop);
            }
        }

        int32 LeftMost = MAX_int32;
        for (const UEdGraphNode* Node : State.Placed)
        {
            LeftMost = FMath::Min(LeftMost, Node->NodePosX);
        }

        if (LeftMost == MAX_int32)
        {
            return;
        }

        const int32 Shift = Params.PosX - LeftMost;
        for (UEdGraphNode* Node : State.Placed)
        {
            Node->NodePosX += Shift;
        }
    }

    // Transitions carry the edges, so the layers come from walking them while the transition nodes
    // themselves are left exactly where they were.
    void ArrangeStateMachine(const TArray<UEdGraphNode*>& Entries, const FLayoutOpParams& Params)
    {
        TSet<UEdGraphNode*> Placed;
        TArray<UEdGraphNode*> Layer;
        for (UEdGraphNode* Entry : Entries)
        {
            if (Entry && !IsTransitionNode(Entry))
            {
                Layer.AddUnique(Entry);
            }
        }

        int32 ColumnX = Params.PosX;
        int32 StrayCursor = 0;
        while (true)
        {
            // What no transition reaches still has to land somewhere, one column past the last layer.
            while (Layer.IsEmpty() && Params.Graph && Params.Graph->Nodes.IsValidIndex(StrayCursor))
            {
                UEdGraphNode* Stray = Params.Graph->Nodes[StrayCursor];
                ++StrayCursor;
                if (Stray && !IsTransitionNode(Stray) && !Placed.Contains(Stray))
                {
                    Layer.Add(Stray);
                }
            }

            if (Layer.IsEmpty())
            {
                return;
            }

            int32 RowY = Params.PosY;
            int32 ColumnWidth = 0;
            for (UEdGraphNode* Node : Layer)
            {
                Placed.Add(Node);
                Node->NodePosX = ColumnX;
                Node->NodePosY = RowY;
                RowY += HeightOf(Node) + Params.RowSpacing;
                ColumnWidth = FMath::Max(ColumnWidth, WidthOf(Node));
            }
            ColumnX += ColumnWidth + Params.Spacing;

            TArray<UEdGraphNode*> Next;
            for (UEdGraphNode* Node : Layer)
            {
                TArray<UEdGraphNode*> Successors;
                GatherStateSuccessors(Node, Successors);
                for (UEdGraphNode* Successor : Successors)
                {
                    if (!Placed.Contains(Successor))
                    {
                        Next.AddUnique(Successor);
                    }
                }
            }

            Layer = MoveTemp(Next);
        }
    }

    bool ApplyOperation(const FString& Op, const TArray<UEdGraphNode*>& Nodes, const FLayoutOpParams& Params)
    {
        if (Nodes.IsEmpty())
        {
            return true;
        }

        if (Op == TEXT("Arrange"))
        {
            FArrangeState State;
            State.ColumnGap = Params.Spacing;
            State.RowGap = Params.RowSpacing;

            const EGraphFlow Flow = FlowOfGraph(Params.Graph);
            if (Flow == EGraphFlow::StateMachine)
            {
                ArrangeStateMachine(Nodes, Params);
                return true;
            }

            if (Flow == EGraphFlow::Pose)
            {
                ArrangePose(Nodes, Params, State);
                return true;
            }

            int32 RowTop = Params.PosY;
            for (UEdGraphNode* Root : Nodes)
            {
                if (State.Placed.Contains(Root))
                {
                    continue;
                }

                RowTop = PlaceExecChain(Root, Params.PosX + FeederSpanOf(Root, State), RowTop, State) + Params.RowSpacing;
            }

            // Whatever the roots never reached still has to go somewhere, or it stays stacked on the
            // origin. The disabled event nodes a fresh Blueprint ships with are the usual case.
            if (Params.Graph)
            {
                for (UEdGraphNode* Stray : Params.Graph->Nodes)
                {
                    if (!Stray || State.Placed.Contains(Stray))
                    {
                        continue;
                    }

                    RowTop = PlaceExecChain(Stray, Params.PosX + FeederSpanOf(Stray, State), RowTop, State) + Params.RowSpacing;
                }
            }

            return true;
        }

        // Editor equivalent of selecting the nodes and pressing Q. The leftmost holds still, a node with
        // several links lands on their average since one Y cannot satisfy all.
        if (Op == TEXT("Straighten"))
        {
            TArray<UEdGraphNode*> Ordered = Nodes;
            Ordered.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodePosX < B.NodePosX; });

            const TSet<UEdGraphNode*> Selected(Nodes);
            TSet<UEdGraphNode*> Anchored;

            for (UEdGraphNode* Node : Ordered)
            {
                Anchored.Add(Node);

                TMap<UEdGraphNode*, TArray<TPair<UEdGraphPin*, UEdGraphPin*>>> Partners;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (!IsPinVisible(Pin))
                    {
                        continue;
                    }

                    for (UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        UEdGraphNode* Partner = Linked ? Linked->GetOwningNode() : nullptr;
                        if (Partner && Selected.Contains(Partner) && !Anchored.Contains(Partner))
                        {
                            Partners.FindOrAdd(Partner).Emplace(Pin, Linked);
                        }
                    }
                }

                for (const TPair<UEdGraphNode*, TArray<TPair<UEdGraphPin*, UEdGraphPin*>>>& Partner : Partners)
                {
                    int32 DeltaSum = 0;
                    for (const TPair<UEdGraphPin*, UEdGraphPin*>& PinPair : Partner.Value)
                    {
                        DeltaSum += (Node->NodePosY + PinOffsetY(Node, PinPair.Key)) - (Partner.Key->NodePosY + PinOffsetY(Partner.Key, PinPair.Value));
                    }

                    Partner.Key->NodePosY += DeltaSum / Partner.Value.Num();
                    Anchored.Add(Partner.Key);
                }
            }

            return true;
        }

        if (Op == TEXT("Move"))
        {
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosX = Params.PosX;
                Node->NodePosY = Params.PosY;
            }
            return true;
        }

        if (Op == TEXT("AlignLeft"))
        {
            int32 Target = Nodes[0]->NodePosX;
            for (const UEdGraphNode* Node : Nodes)
            {
                Target = FMath::Min(Target, Node->NodePosX);
            }
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosX = Target;
            }
            return true;
        }

        if (Op == TEXT("AlignRight"))
        {
            int32 Target = Nodes[0]->NodePosX + WidthOf(Nodes[0]);
            for (const UEdGraphNode* Node : Nodes)
            {
                Target = FMath::Max(Target, Node->NodePosX + WidthOf(Node));
            }
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosX = Target - WidthOf(Node);
            }
            return true;
        }

        if (Op == TEXT("AlignTop"))
        {
            int32 Target = Nodes[0]->NodePosY;
            for (const UEdGraphNode* Node : Nodes)
            {
                Target = FMath::Min(Target, Node->NodePosY);
            }
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosY = Target;
            }
            return true;
        }

        if (Op == TEXT("AlignBottom"))
        {
            int32 Target = Nodes[0]->NodePosY + HeightOf(Nodes[0]);
            for (const UEdGraphNode* Node : Nodes)
            {
                Target = FMath::Max(Target, Node->NodePosY + HeightOf(Node));
            }
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosY = Target - HeightOf(Node);
            }
            return true;
        }

        if (Op == TEXT("AlignCenterX"))
        {
            int32 Sum = 0;
            for (const UEdGraphNode* Node : Nodes)
            {
                Sum += Node->NodePosX + WidthOf(Node) / 2;
            }
            const int32 Center = Sum / Nodes.Num();
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosX = Center - WidthOf(Node) / 2;
            }
            return true;
        }

        if (Op == TEXT("AlignCenterY"))
        {
            int32 Sum = 0;
            for (const UEdGraphNode* Node : Nodes)
            {
                Sum += Node->NodePosY + HeightOf(Node) / 2;
            }
            const int32 Center = Sum / Nodes.Num();
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosY = Center - HeightOf(Node) / 2;
            }
            return true;
        }

        if (Op == TEXT("StackHorizontal"))
        {
            int32 Cursor = Nodes[0]->NodePosX;
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosX = Cursor;
                Node->NodePosY = Nodes[0]->NodePosY;
                Cursor += WidthOf(Node) + Params.Spacing;
            }
            return true;
        }

        if (Op == TEXT("StackVertical"))
        {
            int32 Cursor = Nodes[0]->NodePosY;
            for (UEdGraphNode* Node : Nodes)
            {
                Node->NodePosY = Cursor;
                Node->NodePosX = Nodes[0]->NodePosX;
                Cursor += HeightOf(Node) + Params.Spacing;
            }
            return true;
        }

        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("Unknown layout op '%s'"), *Op);
        return false;
    }

    class FBlueprintLayoutWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Layout");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Layout must be an array of operations"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: layout operation needs Op and Nodes"), *Context.AssetPath);
                    return false;
                }

                UEdGraph* OpGraph = ResolveGraph(Context, Desc);
                if (!OpGraph)
                {
                    return false;
                }

                FLayoutOpParams Params;
                Params.Graph = OpGraph;
                Desc->TryGetNumberField(TEXT("Spacing"), Params.Spacing);
                Desc->TryGetNumberField(TEXT("RowSpacing"), Params.RowSpacing);
                Desc->TryGetNumberField(TEXT("PosX"), Params.PosX);
                Desc->TryGetNumberField(TEXT("PosY"), Params.PosY);

                TArray<UEdGraphNode*> Nodes;
                if (!ResolveNodes(Context, Desc, Op, OpGraph, Nodes))
                {
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: %s on %d node(s)"), *Context.Blueprint->GetName(), *Op, Nodes.Num());
                ++Context.Ops;

                if (!ApplyOperation(Op, Nodes, Params))
                {
                    return false;
                }
            }

            return true;
        }

    private:
        // Every rule graph the engine makes is called Transition and is unique only inside its own node, so
        // a name reaches an arbitrary one. Machine plus the state pair identifies it.
        UEdGraph* ResolveGraph(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc)
        {
            FString MachineName;
            const bool bNamesMachine = Desc->TryGetStringField(TEXT("Machine"), MachineName);

            const TSharedPtr<FJsonObject>* TransitionDesc = nullptr;
            const bool bNamesTransition = Desc->TryGetObjectField(TEXT("Transition"), TransitionDesc);

            if (bNamesTransition && !bNamesMachine)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: layout Transition needs a Machine beside it"), *Context.AssetPath);
                return nullptr;
            }

            if (bNamesMachine)
            {
                UAnimationStateMachineGraph* MachineGraph = BlueprintEdit::FindMachine(Context, MachineName);
                if (!MachineGraph || !bNamesTransition)
                {
                    return MachineGraph;
                }

                FString FromName;
                FString ToName;
                if (!(*TransitionDesc)->TryGetStringField(TEXT("From"), FromName) || !(*TransitionDesc)->TryGetStringField(TEXT("To"), ToName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: layout Transition needs From and To inside it. Present: %s"), *Context.AssetPath, *BlueprintEdit::DescribeTransitions(MachineGraph));
                    return nullptr;
                }

                UAnimStateTransitionNode* TransitionNode = BlueprintEdit::ResolveTransition(Context, MachineGraph, FromName, ToName, *TransitionDesc);
                if (!TransitionNode)
                {
                    return nullptr;
                }

                if (!TransitionNode->BoundGraph)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: transition '%s' to '%s' carries no rule graph"), *Context.AssetPath, *FromName, *ToName);
                }

                return TransitionNode->BoundGraph;
            }

            FString GraphName;
            if (Desc->TryGetStringField(TEXT("Graph"), GraphName))
            {
                UEdGraph* Named = BlueprintEdit::FindGraph(Context.Blueprint, GraphName);
                if (!Named)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no graph named '%s'"), *Context.AssetPath, *GraphName);
                }

                return Named;
            }

            if (!Context.Graph)
            {
                Context.Graph = BlueprintEdit::FindGraph(Context.Blueprint, TEXT("EventGraph"));
                if (!Context.Graph)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no graph named 'EventGraph'"), *Context.AssetPath);
                    return nullptr;
                }

                BlueprintEdit::RegisterExistingNodes(Context.Graph, Context.NodesById);
            }

            return Context.Graph;
        }

        // Geometry only moves nodes of the target graph, so its own Nodes are the map, without the sub
        // graphs a state machine hides. Ids the Graph writer handed out still resolve, held to that graph.
        const TMap<FString, UEdGraphNode*>& NodeMapFor(UEdGraph* Graph)
        {
            if (TMap<FString, UEdGraphNode*>* Cached = m_GraphNodes.Find(Graph))
            {
                return *Cached;
            }

            TMap<FString, UEdGraphNode*>& Fresh = m_GraphNodes.Add(Graph);
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node)
                {
                    Fresh.Add(Node->NodeGuid.ToString(EGuidFormats::Digits), Node);
                }
            }

            return Fresh;
        }

        bool ResolveNodes(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Op, UEdGraph* Graph, TArray<UEdGraphNode*>& OutNodes)
        {
            const TArray<TSharedPtr<FJsonValue>>* NodeIds = nullptr;
            if (!Desc->TryGetArrayField(TEXT("Nodes"), NodeIds))
            {
                return ResolveDefaultEntries(Context, Op, Graph, OutNodes);
            }

            const TMap<FString, UEdGraphNode*>& OwnNodes = NodeMapFor(Graph);
            for (const TSharedPtr<FJsonValue>& IdValue : *NodeIds)
            {
                const FString Id = IdValue->AsString();
                UEdGraphNode* const* Found = OwnNodes.Find(Id);
                if (!Found)
                {
                    Found = Context.NodesById.Find(Id);
                }

                if (!Found)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no node '%s'"), *Context.AssetPath, *Id);
                    return false;
                }

                UEdGraphNode* Node = *Found;
                if (IsTransitionNode(Node))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is a transition, which draws on the wire between two states and is never laid out"), *Context.AssetPath, *Id);
                    return false;
                }

                if (Node->GetGraph() != Graph)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' belongs to graph '%s', not to '%s'"), *Context.AssetPath, *Id, *Node->GetGraph()->GetName(), *Graph->GetName());
                    return false;
                }

                OutNodes.Add(Node);
            }

            return true;
        }

        // Arrange reads Nodes as entry points, and a pose graph or a state machine already names its own.
        bool ResolveDefaultEntries(const FBlueprintEditContext& Context, const FString& Op, UEdGraph* Graph, TArray<UEdGraphNode*>& OutNodes)
        {
            if (Op != TEXT("Arrange"))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: layout operation needs Op and Nodes"), *Context.AssetPath);
                return false;
            }

            const EGraphFlow Flow = FlowOfGraph(Graph);
            if (Flow == EGraphFlow::Pose)
            {
                UEdGraphNode* Sink = FindPoseSink(Graph);
                if (!Sink)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' carries no result node to arrange from"), *Context.AssetPath, *Graph->GetName());
                    return false;
                }

                OutNodes.Add(Sink);
                return true;
            }

            if (Flow == EGraphFlow::StateMachine)
            {
                UEdGraphNode* Entry = FindMachineEntry(Graph);
                if (!Entry)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' carries no entry node to arrange from"), *Context.AssetPath, *Graph->GetName());
                    return false;
                }

                OutNodes.Add(Entry);
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Arrange on '%s' needs Nodes, an exec graph has no single entry point"), *Context.AssetPath, *Graph->GetName());
            return false;
        }

        TMap<UEdGraph*, TMap<FString, UEdGraphNode*>> m_GraphNodes;
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintLayoutWriter()
{
    return MakeUnique<FBlueprintLayoutWriter>();
}
