#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "WidgetLayoutImportCommandlet.generated.h"

class FJsonObject;
class UWidget;
class UWidgetTree;

/*
 * Builds a Widget Blueprint's WidgetTree from a JSON layout spec, the inverse of WidgetLayoutExport.
 * Reads Class / Name / Properties / Slot / Children and ignores every other field, so an exported
 * layout JSON round-trips and a hand-written minimal spec works through the same path.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=WidgetLayoutImport -spec="C:/path/spec.json"
 *
 * Spec:
 *   { "AssetPath": "/Game/Path/WBP_A", "WidgetTree": { "Class": "HorizontalBox", "Name": "Root", ... } }
 *
 * Replaces the whole tree. Class is a bare UMG name (HorizontalBox) or a full object path for a
 * Blueprint class (/Game/Path/WBP_X.WBP_X_C).
 */
UCLASS()
class UWidgetLayoutImportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UWidgetLayoutImportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    UWidget* BuildWidget(UWidgetTree* WidgetTree, const TSharedPtr<FJsonObject>& Spec) const;
    void ApplyProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties) const;
    UClass* ResolveWidgetClass(const FString& ClassName) const;

    // The blueprint keeps widget and animation GUIDs in a map beside the tree. Replacing the tree
    // without reconciling it trips the compiler on both sides, stale entries and missing ones.
    void SyncWidgetGuids(class UWidgetBlueprint* WidgetBP) const;
};
