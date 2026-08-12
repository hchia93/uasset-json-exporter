#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "BehaviorTreeExportCommandlet.generated.h"

class UBTNode;
class UBTCompositeNode;
struct FBTCompositeChild;

/*
 * Exports BehaviorTree structure (composites, tasks, decorators, services) to JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=BehaviorTreeExport -assets="/Game/Path/BT_A,/Game/Path/BT_B"
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json
 */
UCLASS()
class UBehaviorTreeExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UBehaviorTreeExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportBehaviorTree(class UBehaviorTree* BT) const;
    TSharedPtr<FJsonObject> ExportCompositeNode(UBTCompositeNode* Node) const;
    TSharedPtr<FJsonObject> ExportChildEntry(const FBTCompositeChild& Child) const;
    TSharedPtr<FJsonObject> ExportNode(UBTNode* Node) const;
    TSharedPtr<FJsonObject> ExportSubclassProperties(UObject* Object, UClass* StopAtClass) const;

};
