#include "Export/NiagaraSystemExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/StaticMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "NiagaraDataInterfaceColorCurve.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceVector2DCurve.h"
#include "NiagaraDataInterfaceVectorCurve.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

UNiagaraSystemExportCommandlet::UNiagaraSystemExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UNiagaraSystemExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - NiagaraSystemExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/NS_A,/Game/Path/NS_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
        if (!System)
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load NiagaraSystem: %s"), *AssetPath);
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject = ExportNiagaraSystem(System);
        if (!JsonObject.IsValid())
        {
            UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to export NiagaraSystem: %s"), *AssetPath);
            continue;
        }

        FString OutputPath = UAssetWorkbench::GetExportPath(AssetPath);
        if (UAssetWorkbench::SaveJsonToFile(JsonObject.ToSharedRef(), OutputPath))
        {
            UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *OutputPath);
            ExportedCount++;
        }
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d Niagara systems exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportNiagaraSystem(UNiagaraSystem* System) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("NiagaraSystem"));
    Root->SetStringField(TEXT("SystemName"), System->GetName());
    Root->SetStringField(TEXT("AssetPath"), System->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // System-level exposed user parameters with their values
    TSharedPtr<FJsonObject> ExposedParams = ExportParameterStoreValues(System->GetExposedParameters());
    Root->SetObjectField(TEXT("ExposedParameters"), ExposedParams.ToSharedRef());

    // Emitters
    TArray<TSharedPtr<FJsonValue>> EmittersArray;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        TSharedPtr<FJsonObject> EmitterObj = ExportEmitter(EmitterHandle);
        if (EmitterObj.IsValid())
        {
            EmittersArray.Add(MakeShared<FJsonValueObject>(EmitterObj));
        }
    }
    Root->SetArrayField(TEXT("Emitters"), EmittersArray);

    return Root;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportEmitter(const FNiagaraEmitterHandle& EmitterHandle) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("Name"), EmitterHandle.GetName().ToString());
    Obj->SetBoolField(TEXT("Enabled"), EmitterHandle.GetIsEnabled());
    Obj->SetStringField(TEXT("UniqueInstanceName"), EmitterHandle.GetUniqueInstanceName());

    FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData();
    if (!EmitterData)
    {
        return Obj;
    }

    const TCHAR* SimTargetName = EmitterData->SimTarget == ENiagaraSimTarget::CPUSim ? TEXT("CPU") : TEXT("GPU");
    Obj->SetStringField(TEXT("SimTarget"), SimTargetName);
    Obj->SetBoolField(TEXT("LocalSpace"), EmitterData->bLocalSpace);

    // Emitter lifecycle stage. Loop duration / loop behavior live in this script's inputs
    if (EmitterData->EmitterUpdateScriptProps.Script)
    {
        Obj->SetObjectField(TEXT("EmitterUpdateScript"), ExportScript(EmitterData->EmitterUpdateScriptProps.Script, TEXT("EmitterUpdate")));
    }

    // Particle stages. Lifetime / initial Scale live in these inputs
    if (EmitterData->SpawnScriptProps.Script)
    {
        Obj->SetObjectField(TEXT("SpawnScript"), ExportScript(EmitterData->SpawnScriptProps.Script, TEXT("Spawn")));
    }
    if (EmitterData->UpdateScriptProps.Script)
    {
        Obj->SetObjectField(TEXT("UpdateScript"), ExportScript(EmitterData->UpdateScriptProps.Script, TEXT("Update")));
    }

    // Module stack + curves, one source graph per emitter
    TSharedPtr<FJsonObject> GraphObj = ExportEmitterGraph(EmitterData);
    if (GraphObj.IsValid() && GraphObj->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Graph"), GraphObj);
    }

    // Renderers
    TArray<TSharedPtr<FJsonValue>> RenderersArray;
    for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
    {
        if (Renderer)
        {
            TSharedPtr<FJsonObject> RendererObj = ExportRendererProperties(Renderer);
            if (RendererObj.IsValid())
            {
                RenderersArray.Add(MakeShared<FJsonValueObject>(RendererObj));
            }
        }
    }
    Obj->SetArrayField(TEXT("Renderers"), RenderersArray);

    return Obj;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportScript(UNiagaraScript* Script, const FString& Usage) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("Usage"), Usage);
    Obj->SetStringField(TEXT("ScriptName"), Script->GetName());

    // Decoded module input constants the artist typed (Lifetime, Scale, SpawnCount, loop settings...)
    TSharedPtr<FJsonObject> Inputs = ExportParameterStoreValues(Script->RapidIterationParameters);
    if (Inputs.IsValid() && Inputs->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Inputs"), Inputs);
    }

    return Obj;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportParameterStoreValues(const FNiagaraParameterStore& Store) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    const TArray<uint8>& DataArray = Store.GetParameterDataArray();

    for (const FNiagaraVariableWithOffset& Var : Store.ReadParameterVariables())
    {
        // POD scalars only. Data interfaces / UObjects index a separate store and carry no scalar value
        if (Var.GetType().IsDataInterface() || Var.GetType().IsUObject())
        {
            continue;
        }
        if (Var.Offset < 0 || Var.Offset >= DataArray.Num())
        {
            continue;
        }

        const FString Value = DecodeNiagaraValue(Var.GetType(), DataArray.GetData() + Var.Offset);
        if (!Value.IsEmpty())
        {
            Obj->SetStringField(Var.GetName().ToString(), Value);
        }
    }

    return Obj;
}

