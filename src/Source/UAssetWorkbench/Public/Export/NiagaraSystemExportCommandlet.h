#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "NiagaraSystemExportCommandlet.generated.h"

class UNiagaraSystem;
class UNiagaraDataInterface;
class UNiagaraMeshRendererProperties;
class UNiagaraNodeFunctionCall;
struct FNiagaraParameterStore;
struct FNiagaraTypeDefinition;
struct FVersionedNiagaraEmitterData;

/*
 * Exports Niagara System structure (emitters, module input values, lifecycle, renderers, curves) to JSON.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe Project.uproject -run=NiagaraSystemExport -assets="/Game/Path/NS_A,/Game/Path/NS_B"
 *
 * Output:
 *   <ProjectDir>/Intermediate/UAssetExport/<AssetPath>.json
 */
UCLASS()
class UNiagaraSystemExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:

    UNiagaraSystemExportCommandlet();

    virtual int32 Main(const FString& Params) override;

private:

    TSharedPtr<FJsonObject> ExportNiagaraSystem(UNiagaraSystem* System) const;
    TSharedPtr<FJsonObject> ExportEmitter(const struct FNiagaraEmitterHandle& EmitterHandle) const;
    TSharedPtr<FJsonObject> ExportScript(class UNiagaraScript* Script, const FString& Usage) const;
    TSharedPtr<FJsonObject> ExportRendererProperties(class UNiagaraRendererProperties* Renderer) const;
    TSharedPtr<FJsonObject> ExportSubclassProperties(UObject* Object, UClass* StopAtClass) const;

    // Module input values: decode the script's RapidIterationParameters (Lifetime, Scale, SpawnCount, loop settings)
    TSharedPtr<FJsonObject> ExportParameterStoreValues(const FNiagaraParameterStore& Store) const;
    FString DecodeNiagaraValue(const FNiagaraTypeDefinition& Type, const uint8* Data) const;

    // Module stack + curve data interfaces from the emitter source graph
    TSharedPtr<FJsonObject> ExportEmitterGraph(FVersionedNiagaraEmitterData* EmitterData) const;
    TSharedPtr<FJsonObject> ExportCurveDataInterface(const FString& InputName, UNiagaraDataInterface* DataInterface) const;

    // Static switch selections on a module call (e.g. ShapeLocation "Shape Primitive"); these are pin
    // defaults on the call node, not RapidIterationParameters, so ExportParameterStoreValues misses them.
    TSharedPtr<FJsonObject> ExportNodeStaticSwitches(UNiagaraNodeFunctionCall* Node) const;

};
