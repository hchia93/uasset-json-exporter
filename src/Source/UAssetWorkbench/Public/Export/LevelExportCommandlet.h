#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "LevelExportCommandlet.generated.h"

/*
 * Exports Level (.umap) contents to JSON.
 *
 * Strategy:
 *   For every Actor and Component, only non-default properties (delta against
 *   UObject::GetArchetype()) are serialized. This mirrors what the .umap itself
 *   persists, giving complete fidelity at minimum size.
 *
 *   Key collision / mesh / mobility fields are hoisted into top-level component
 *   keys for easy grep; remaining overrides live under DeltaProperties.
 *
 *   Instanced components (ISM / HISM / Foliage) with > InstanceDumpThreshold
 *   instances only record count + bounds + first few samples.
 *
 *   Instanced subobjects that are not components (WorldSettings' NavigationSystemConfig,
 *   etc) are nested under their property name with absolute values, not a delta.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=LevelExport -assets="/Game/Maps/L_A,/Game/Maps/L_B"
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<LevelPath>.json
 */
UCLASS()
class ULevelExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    ULevelExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    static constexpr int32 kInstanceDumpThreshold = 200;
    static constexpr int32 kInstanceSampleCount = 5;

    TSharedPtr<FJsonObject> ExportLevel(class UWorld* World, const FString& LevelPath) const;
    TSharedPtr<FJsonObject> ExportActor(class AActor* Actor) const;
    TSharedPtr<FJsonObject> ExportComponent(class UActorComponent* Component) const;
    TSharedPtr<FJsonObject> ExportWorldSettings(class AWorldSettings* WorldSettings) const;
    TSharedPtr<FJsonObject> ExportDeltaProperties(UObject* Object, UObject* Archetype) const;
    TSharedPtr<FJsonObject> ExportInstancedSubobject(UObject* SubObject) const;

    void AddInstancedComponentData(class UInstancedStaticMeshComponent* IsmComp, TSharedPtr<FJsonObject>& OutJson) const;
    void AddTransformField(const FTransform& Transform, const FString& FieldName, TSharedPtr<FJsonObject>& OutJson) const;

};
