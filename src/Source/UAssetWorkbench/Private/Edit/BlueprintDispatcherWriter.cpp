#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"

namespace
{
    using BlueprintEdit::FSignatureEntry;

    // A dispatcher is a multicast delegate member variable plus a signature graph of the same name.
    // Either half alone is a broken dispatcher, so every op addresses both.
    UEdGraph* FindSignatureGraph(const UBlueprint* Blueprint, const FName Name)
    {
        for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
        {
            if (Graph && Graph->GetFName() == Name)
            {
                return Graph;
            }
        }

        return nullptr;
    }

    FString DescribeDispatchers(const UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        for (const UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
        {
            if (Graph)
            {
                Names.Add(Graph->GetName());
            }
        }

        if (Names.IsEmpty())
        {
            return TEXT("<none>");
        }

        return FString::Join(Names, TEXT(", "));
    }

    UK2Node_FunctionEntry* FindEntryNode(const UEdGraph* Graph)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                return Entry;
            }
        }

        return nullptr;
    }

    class FBlueprintDispatcherWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Dispatchers");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Dispatchers must be an array of ops"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                FString Name;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Name"), Name))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Dispatchers entry needs Op and Name"), *Context.AssetPath);
                    return false;
                }

                if (!ApplyOp(Context, Desc, Op, Name))
                {
                    return false;
                }

                ++Context.Ops;
                Context.bNeedsStructuralRecompile = true;
            }

            return true;
        }

    private:
        bool ApplyOp(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Op, const FString& Name) const
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Desc, Name);
            }

            if (Op == TEXT("Rename"))
            {
                return ApplyRename(Context, Desc, Name);
            }

            if (Op == TEXT("Modify"))
            {
                return ApplyModify(Context, Desc, Name);
            }

            if (Op == TEXT("Remove"))
            {
                return ApplyRemove(Context, Name);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown dispatcher op '%s'. Expected Add, Rename, Modify or Remove"), *Context.AssetPath, *Op);
            return false;
        }

        bool ValidateNewName(const FBlueprintEditContext& Context, const FString& Name) const
        {
            FKismetNameValidator Validator(Context.Blueprint);
            const EValidatorResult Result = Validator.IsValid(Name);
            if (Result == EValidatorResult::Ok)
            {
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is not a usable dispatcher name (%s). Dispatchers: %s"), *Context.AssetPath, *Name, *INameValidatorInterface::GetErrorString(Name, Result), *DescribeDispatchers(Context.Blueprint));
            return false;
        }

        UEdGraph* RequireSignatureGraph(const FBlueprintEditContext& Context, const FString& Name) const
        {
            UEdGraph* Graph = FindSignatureGraph(Context.Blueprint, FName(*Name));
            if (!Graph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no event dispatcher named '%s'. Present: %s"), *Context.AssetPath, *Name, *DescribeDispatchers(Context.Blueprint));
            }

            return Graph;
        }

        bool ApplyAdd(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Name) const
        {
            if (!ValidateNewName(Context, Name))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + dispatcher '%s'"), *Context.Blueprint->GetName(), *Name);

            const FName DispatcherName(*Name);

            FEdGraphPinType DelegateType;
            DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
            if (!FBlueprintEditorUtils::AddMemberVariable(Context.Blueprint, DispatcherName, DelegateType))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddMemberVariable failed for dispatcher '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Context.Blueprint, DispatcherName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            if (!Graph)
            {
                FBlueprintEditorUtils::RemoveMemberVariable(Context.Blueprint, DispatcherName);
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: CreateNewGraph failed for dispatcher '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            // The signature graph is a declaration, the editor never lets anyone drop nodes into it.
            Graph->bEditable = false;

            const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
            Schema->CreateDefaultNodesForGraph(*Graph);
            Schema->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
            Schema->AddExtraFunctionFlags(Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
            Schema->MarkFunctionEntryAsEditable(Graph, true);

            Context.Blueprint->DelegateSignatureGraphs.Add(Graph);

            FString Category;
            if (Desc->TryGetStringField(TEXT("Category"), Category))
            {
                FBlueprintEditorUtils::SetBlueprintVariableCategory(Context.Blueprint, DispatcherName, nullptr, FText::FromString(Category), /* bDontRecompile */ true);
            }

            const TSharedPtr<FJsonObject>* Signature = nullptr;
            if (Desc->TryGetObjectField(TEXT("Signature"), Signature) && !ApplySignature(Context, Graph, *Signature))
            {
                return false;
            }

            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
            RegisterEntry(Context, Desc, Graph);
            return true;
        }

        // Same convention the Functions writer uses: the spec Id answers to the graph's entry node, which is
        // where a dispatcher's parameters live.
        void RegisterEntry(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, UEdGraph* Graph) const
        {
            FString Id;
            if (!Desc->TryGetStringField(TEXT("Id"), Id))
            {
                return;
            }

            if (UK2Node_FunctionEntry* Entry = FindEntryNode(Graph))
            {
                Context.NodesById.Add(Id, Entry);
            }
        }

        bool ApplyRename(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Name) const
        {
            FString NewName;
            if (!Desc->TryGetStringField(TEXT("NewName"), NewName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: dispatcher Rename needs a NewName"), *Context.AssetPath);
                return false;
            }

            if (!RequireSignatureGraph(Context, Name))
            {
                return false;
            }

            if (!ValidateNewName(Context, NewName))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: rename dispatcher '%s' to '%s'"), *Context.Blueprint->GetName(), *Name, *NewName);

            // RenameMemberVariable carries the signature graph and the bound event nodes along with the variable.
            FBlueprintEditorUtils::RenameMemberVariable(Context.Blueprint, FName(*Name), FName(*NewName));
            return true;
        }

        bool ApplyModify(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Name) const
        {
            const TSharedPtr<FJsonObject>* Signature = nullptr;
            if (!Desc->TryGetObjectField(TEXT("Signature"), Signature))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: dispatcher Modify needs a Signature"), *Context.AssetPath);
                return false;
            }

            UEdGraph* Graph = RequireSignatureGraph(Context, Name);
            if (!Graph)
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: signature of dispatcher '%s'"), *Context.Blueprint->GetName(), *Name);

            if (!ApplySignature(Context, Graph, *Signature))
            {
                return false;
            }

            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
            RegisterEntry(Context, Desc, Graph);
            return true;
        }

        bool ApplyRemove(FBlueprintEditContext& Context, const FString& Name) const
        {
            UEdGraph* Graph = RequireSignatureGraph(Context, Name);
            if (!Graph)
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove dispatcher '%s'"), *Context.Blueprint->GetName(), *Name);

            // Nodes on the removed dispatcher survive as error nodes, a spec that drops one clears them in
            // the same run. The editor also repairs other loaded assets here, which a commandlet must not.
            FBlueprintEditorUtils::RemoveMemberVariable(Context.Blueprint, Graph->GetFName());
            FBlueprintEditorUtils::RemoveGraph(Context.Blueprint, Graph, EGraphRemoveFlags::None);
            return true;
        }

        bool ApplySignature(const FBlueprintEditContext& Context, UEdGraph* Graph, const TSharedPtr<FJsonObject>& Signature) const
        {
            if (Signature->HasField(TEXT("Outputs")))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: dispatcher '%s' cannot carry Outputs, a multicast delegate only takes parameters"), *Context.AssetPath, *Graph->GetName());
                return false;
            }

            UK2Node_FunctionEntry* Entry = FindEntryNode(Graph);
            if (!Entry)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: dispatcher '%s' has no entry node to carry a signature"), *Context.AssetPath, *Graph->GetName());
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
            if (!Signature->TryGetArrayField(TEXT("Inputs"), Inputs))
            {
                return true;
            }

            TArray<FSignatureEntry> InputEntries;
            if (!BlueprintEdit::ReadSignatureEntries(Context, *Inputs, TEXT("Inputs"), InputEntries))
            {
                return false;
            }

            if (!BlueprintEdit::ApplyPinShape(Context, Entry, InputEntries, EGPD_Output))
            {
                return false;
            }

            BlueprintEdit::ReconstructTerminator(Entry);
            return BlueprintEdit::ApplyPinDefaults(Context, Entry, InputEntries);
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintDispatcherWriter()
{
    return MakeUnique<FBlueprintDispatcherWriter>();
}