FString UNiagaraSystemExportCommandlet::DecodeNiagaraValue(const FNiagaraTypeDefinition& Type, const uint8* Data) const
{
    if (!Data)
    {
        return FString();
    }

    auto ReadFloat = [Data](int32 Index)
    {
        float Value;
        FMemory::Memcpy(&Value, Data + Index * sizeof(float), sizeof(float));
        return Value;
    };

    if (Type == FNiagaraTypeDefinition::GetFloatDef())
    {
        return FString::SanitizeFloat(ReadFloat(0));
    }
    if (Type == FNiagaraTypeDefinition::GetIntDef())
    {
        int32 Value;
        FMemory::Memcpy(&Value, Data, sizeof(int32));
        return FString::FromInt(Value);
    }
    if (Type == FNiagaraTypeDefinition::GetBoolDef())
    {
        int32 Value;
        FMemory::Memcpy(&Value, Data, sizeof(int32));
        return Value != 0 ? TEXT("true") : TEXT("false");
    }
    // Enums (Loop Behavior, Life Cycle Mode...) are stored as int32
    if (const UEnum* Enum = Type.GetEnum())
    {
        int32 Value;
        FMemory::Memcpy(&Value, Data, sizeof(int32));
        return Enum->GetNameStringByValue(Value);
    }
    if (Type == FNiagaraTypeDefinition::GetVec2Def())
    {
        return FString::Printf(TEXT("(X=%f,Y=%f)"), ReadFloat(0), ReadFloat(1));
    }
    if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
    {
        return FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), ReadFloat(0), ReadFloat(1), ReadFloat(2));
    }
    if (Type == FNiagaraTypeDefinition::GetVec4Def() || Type == FNiagaraTypeDefinition::GetColorDef() || Type == FNiagaraTypeDefinition::GetQuatDef())
    {
        return FString::Printf(TEXT("(X=%f,Y=%f,Z=%f,W=%f)"), ReadFloat(0), ReadFloat(1), ReadFloat(2), ReadFloat(3));
    }

    // Fallback: treat unknown POD struct as a raw float array, tagged with the type name
    const int32 Size = Type.GetSize();
    if (Size >= 4 && (Size % 4) == 0)
    {
        TArray<FString> Floats;
        for (int32 Index = 0; Index < Size / 4; ++Index)
        {
            Floats.Add(FString::SanitizeFloat(ReadFloat(Index)));
        }
        return FString::Printf(TEXT("[%s] %s"), *Type.GetName(), *FString::Join(Floats, TEXT(",")));
    }

    return FString::Printf(TEXT("[%s] <%d bytes>"), *Type.GetName(), Size);
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportEmitterGraph(FVersionedNiagaraEmitterData* EmitterData) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    UNiagaraScript* SourceScript = EmitterData->SpawnScriptProps.Script;
    if (!SourceScript)
    {
        return Obj;
    }

    UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceScript->GetLatestSource());
    if (!Source || !Source->NodeGraph)
    {
        return Obj;
    }

    UNiagaraGraph* Graph = Source->NodeGraph;

    // Module / dynamic-input function calls present in the graph
    TArray<UNiagaraNodeFunctionCall*> FunctionNodes;
    Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(FunctionNodes);

    TArray<TSharedPtr<FJsonValue>> ModulesArray;
    for (UNiagaraNodeFunctionCall* Node : FunctionNodes)
    {
        if (!Node)
        {
            continue;
        }
        TSharedPtr<FJsonObject> ModuleObj = MakeShared<FJsonObject>();
        ModuleObj->SetStringField(TEXT("Name"), Node->FunctionScript ? Node->FunctionScript->GetName() : Node->GetName());

        TSharedPtr<FJsonObject> Switches = ExportNodeStaticSwitches(Node);
        if (Switches.IsValid() && Switches->Values.Num() > 0)
        {
            ModuleObj->SetObjectField(TEXT("StaticSwitches"), Switches);
        }

        ModulesArray.Add(MakeShared<FJsonValueObject>(ModuleObj));
    }
    if (ModulesArray.Num() > 0)
    {
        Obj->SetArrayField(TEXT("Modules"), ModulesArray);
    }

    // Curve data interfaces drive fade / shrink over life
    TArray<UNiagaraNodeInput*> InputNodes;
    Graph->GetNodesOfClass<UNiagaraNodeInput>(InputNodes);

    // GetDataInterface() is not exported from NiagaraEditor, read the UPROPERTY via reflection
    FObjectPropertyBase* DataInterfaceProp = CastField<FObjectPropertyBase>(UNiagaraNodeInput::StaticClass()->FindPropertyByName(TEXT("DataInterface")));

    TArray<TSharedPtr<FJsonValue>> CurvesArray;
    for (UNiagaraNodeInput* InputNode : InputNodes)
    {
        if (!InputNode || !DataInterfaceProp)
        {
            continue;
        }
        UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(DataInterfaceProp->GetObjectPropertyValue_InContainer(InputNode));
        if (!DataInterface)
        {
            continue;
        }
        TSharedPtr<FJsonObject> CurveObj = ExportCurveDataInterface(InputNode->Input.GetName().ToString(), DataInterface);
        if (CurveObj.IsValid())
        {
            CurvesArray.Add(MakeShared<FJsonValueObject>(CurveObj));
        }
    }
    if (CurvesArray.Num() > 0)
    {
        Obj->SetArrayField(TEXT("Curves"), CurvesArray);
    }

    return Obj;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportNodeStaticSwitches(UNiagaraNodeFunctionCall* Node) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    UNiagaraGraph* CalledGraph = Node ? Node->GetCalledGraph() : nullptr;
    if (!CalledGraph)
    {
        return Obj;
    }

    // Static switch value lives as the call node's input pin default, keyed by the switch variable name.
    for (const FNiagaraVariable& SwitchVar : CalledGraph->FindStaticSwitchInputs())
    {
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input || Pin->GetFName() != SwitchVar.GetName())
            {
                continue;
            }

            FString Value = Pin->DefaultValue;
            // Enum switches (Shape Primitive, etc.) resolve to the artist-facing display name. User-defined
            // enums store the enumerator's internal name (e.g. "NewEnumerator2"), native enums an integer.
            if (const UEnum* Enum = SwitchVar.GetType().GetEnum())
            {
                int32 Index = INDEX_NONE;
                if (Value.IsNumeric())
                {
                    Index = Enum->GetIndexByValue(FCString::Atoi64(*Value));
                }
                else
                {
                    for (int32 EnumIndex = 0; EnumIndex < Enum->NumEnums(); ++EnumIndex)
                    {
                        if (Enum->GetNameStringByIndex(EnumIndex) == Value)
                        {
                            Index = EnumIndex;
                            break;
                        }
                    }
                }
                if (Index != INDEX_NONE)
                {
                    Value = Enum->GetDisplayNameTextByIndex(Index).ToString();
                }
            }
            if (!Value.IsEmpty())
            {
                Obj->SetStringField(SwitchVar.GetName().ToString(), Value);
            }
            break;
        }
    }

    return Obj;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportCurveDataInterface(const FString& InputName, UNiagaraDataInterface* DataInterface) const
{
    auto KeysToJson = [](const FRichCurve& Curve)
    {
        TArray<TSharedPtr<FJsonValue>> Keys;
        for (const FRichCurveKey& Key : Curve.GetConstRefOfKeys())
        {
            TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
            KeyObj->SetNumberField(TEXT("Time"), Key.Time);
            KeyObj->SetNumberField(TEXT("Value"), Key.Value);
            Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
        }
        return Keys;
    };

    if (UNiagaraDataInterfaceCurve* FloatCurve = Cast<UNiagaraDataInterfaceCurve>(DataInterface))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("Input"), InputName);
        Obj->SetStringField(TEXT("CurveType"), TEXT("Float"));
        Obj->SetArrayField(TEXT("Keys"), KeysToJson(FloatCurve->Curve));
        return Obj;
    }
    if (UNiagaraDataInterfaceVector2DCurve* Vec2Curve = Cast<UNiagaraDataInterfaceVector2DCurve>(DataInterface))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("Input"), InputName);
        Obj->SetStringField(TEXT("CurveType"), TEXT("Vector2D"));
        Obj->SetArrayField(TEXT("X"), KeysToJson(Vec2Curve->XCurve));
        Obj->SetArrayField(TEXT("Y"), KeysToJson(Vec2Curve->YCurve));
        return Obj;
    }
    if (UNiagaraDataInterfaceVectorCurve* VecCurve = Cast<UNiagaraDataInterfaceVectorCurve>(DataInterface))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("Input"), InputName);
        Obj->SetStringField(TEXT("CurveType"), TEXT("Vector"));
        Obj->SetArrayField(TEXT("X"), KeysToJson(VecCurve->XCurve));
        Obj->SetArrayField(TEXT("Y"), KeysToJson(VecCurve->YCurve));
        Obj->SetArrayField(TEXT("Z"), KeysToJson(VecCurve->ZCurve));
        return Obj;
    }
    if (UNiagaraDataInterfaceColorCurve* ColorCurve = Cast<UNiagaraDataInterfaceColorCurve>(DataInterface))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("Input"), InputName);
        Obj->SetStringField(TEXT("CurveType"), TEXT("Color"));
        Obj->SetArrayField(TEXT("R"), KeysToJson(ColorCurve->RedCurve));
        Obj->SetArrayField(TEXT("G"), KeysToJson(ColorCurve->GreenCurve));
        Obj->SetArrayField(TEXT("B"), KeysToJson(ColorCurve->BlueCurve));
        Obj->SetArrayField(TEXT("A"), KeysToJson(ColorCurve->AlphaCurve));
        return Obj;
    }

    return nullptr;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportRendererProperties(UNiagaraRendererProperties* Renderer) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("RendererClass"), Renderer->GetClass()->GetName());

    TSharedPtr<FJsonObject> Props = ExportSubclassProperties(Renderer, UNiagaraRendererProperties::StaticClass());
    if (Props.IsValid() && Props->Values.Num() > 0)
    {
        Obj->SetObjectField(TEXT("Properties"), Props);
    }

    // Resolve mesh references to native size so "mesh too big" is measurable
    if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
    {
        TArray<TSharedPtr<FJsonValue>> MeshArray;
        for (const FNiagaraMeshRendererMeshProperties& MeshProps : MeshRenderer->Meshes)
        {
            TSharedPtr<FJsonObject> MeshObj = MakeShared<FJsonObject>();
            if (MeshProps.Mesh)
            {
                const FBoxSphereBounds Bounds = MeshProps.Mesh->GetBounds();
                MeshObj->SetStringField(TEXT("Mesh"), MeshProps.Mesh->GetPathName());
                MeshObj->SetStringField(TEXT("NativeBoundsExtent"), Bounds.BoxExtent.ToString());
                MeshObj->SetNumberField(TEXT("NativeBoundsRadius"), Bounds.SphereRadius);
            }
            MeshObj->SetStringField(TEXT("Scale"), MeshProps.Scale.ToString());
            MeshObj->SetStringField(TEXT("PivotOffset"), MeshProps.PivotOffset.ToString());
            MeshArray.Add(MakeShared<FJsonValueObject>(MeshObj));
        }
        if (MeshArray.Num() > 0)
        {
            Obj->SetArrayField(TEXT("MeshDetails"), MeshArray);
        }
    }

    return Obj;
}

TSharedPtr<FJsonObject> UNiagaraSystemExportCommandlet::ExportSubclassProperties(UObject* Object, UClass* StopAtClass) const
{
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

    UObject* DefaultObject = Object->GetClass()->GetDefaultObject();

    UClass* CurrentClass = Object->GetClass();
    while (CurrentClass && CurrentClass != StopAtClass)
    {
        for (TFieldIterator<FProperty> PropIt(CurrentClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }
            // Drop values equal to the class default, kills the default binding boilerplate noise
            if (DefaultObject && Prop->Identical_InContainer(Object, DefaultObject))
            {
                continue;
            }

            FString Value;
            Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
            if (!Value.IsEmpty())
            {
                Props->SetStringField(Prop->GetName(), Value);
            }
        }
        CurrentClass = CurrentClass->GetSuperClass();
    }

    return Props;
}
