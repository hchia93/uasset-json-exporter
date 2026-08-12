#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "WidgetLayoutExportCommandlet.generated.h"

class UWidget;
class UPanelSlot;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UWidgetAnimation;

/*
 * Exports Widget Blueprint structure (EdGraph, WidgetTree, layout properties, animations) to JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=WidgetLayoutExport -assets="/Game/Path/WBP_A,/Game/Path/WBP_B"
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json
 */
UCLASS()
class UWidgetLayoutExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UWidgetLayoutExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportWidgetBlueprint(class UWidgetBlueprint* WidgetBP) const;
    TSharedPtr<FJsonObject> ExportWidget(UWidget* Widget) const;
    TSharedPtr<FJsonObject> ExportSlotProperties(UPanelSlot* Slot) const;
    TSharedPtr<FJsonObject> ExportAnimation(UWidgetAnimation* Animation) const;

    // EdGraph export (shared with BlueprintEdGraphExportCommandlet)
    TSharedPtr<FJsonObject> ExportGraph(const UEdGraph* Graph) const;
    TSharedPtr<FJsonObject> ExportNode(const UEdGraphNode* Node) const;
    TSharedPtr<FJsonObject> ExportPin(const UEdGraphPin* Pin) const;

};
