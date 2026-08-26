#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"

namespace
{
    using BlueprintEdit::FSignatureEntry;

    UEdGraph* FindFunctionGraph(const UBlueprint* Blueprint, const FString& Name)
    {
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetName() == Name)
            {
                return Graph;
            }
        }

        return nullptr;
    }

    FString DescribeFunctionGraphs(const UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph)
            {
                Names.Add(Graph->GetName());
            }
        }

        return FString::Join(Names, TEXT(", "));
    }

    FString DescribeNameOwner(UBlueprint* Blueprint, const FString& Name)
    {
        TArray<FString> Owners;

        if (FindFunctionGraph(Blueprint, Name))
        {
            Owners.Add(TEXT("a function graph"));
        }

        for (const UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph && Graph->GetName() == Name)
            {
                Owners.Add(TEXT("a macro graph"));
                break;
            }
        }

        for (const UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (Graph && Graph->GetName() == Name)
            {
                Owners.Add(TEXT("an event graph"));
                break;
            }
        }

        if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Name)) != INDEX_NONE)
        {
            Owners.Add(TEXT("a variable"));
        }

        if (Blueprint->ParentClass && Blueprint->ParentClass->FindFunctionByName(FName(*Name)))
        {
            Owners.Add(Blueprint->ParentClass->GetName());
        }

        if (Owners.IsEmpty())
        {
            Owners.Add(TEXT("something else in the blueprint"));
        }

        return FString::Join(Owners, TEXT(" and "));
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

    UK2Node_FunctionResult* FindResultNode(const UEdGraph* Graph)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
            {
                return Result;
            }
        }

        return nullptr;
    }

    class FBlueprintFunctionWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Functions");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Functions must be an array of ops"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                FString Name;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Name"), Name))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Functions entry needs Op and Name"), *Context.AssetPath);
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

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown function op '%s'. Expected Add, Rename, Modify or Remove"), *Context.AssetPath, *Op);
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

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is not a usable function name (%s), it is held by %s. Functions: %s"), *Context.AssetPath, *Name, *INameValidatorInterface::GetErrorString(Name, Result), *DescribeNameOwner(Context.Blueprint, Name), *DescribeFunctionGraphs(Context.Blueprint));
            return false;
        }

        bool ApplyAdd(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Name) const
        {
            if (!ValidateNewName(Context, Name))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + function '%s'"), *Context.Blueprint->GetName(), *Name);

            UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Context.Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            if (!Graph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: CreateNewGraph failed for '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            FBlueprintEditorUtils::AddFunctionGraph<UClass>(Context.Blueprint, Graph, /* bIsUserCreated */ true, static_cast<UClass*>(nullptr));

            const TSharedPtr<FJsonObject>* Signature = nullptr;
            if (Desc->TryGetObjectField(TEXT("Signature"), Signature) && !ApplySignature(Context, Graph, *Signature))
            {
                return false;
            }

            RegisterTerminators(Context, Desc, Graph);
            return true;
        }

        // The entry answers to the spec Id and the result to "<Id>.Result", so the Graph writer in the same
        // spec can wire into a function it has no NodeGuid for yet.
        void RegisterTerminators(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, UEdGraph* Graph) const
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

            if (UK2Node_FunctionResult* Result = FindResultNode(Graph))
            {
                Context.NodesById.Add(Id + TEXT(".Result"), Result);
            }
        }

        bool ApplyRename(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Name) const
        {
            FString NewName;
            if (!Desc->TryGetStringField(TEXT("NewName"), NewName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: function Rename needs a NewName"), *Context.AssetPath);
                return false;
            }

            UEdGraph* Graph = FindFunctionGraph(Context.Blueprint, Name);
            if (!Graph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no function graph named '%s'. Present: %s"), *Context.AssetPath, *Name, *DescribeFunctionGraphs(Context.Blueprint));
                return false;
            }

            if (!ValidateNewName(Context, NewName))
            {
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: rename function '%s' to '%s'"), *Context.Blueprint->GetName(), *Name, *NewName);

            // RenameGraph also retargets the call sites, which a plain UObject rename would leave dangling.
            FBlueprintEditorUtils::RenameGraph(Graph, NewName);
            return true;
        }

        bool ApplyModify(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Name) const
        {
            const TSharedPtr<FJsonObject>* Signature = nullptr;
            if (!Desc->TryGetObjectField(TEXT("Signature"), Signature))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: function Modify needs a Signature"), *Context.AssetPath);
                return false;
            }

            UEdGraph* Graph = FindFunctionGraph(Context.Blueprint, Name);
            if (!Graph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no function graph named '%s'. Present: %s"), *Context.AssetPath, *Name, *DescribeFunctionGraphs(Context.Blueprint));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: signature of function '%s'"), *Context.Blueprint->GetName(), *Name);

            if (!ApplySignature(Context, Graph, *Signature))
            {
                return false;
            }

            RegisterTerminators(Context, Desc, Graph);
            return true;
        }

        bool ApplyRemove(FBlueprintEditContext& Context, const FString& Name) const
        {
            UEdGraph* Graph = FindFunctionGraph(Context.Blueprint, Name);
            if (!Graph)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no function graph named '%s'. Present: %s"), *Context.AssetPath, *Name, *DescribeFunctionGraphs(Context.Blueprint));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove function '%s'"), *Context.Blueprint->GetName(), *Name);

            // Callers of a removed function survive as error nodes, so a spec that drops one is expected to
            // clear its call sites in the same run.
            FBlueprintEditorUtils::RemoveGraph(Context.Blueprint, Graph, EGraphRemoveFlags::None);
            return true;
        }

        bool ApplySignature(FBlueprintEditContext& Context, UEdGraph* Graph, const TSharedPtr<FJsonObject>& Signature) const
        {
            UK2Node_FunctionEntry* Entry = FindEntryNode(Graph);
            if (!Entry)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: function '%s' has no entry node to carry a signature"), *Context.AssetPath, *Graph->GetName());
                return false;
            }

            if (!ValidateSignatureIsOwnedHere(Context, Graph, Entry))
            {
                return false;
            }

            if (!ApplyFlags(Context, Entry, Signature))
            {
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
            const bool bHasInputs = Signature->TryGetArrayField(TEXT("Inputs"), Inputs);
            TArray<FSignatureEntry> InputEntries;
            if (bHasInputs && !BlueprintEdit::ReadSignatureEntries(Context, *Inputs, TEXT("Inputs"), InputEntries))
            {
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
            const bool bHasOutputs = Signature->TryGetArrayField(TEXT("Outputs"), Outputs);
            TArray<FSignatureEntry> OutputEntries;
            if (bHasOutputs && !BlueprintEdit::ReadSignatureEntries(Context, *Outputs, TEXT("Outputs"), OutputEntries))
            {
                return false;
            }

            if (bHasInputs && !BlueprintEdit::ApplyPinShape(Context, Entry, InputEntries, EGPD_Output))
            {
                return false;
            }

            UK2Node_FunctionResult* Result = FindResultNode(Graph);
            if (bHasOutputs)
            {
                if (!Result && OutputEntries.Num() > 0)
                {
                    Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
                    if (!Result)
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: function '%s' would not take a result node"), *Context.AssetPath, *Graph->GetName());
                        return false;
                    }
                }

                if (Result && !BlueprintEdit::ApplyPinShape(Context, Result, OutputEntries, EGPD_Input))
                {
                    return false;
                }
            }

            if (bHasInputs || bHasOutputs)
            {
                BlueprintEdit::ReconstructTerminator(Entry);
                if (Result)
                {
                    BlueprintEdit::ReconstructTerminator(Result);
                }

                // A Default parses against the pin, which only carries the new type once the node is rebuilt.
                if (bHasInputs && !BlueprintEdit::ApplyPinDefaults(Context, Entry, InputEntries))
                {
                    return false;
                }

                if (bHasOutputs && Result && !BlueprintEdit::ApplyPinDefaults(Context, Result, OutputEntries))
                {
                    return false;
                }
            }

            if (!ApplyLocalVariables(Context, Graph, Entry, Signature))
            {
                return false;
            }

            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
            return true;
        }

        // An override mirrors the parent signature on an entry node with no UserDefinedPins, so editing it
        // here would be overwritten on the next compile.
        bool ValidateSignatureIsOwnedHere(const FBlueprintEditContext& Context, const UEdGraph* Graph, UK2Node_FunctionEntry* Entry) const
        {
            if (Entry->UserDefinedPins.Num() > 0)
            {
                return true;
            }

            const FName FunctionName = Graph->GetFName();
            UClass* ParentClass = Context.Blueprint->ParentClass;
            if (ParentClass && ParentClass->FindFunctionByName(FunctionName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' overrides %s, its signature is owned there. Change the parent, or drop the override"), *Context.AssetPath, *FunctionName.ToString(), *ParentClass->GetName());
                return false;
            }

            if (FBlueprintEditorUtils::FindFunctionInImplementedInterfaces(Context.Blueprint, FunctionName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' implements an interface function, its signature is owned by the interface"), *Context.AssetPath, *FunctionName.ToString());
                return false;
            }

            return true;
        }

        bool ApplyLocalVariables(const FBlueprintEditContext& Context, UEdGraph* Graph, UK2Node_FunctionEntry* Entry, const TSharedPtr<FJsonObject>& Signature) const
        {
            const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
            if (!Signature->TryGetArrayField(TEXT("LocalVariables"), Items))
            {
                return true;
            }

            TArray<FSignatureEntry> Entries;
            if (!BlueprintEdit::ReadSignatureEntries(Context, *Items, TEXT("LocalVariables"), Entries))
            {
                return false;
            }

            TArray<FName> Doomed;
            for (const FBPVariableDescription& Local : Entry->LocalVariables)
            {
                if (!Entries.ContainsByPredicate([&Local](const FSignatureEntry& Wanted) { return Wanted.Name == Local.VarName; }))
                {
                    Doomed.Add(Local.VarName);
                }
            }

            Entry->Modify();
            for (const FName& VarName : Doomed)
            {
                // RemoveLocalVariable resolves its scope through the skeleton class, which a function created
                // in this same run does not have yet. Drop the description and its nodes directly instead.
                Entry->LocalVariables.RemoveAll([VarName](const FBPVariableDescription& Local) { return Local.VarName == VarName; });
                FBlueprintEditorUtils::RemoveVariableNodes(Context.Blueprint, VarName, /* bForSelfOnly */ true, Graph);
            }

            for (const FSignatureEntry& Wanted : Entries)
            {
                FBPVariableDescription* Existing = Entry->LocalVariables.FindByPredicate([&Wanted](const FBPVariableDescription& Local) { return Local.VarName == Wanted.Name; });
                if (!Existing)
                {
                    if (!FBlueprintEditorUtils::AddLocalVariable(Context.Blueprint, Graph, Wanted.Name, Wanted.Type, Wanted.Default))
                    {
                        UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddLocalVariable failed for '%s' in '%s'"), *Context.AssetPath, *Wanted.Name.ToString(), *Graph->GetName());
                        return false;
                    }

                    continue;
                }

                Existing->VarType = Wanted.Type;
                if (Wanted.bHasDefault)
                {
                    Existing->DefaultValue = Wanted.Default;
                }
            }

            return true;
        }

        bool ApplyFlags(const FBlueprintEditContext& Context, UK2Node_FunctionEntry* Entry, const TSharedPtr<FJsonObject>& Signature) const
        {
            int32 ExtraFlags = Entry->GetExtraFlags();

            bool bFlag = false;
            if (Signature->TryGetBoolField(TEXT("Pure"), bFlag))
            {
                ExtraFlags = bFlag ? ExtraFlags | FUNC_BlueprintPure : ExtraFlags & ~FUNC_BlueprintPure;
            }
            if (Signature->TryGetBoolField(TEXT("Const"), bFlag))
            {
                ExtraFlags = bFlag ? ExtraFlags | FUNC_Const : ExtraFlags & ~FUNC_Const;
            }
            if (Signature->TryGetBoolField(TEXT("Static"), bFlag))
            {
                ExtraFlags = bFlag ? ExtraFlags | FUNC_Static : ExtraFlags & ~FUNC_Static;
            }

            FString Access;
            if (Signature->TryGetStringField(TEXT("Access"), Access))
            {
                int32 AccessFlag = 0;
                if (Access == TEXT("Public"))
                {
                    AccessFlag = FUNC_Public;
                }
                else if (Access == TEXT("Protected"))
                {
                    AccessFlag = FUNC_Protected;
                }
                else if (Access == TEXT("Private"))
                {
                    AccessFlag = FUNC_Private;
                }
                else
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown Access '%s'. Expected Public, Protected or Private"), *Context.AssetPath, *Access);
                    return false;
                }

                ExtraFlags &= ~FUNC_AccessSpecifiers;
                ExtraFlags |= AccessFlag;
            }

            Entry->Modify();
            Entry->SetExtraFlags(ExtraFlags);

            if (Signature->TryGetBoolField(TEXT("CallInEditor"), bFlag))
            {
                Entry->MetaData.bCallInEditor = bFlag;
            }

            FString Text;
            if (Signature->TryGetStringField(TEXT("Category"), Text))
            {
                Entry->MetaData.Category = FText::FromString(Text);
            }
            if (Signature->TryGetStringField(TEXT("Keywords"), Text))
            {
                Entry->MetaData.Keywords = FText::FromString(Text);
            }
            if (Signature->TryGetStringField(TEXT("Tooltip"), Text))
            {
                Entry->MetaData.ToolTip = FText::FromString(Text);
            }

            return true;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintFunctionWriter()
{
    return MakeUnique<FBlueprintFunctionWriter>();
}
