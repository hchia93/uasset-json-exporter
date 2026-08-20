#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"

namespace
{
    // NodeWidth / NodeHeight are only filled once Slate has drawn a node, so a headless run reads
    // zero on everything the Graph writer just made. A flat fallback constant guarantees overlap for
    // every node taller than it, so the extent is estimated from what drives the real one: a title
    // bar plus one row per visible pin, and a width floor the title can push past.
    //
    // The numbers are fitted against a graph the editor itself straightened, which is the only way to
    // get them: a title's second line ("Target is Light Component" under a call, "Math Expression"
    // under an expression) is worth half a row, and missing that put every single-title node off by
    // one line's worth.
    constexpr int32 kTitleLineHeight = 16;
    constexpr int32 kTitleBarHeight = 32;
    constexpr int32 kPinRowHeight = 32;
    constexpr int32 kCompactNodeHeight = 52;
    constexpr int32 kMinNodeWidth = 200;
    constexpr int32 kTitleCharWidth = 8;

    bool IsPinVisible(const UEdGraphPin* Pin)
    {
        return Pin && !Pin->bHidden && !Pin->bAdvancedView;
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

    int32 WidthOf(const UEdGraphNode* Node)
    {
        if (Node->NodeWidth > 0)
        {
            return Node->NodeWidth;
        }

        const int32 TitleWidth = Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Len() * kTitleCharWidth;
        return FMath::Max(kMinNodeWidth, TitleWidth);
    }

    int32 HeightOf(const UEdGraphNode* Node)
    {
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

        return TitleHeightOf(Node) + FMath::Max(InputRows, OutputRows) * kPinRowHeight;
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

    bool IsExecPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    }

    // Nodes wired into this one's data inputs. Exec links are the spine and are walked separately.
    void GatherFeeders(UEdGraphNode* Node, TArray<UEdGraphNode*>& OutFeeders)
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinVisible(Pin) || Pin->Direction != EGPD_Input || IsExecPin(Pin))
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

    // Lays a node's data inputs out to its left. Every feeder gets the Y that puts its own pin on the
    // same line as the pin it feeds, so the wire comes out flat, and siblings step further left past
    // each other's whole subtree rather than stacking downward. Nothing has to give up its line that
    // way, and a single-input chain reads as one horizontal lane. Returns the lowest Y consumed.
    int32 PlaceFeeders(UEdGraphNode* Node, FArrangeState& State)
    {
        int32 Bottom = Node->NodePosY + HeightOf(Node);
        int32 LeftEdge = Node->NodePosX;

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinVisible(Pin) || Pin->Direction != EGPD_Input || IsExecPin(Pin))
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

    // Walks the exec chain left to right. The first exec output continues the current row, every
    // further output starts a new row under everything placed so far, which is what a Branch does.
    // Continuing a row still straightens: a node whose title carries a second line puts its exec pin
    // a half row lower than a plain one, so a flat run is not a shared NodePosY.
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

    struct FLayoutOpParams
    {
        UEdGraph* Graph = nullptr;
        int32 Spacing = 80;
        int32 RowSpacing = 48;
        int32 PosX = 0;
        int32 PosY = 0;
    };

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

        // Editor equivalent: select the nodes and press Q. The leftmost node holds still and every
        // node it reaches slides vertically until the pins on each end of a wire share a line. A node
        // with several links lands on the average, since one Y cannot satisfy all of them.
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
                const TArray<TSharedPtr<FJsonValue>>* NodeIds = nullptr;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetArrayField(TEXT("Nodes"), NodeIds))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: layout operation needs Op and Nodes"), *Context.AssetPath);
                    return false;
                }

                UEdGraph* OpGraph = nullptr;
                const TMap<FString, UEdGraphNode*>* NodesById = ResolveNodeMap(Context, Desc, OpGraph);
                if (!NodesById)
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
                for (const TSharedPtr<FJsonValue>& IdValue : *NodeIds)
                {
                    const FString Id = IdValue->AsString();
                    UEdGraphNode* const* Found = NodesById->Find(Id);
                    if (!Found)
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no node '%s'"), *Context.AssetPath, *Id);
                        return false;
                    }
                    Nodes.Add(*Found);
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: %s on %d node(s)"), *Context.Blueprint->GetName(), *Op, Nodes.Num());
                ++Context.Ops;

                if (!Context.bApply)
                {
                    continue;
                }

                if (!ApplyOperation(Op, Nodes, Params))
                {
                    return false;
                }
            }

            return true;
        }

    private:
        // Ops on the graph the Graph writer already touched reuse its map, which lets a layout
        // address the spec Ids from Graph.Nodes. Any other graph gets its own NodeGuid-only map.
        const TMap<FString, UEdGraphNode*>* ResolveNodeMap(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, UEdGraph*& OutGraph)
        {
            FString GraphName;
            const bool bNamed = Desc->TryGetStringField(TEXT("Graph"), GraphName);

            if (!bNamed || (Context.Graph && Context.Graph->GetName() == GraphName))
            {
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
                OutGraph = Context.Graph;
                return &Context.NodesById;
            }

            if (TMap<FString, UEdGraphNode*>* Cached = OtherGraphs.Find(GraphName))
            {
                OutGraph = BlueprintEdit::FindGraph(Context.Blueprint, GraphName);
                return Cached;
            }

            UEdGraph* Other = BlueprintEdit::FindGraph(Context.Blueprint, GraphName);
            if (!Other)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no graph named '%s'"), *Context.AssetPath, *GraphName);
                return nullptr;
            }

            TMap<FString, UEdGraphNode*>& Fresh = OtherGraphs.Add(GraphName);
            BlueprintEdit::RegisterExistingNodes(Other, Fresh);
            OutGraph = Other;
            return &Fresh;
        }

        TMap<FString, TMap<FString, UEdGraphNode*>> OtherGraphs;
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintLayoutWriter()
{
    return MakeUnique<FBlueprintLayoutWriter>();
}
