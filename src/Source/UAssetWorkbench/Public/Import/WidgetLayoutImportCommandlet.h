#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "WidgetLayoutImportCommandlet.generated.h"

class FJsonObject;
class UWidget;
class UWidgetTree;

// Builds a Widget Blueprint's WidgetTree from a JSON layout spec, the inverse of WidgetLayoutExport.
// Replaces the whole tree, so the spec has to describe all of it. Fields it does not read are ignored,
// which is what lets an exported layout round-trip unedited.
//   UnrealEditor-Cmd.exe Project.uproject -run=WidgetLayoutImport -spec="C:/path/spec.json"
// Contract: Docs/Import.md
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
