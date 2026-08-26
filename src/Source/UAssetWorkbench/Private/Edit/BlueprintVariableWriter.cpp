#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/UnrealType.h"

namespace
{
    const TCHAR* kDialogSection = TEXT("SuppressableDialogs");
    const TCHAR* kTypeChangeDialogKey = TEXT("ChangeVariableType_Warning");

    // ChangeMemberVariableType raises a modal as soon as any node reads the variable, and neither a
    // commandlet nor the queue can answer it. Suppressed up front, user setting restored after.
    class FScopedTypeChangePrompt
    {
    public:
        FScopedTypeChangePrompt()
        {
            m_bHadSetting = GConfig->GetBool(kDialogSection, kTypeChangeDialogKey, m_bPreviousSetting, GEditorPerProjectIni);
            GConfig->SetBool(kDialogSection, kTypeChangeDialogKey, true, GEditorPerProjectIni);
        }

        ~FScopedTypeChangePrompt()
        {
            if (m_bHadSetting)
            {
                GConfig->SetBool(kDialogSection, kTypeChangeDialogKey, m_bPreviousSetting, GEditorPerProjectIni);
                return;
            }

            GConfig->RemoveKey(kDialogSection, kTypeChangeDialogKey, GEditorPerProjectIni);
        }

    private:
        bool m_bHadSetting = false;
        bool m_bPreviousSetting = false;
    };

    FString DescribeVariables(UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        for (const FBPVariableDescription& Description : Blueprint->NewVariables)
        {
            Names.Add(Description.VarName.ToString());
        }

        return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("<none>");
    }

