#include "Import/WidgetLayoutImportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

namespace
{
    // Export writes these for readability. Slots is a live object list rebuilt by AddChild, SlotClass
    // is export-only metadata, feeding either back through ImportText corrupts the tree. Delegate
    // bindings export as an unparseable placeholder and carry nothing worth restoring.
    bool IsExportOnlyField(const FString& FieldName, const FString& Value)
    {
        if (FieldName == TEXT("Slots") || FieldName == TEXT("SlotClass"))
        {
            return true;
        }

        return FieldName.EndsWith(TEXT("Delegate")) || Value == TEXT("(null).None");
    }
}

UWidgetLayoutImportCommandlet::UWidgetLayoutImportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UWidgetLayoutImportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("UAssetWorkbench v%s - WidgetLayoutImport"), UASSET_WORKBENCH_VERSION_STRING);

    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("spec="), SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("No spec specified. Usage: -spec=\"C:/path/spec.json\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    SpecPath = SpecPath.TrimQuotes();

    FString SpecText;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to read spec: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    TSharedPtr<FJsonObject> Spec;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecText);
    if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Spec is not valid JSON: %s"), *SpecPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FString AssetPath;
    if (!Spec->TryGetStringField(TEXT("AssetPath"), AssetPath))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Spec has no AssetPath field"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    const TSharedPtr<FJsonObject>* RootSpec = nullptr;
    if (!Spec->TryGetObjectField(TEXT("WidgetTree"), RootSpec))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Spec has no WidgetTree field"));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to load WidgetBlueprint: %s"), *AssetPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    WidgetBP->Modify();
    WidgetBP->WidgetTree->Modify();

    // Old widgets keep the tree as their outer even once the root is dropped, so constructing a
    // same-named replacement collides with them. Move them out before rebuilding.
    TArray<UWidget*> RetiredWidgets;
    WidgetBP->WidgetTree->GetAllWidgets(RetiredWidgets);
    for (UWidget* Retired : RetiredWidgets)
    {
        if (Retired)
        {
            Retired->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
        }
    }

    // Whole-tree replace. The old root goes unreachable and drops out of the package on save.
    WidgetBP->WidgetTree->RootWidget = nullptr;

    UWidget* Root = BuildWidget(WidgetBP->WidgetTree, *RootSpec);
    if (!Root)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to build widget tree for %s"), *AssetPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    WidgetBP->WidgetTree->RootWidget = Root;

    SyncWidgetGuids(WidgetBP);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    FKismetEditorUtilities::CompileBlueprint(WidgetBP);

    if (WidgetBP->Status == BS_Error)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("%s compiled with errors, most likely a BindWidget name or type mismatch"), *AssetPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    // EditDefaultsOnly properties live on the CDO, not in the tree. Compile rebuilt it, so this has to
    // come after.
    const TSharedPtr<FJsonObject>* ClassDefaults = nullptr;
    if (Spec->TryGetObjectField(TEXT("ClassDefaults"), ClassDefaults))
    {
        UObject* DefaultWidget = WidgetBP->GeneratedClass ? WidgetBP->GeneratedClass->GetDefaultObject() : nullptr;
        if (!DefaultWidget)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("%s has no generated class, cannot apply ClassDefaults"), *AssetPath);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }

        DefaultWidget->Modify();

        int32 DefaultFailures = 0;
        UAssetWorkbench::ApplyProperties(DefaultWidget, *ClassDefaults, DefaultFailures);
        if (DefaultFailures > 0)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("%d class default(s) failed on %s"), DefaultFailures, *AssetPath);
            return ToExitCode(EUAssetWorkbenchExitType::Failed);
        }
    }

    // Already compiled above, saving only.
    if (!UAssetWorkbench::CompileAndSavePackage(WidgetBP, /* bCompileBlueprint */ false))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to save package for %s"), *AssetPath);
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    UE_LOG(LogUAssetWorkbenchImporter, Display, TEXT("Imported layout into %s"), *AssetPath);
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

