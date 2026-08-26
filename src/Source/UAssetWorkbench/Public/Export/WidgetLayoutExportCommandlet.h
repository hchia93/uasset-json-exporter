#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "WidgetLayoutExportCommandlet.generated.h"

class UWidget;
class UPanelSlot;
class UWidgetAnimation;

// Exports Widget Blueprint structure to JSON: EdGraph, widget tree, layout properties, animations.
//   UnrealEditor-Cmd.exe Project.uproject -run=WidgetLayoutExport -assets="/Game/Path/WBP_A,/Game/Path/WBP_B"
// Contract: Docs/Export.md
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

};