    FString DescribeComponents(UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        if (Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node)
                {
                    Names.Add(Node->GetVariableName().ToString());
                }
            }
        }

        return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("<none>");
    }

    bool IsComponentVariable(UBlueprint* Blueprint, const FName VarName)
    {
        if (!Blueprint->SimpleConstructionScript)
        {
            return false;
        }

        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node && Node->GetVariableName() == VarName)
            {
                return true;
            }
        }

        return false;
    }

    void SetVariableMetaDataFlag(UBlueprint* Blueprint, const FName VarName, const FName& Key, bool bEnabled)
    {
        if (bEnabled)
        {
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, Key, TEXT("true"));
            return;
        }

        FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr, Key);
    }

    class FBlueprintVariableWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Variables");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Variables must be an array of operations"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                FString Name;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Name"), Name))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable operation needs Op and Name"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: variable %s %s"), *Context.Blueprint->GetName(), *Op, *Name);
                ++Context.Ops;

                if (!ApplyOne(Context, Op, Name, Desc))
                {
                    return false;
                }
            }

            Context.bNeedsStructuralRecompile = true;
            return true;
        }

    private:
        bool ApplyOne(FBlueprintEditContext& Context, const FString& Op, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            const FName VarName(*Name);

            if (Op == TEXT("Add"))
            {
                if (FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName) != INDEX_NONE)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: already has a variable named '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                FEdGraphPinType PinType;
                if (!BlueprintEdit::ResolvePinType(Context, Desc, PinType))
                {
                    return false;
                }

                FString DefaultValue;
                Desc->TryGetStringField(TEXT("Default"), DefaultValue);

                if (!FBlueprintEditorUtils::AddMemberVariable(Context.Blueprint, VarName, PinType, DefaultValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddMemberVariable failed for '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                return ApplyFields(Context, VarName, Desc);
            }

            if (Op == TEXT("Remove"))
            {
                if (FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName) == INDEX_NONE)
                {
                    LogUnknownVariable(Context, VarName);
                    return false;
                }

                FBlueprintEditorUtils::RemoveMemberVariable(Context.Blueprint, VarName);
                return true;
            }

            if (Op == TEXT("Rename"))
            {
                FString NewName;
                if (!Desc->TryGetStringField(TEXT("NewName"), NewName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable Rename needs a NewName"), *Context.AssetPath);
                    return false;
                }

                if (FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName) == INDEX_NONE)
                {
                    LogUnknownVariable(Context, VarName);
                    return false;
                }

                FBlueprintEditorUtils::RenameMemberVariable(Context.Blueprint, VarName, FName(*NewName));
                return true;
            }

            if (Op == TEXT("Modify"))
            {
                return ModifyOne(Context, VarName, Desc);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown variable Op '%s'. Accepted: Add, Modify, Remove, Rename"), *Context.AssetPath, *Op);
            return false;
        }

        bool ModifyOne(FBlueprintEditContext& Context, const FName VarName, const TSharedPtr<FJsonObject>& Desc) const
        {
            const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName);
            if (VarIndex == INDEX_NONE)
            {
                LogUnknownVariable(Context, VarName);
                return false;
            }

            FEdGraphPinType NewType = Context.Blueprint->NewVariables[VarIndex].VarType;
            bool bTouchedType = false;
            if (!BlueprintEdit::ResolvePinTypeOverrides(Context, Desc, NewType, bTouchedType))
            {
                return false;
            }

            const bool bRetyped = bTouchedType && NewType != Context.Blueprint->NewVariables[VarIndex].VarType;
            if (bRetyped)
            {
                {
                    FScopedTypeChangePrompt Prompt;
                    FBlueprintEditorUtils::ChangeMemberVariableType(Context.Blueprint, VarName, NewType);
                }

                // ChangeMemberVariableType returns void and walks away from a type it will not take,
                // so reading the type back is the only way to hear about it.
                if (Context.Blueprint->NewVariables[VarIndex].VarType != NewType)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: engine refused the new type for variable '%s'"), *Context.AssetPath, *VarName.ToString());
                    return false;
                }
            }

            if (!ApplyFields(Context, VarName, Desc))
            {
                return false;
            }

            FString Default;
            if (Desc->TryGetStringField(TEXT("Default"), Default))
            {
                return ApplyDefault(Context, VarName, Default, bRetyped);
            }

            return true;
        }

        bool ApplyFields(FBlueprintEditContext& Context, const FName VarName, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString Category;
            if (Desc->TryGetStringField(TEXT("Category"), Category))
            {
                FBlueprintEditorUtils::SetBlueprintVariableCategory(Context.Blueprint, VarName, nullptr, FText::FromString(Category), /* bDontRecompile */ true);
            }

            FString Tooltip;
            if (Desc->TryGetStringField(TEXT("Tooltip"), Tooltip))
            {
                FBlueprintEditorUtils::SetBlueprintVariableMetaData(Context.Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
            }

            bool bFlag = false;
            if (Desc->TryGetBoolField(TEXT("InstanceEditable"), bFlag))
            {
                // The engine stores the opposite of what the checkbox says.
                FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Context.Blueprint, VarName, !bFlag);
            }
            if (Desc->TryGetBoolField(TEXT("ReadOnly"), bFlag))
            {
                FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Context.Blueprint, VarName, bFlag);
            }
            if (Desc->TryGetBoolField(TEXT("Transient"), bFlag))
            {
                FBlueprintEditorUtils::SetVariableTransientFlag(Context.Blueprint, VarName, bFlag);
            }
            if (Desc->TryGetBoolField(TEXT("SaveGame"), bFlag))
            {
                FBlueprintEditorUtils::SetVariableSaveGameFlag(Context.Blueprint, VarName, bFlag);
            }
            if (Desc->TryGetBoolField(TEXT("AdvancedDisplay"), bFlag))
            {
                FBlueprintEditorUtils::SetVariableAdvancedDisplayFlag(Context.Blueprint, VarName, bFlag);
            }
            if (Desc->TryGetBoolField(TEXT("ExposeToCinematics"), bFlag))
            {
                FBlueprintEditorUtils::SetInterpFlag(Context.Blueprint, VarName, bFlag);
            }
            if (Desc->TryGetBoolField(TEXT("ExposeOnSpawn"), bFlag))
            {
                SetVariableMetaDataFlag(Context.Blueprint, VarName, FBlueprintMetadata::MD_ExposeOnSpawn, bFlag);
            }
            if (Desc->TryGetBoolField(TEXT("Private"), bFlag))
            {
                SetVariableMetaDataFlag(Context.Blueprint, VarName, FBlueprintMetadata::MD_Private, bFlag);
            }
            if (Desc->TryGetBoolField(TEXT("Config"), bFlag) && !ApplyPropertyFlag(Context, VarName, CPF_Config, bFlag))
            {
                return false;
            }

            return ApplyReplication(Context, VarName, Desc);
        }

        bool ApplyPropertyFlag(FBlueprintEditContext& Context, const FName VarName, uint64 Flag, bool bEnabled) const
        {
            uint64* PropertyFlags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Context.Blueprint, VarName);
            if (!PropertyFlags)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable '%s' carries no property flags"), *Context.AssetPath, *VarName.ToString());
                return false;
            }

            if (bEnabled)
            {
                *PropertyFlags |= Flag;
            }
            else
            {
                *PropertyFlags &= ~Flag;
            }

            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
            return true;
        }

        bool ApplyReplication(FBlueprintEditContext& Context, const FName VarName, const TSharedPtr<FJsonObject>& Desc) const
        {
            bool bReplicated = false;
            bool bRepNotify = false;
            FString RepNotifyFunc;
            const bool bHasReplicated = Desc->TryGetBoolField(TEXT("Replicated"), bReplicated);
            const bool bHasRepNotify = Desc->TryGetBoolField(TEXT("RepNotify"), bRepNotify);
            const bool bHasRepNotifyFunc = Desc->TryGetStringField(TEXT("RepNotifyFunc"), RepNotifyFunc);

            if (!bHasReplicated && !bHasRepNotify && !bHasRepNotifyFunc)
            {
                return true;
            }

            const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName);
            uint64* PropertyFlags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Context.Blueprint, VarName);
            if (VarIndex == INDEX_NONE || !PropertyFlags)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable '%s' carries no property flags"), *Context.AssetPath, *VarName.ToString());
                return false;
            }

            const bool bWantsReplicated = bHasReplicated ? bReplicated : (*PropertyFlags & CPF_Net) != 0;
            const bool bWantsRepNotify = bHasRepNotify ? bRepNotify : (*PropertyFlags & CPF_RepNotify) != 0;

            if (!bWantsReplicated)
            {
                *PropertyFlags &= ~CPF_Net;
                *PropertyFlags &= ~CPF_RepNotify;
                FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Context.Blueprint, VarName, NAME_None);
                Context.Blueprint->NewVariables[VarIndex].ReplicationCondition = COND_None;
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
                return true;
            }

            *PropertyFlags |= CPF_Net;

            if (!bWantsRepNotify)
            {
                *PropertyFlags &= ~CPF_RepNotify;
                FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Context.Blueprint, VarName, NAME_None);
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
                return true;
            }

            FName NotifyName = FName(*RepNotifyFunc);
            if (!bHasRepNotifyFunc)
            {
                const FName Existing = FBlueprintEditorUtils::GetBlueprintVariableRepNotifyFunc(Context.Blueprint, VarName);
                NotifyName = Existing.IsNone() ? FName(*FString::Printf(TEXT("OnRep_%s"), *VarName.ToString())) : Existing;
            }

            *PropertyFlags |= CPF_RepNotify;
            FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Context.Blueprint, VarName, NotifyName);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
            return true;
        }

        bool ApplyDefault(FBlueprintEditContext& Context, const FName VarName, const FString& Default, bool bRetyped) const
        {
            const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName);
            if (VarIndex == INDEX_NONE)
            {
                LogUnknownVariable(Context, VarName);
                return false;
            }

            Context.Blueprint->Modify();
            Context.Blueprint->NewVariables[VarIndex].DefaultValue = Default;

            // A retype leaves the compiled property on the old type, so only the compiler can carry the
            // value. Every other case writes the CDO too, which is what a reader sees before that compile.
            if (bRetyped)
            {
                return true;
            }

            UClass* GeneratedClass = Context.Blueprint->GeneratedClass;
            UObject* DefaultObject = GeneratedClass ? GeneratedClass->GetDefaultObject() : nullptr;
            FProperty* Property = DefaultObject ? FindFProperty<FProperty>(DefaultObject->GetClass(), VarName) : nullptr;
            if (!Property)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable '%s' has no compiled property to take a Default"), *Context.AssetPath, *VarName.ToString());
                return false;
            }

            DefaultObject->Modify();
            if (!FBlueprintEditorUtils::PropertyValueFromString(Property, Default, reinterpret_cast<uint8*>(DefaultObject), DefaultObject))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable '%s' rejected Default '%s'"), *Context.AssetPath, *VarName.ToString(), *Default);
                return false;
            }

            return true;
        }

        void LogUnknownVariable(const FBlueprintEditContext& Context, const FName VarName) const
        {
            if (IsComponentVariable(Context.Blueprint, VarName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is a component, not a variable, its defaults go through the Defaults section. Components: %s"), *Context.AssetPath, *VarName.ToString(), *DescribeComponents(Context.Blueprint));
                return;
            }

            UClass* ParentClass = Context.Blueprint->ParentClass;
            if (ParentClass && FindFProperty<FProperty>(ParentClass, VarName))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is inherited from %s and cannot be edited here. Own variables: %s"), *Context.AssetPath, *VarName.ToString(), *ParentClass->GetName(), *DescribeVariables(Context.Blueprint));
                return;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no variable named '%s'. Present: %s"), *Context.AssetPath, *VarName.ToString(), *DescribeVariables(Context.Blueprint));
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintVariableWriter()
{
    return MakeUnique<FBlueprintVariableWriter>();
}