UWidget* UWidgetLayoutImportCommandlet::BuildWidget(UWidgetTree* WidgetTree, const TSharedPtr<FJsonObject>& Spec) const
{
    FString ClassName;
    if (!Spec->TryGetStringField(TEXT("Class"), ClassName))
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Widget spec node has no Class field"));
        return nullptr;
    }

    UClass* WidgetClass = ResolveWidgetClass(ClassName);
    if (!WidgetClass)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Unresolved widget class: %s"), *ClassName);
        return nullptr;
    }

    FString WidgetName;
    Spec->TryGetStringField(TEXT("Name"), WidgetName);

    // Empty name would collide across siblings, let the tree generate one instead.
    const FName ConstructName = WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName);

    UWidget* Widget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, ConstructName);
    if (!Widget)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Error, TEXT("Failed to construct widget %s of class %s"), *WidgetName, *ClassName);
        return nullptr;
    }

    const TSharedPtr<FJsonObject>* Properties = nullptr;
    if (Spec->TryGetObjectField(TEXT("Properties"), Properties))
    {
        ApplyProperties(Widget, *Properties);
    }

    const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
    if (!Spec->TryGetArrayField(TEXT("Children"), Children))
    {
        return Widget;
    }

    UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
    if (!Panel)
    {
        UE_LOG(LogUAssetWorkbenchImporter, Warning, TEXT("%s has children but is not a panel, children dropped"), *WidgetName);
        return Widget;
    }

    for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
    {
        const TSharedPtr<FJsonObject>* ChildSpec = nullptr;
        if (!ChildValue->TryGetObject(ChildSpec))
        {
            continue;
        }

        UWidget* Child = BuildWidget(WidgetTree, *ChildSpec);
        if (!Child)
        {
            continue;
        }

        UPanelSlot* Slot = Panel->AddChild(Child);
        if (!Slot)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Warning, TEXT("%s rejected child %s"), *WidgetName, *Child->GetName());
            continue;
        }

        const TSharedPtr<FJsonObject>* SlotSpec = nullptr;
        if ((*ChildSpec)->TryGetObjectField(TEXT("Slot"), SlotSpec))
        {
            ApplyProperties(Slot, *SlotSpec);
        }
    }

    return Widget;
}

void UWidgetLayoutImportCommandlet::ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties) const
{
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        FString Value;
        if (!Pair.Value->TryGetString(Value))
        {
            continue;
        }

        if (IsExportOnlyField(Pair.Key, Value))
        {
            continue;
        }

        FProperty* Property = Target->GetClass()->FindPropertyByName(FName(*Pair.Key));
        if (!Property)
        {
            UE_LOG(LogUAssetWorkbenchImporter, Warning, TEXT("No property %s on %s"), *Pair.Key, *Target->GetClass()->GetName());
            continue;
        }

        void* Address = Property->ContainerPtrToValuePtr<void>(Target);
        if (!Property->ImportText_Direct(*Value, Address, Target, PPF_None))
        {
            UE_LOG(LogUAssetWorkbenchImporter, Warning, TEXT("Import failed for %s.%s = %s"), *Target->GetName(), *Pair.Key, *Value);
        }
    }
}

void UWidgetLayoutImportCommandlet::SyncWidgetGuids(UWidgetBlueprint* WidgetBP) const
{
    TSet<FName> LiveNames;
    WidgetBP->WidgetTree->ForEachWidget([&LiveNames](UWidget* Widget)
    {
        if (Widget)
        {
            LiveNames.Add(Widget->GetFName());
        }
    });

    // Animations share this map, dropping their entries would break references to them.
    for (const UWidgetAnimation* Animation : WidgetBP->Animations)
    {
        if (Animation)
        {
            LiveNames.Add(Animation->GetFName());
        }
    }

    for (auto It = WidgetBP->WidgetVariableNameToGuidMap.CreateIterator(); It; ++It)
    {
        if (!LiveNames.Contains(It.Key()))
        {
            It.RemoveCurrent();
        }
    }

    // Same name keeps its old GUID, so external references survive a tree rebuild.
    WidgetBP->WidgetTree->ForEachWidget([WidgetBP](UWidget* Widget)
    {
        if (Widget && !WidgetBP->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
        {
            WidgetBP->WidgetVariableNameToGuidMap.Add(Widget->GetFName(), FGuid::NewGuid());
        }
    });
}

UClass* UWidgetLayoutImportCommandlet::ResolveWidgetClass(const FString& ClassName) const
{
    if (ClassName.Contains(TEXT("/")))
    {
        return LoadObject<UClass>(nullptr, *ClassName);
    }

    UClass* UMGClass = UClass::TryFindTypeSlow<UClass>(FString::Printf(TEXT("/Script/UMG.%s"), *ClassName));
    if (UMGClass)
    {
        return UMGClass;
    }

    return UClass::TryFindTypeSlow<UClass>(ClassName);
}
