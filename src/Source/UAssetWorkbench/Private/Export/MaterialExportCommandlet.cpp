#include "Export/MaterialExportCommandlet.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "UObject/UnrealType.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    // Custom-node output type is a plain enum (not UENUM), so map it by hand.
    FString CustomOutputTypeToString(ECustomMaterialOutputType Type)
    {
        switch (Type)
        {
        case CMOT_Float1:             return TEXT("Float1");
        case CMOT_Float2:             return TEXT("Float2");
        case CMOT_Float3:             return TEXT("Float3");
        case CMOT_Float4:             return TEXT("Float4");
        case CMOT_MaterialAttributes: return TEXT("MaterialAttributes");
        default:                      return TEXT("Unknown");
        }
    }
}

UMaterialExportCommandlet::UMaterialExportCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UMaterialExportCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("UAssetWorkbench v%s - MaterialExport"), UASSET_WORKBENCH_VERSION_STRING);

    TArray<FString> AssetPaths = UAssetWorkbench::ParseAssetPaths(Params);

    if (AssetPaths.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchExporter, Error, TEXT("No assets specified. Usage: -assets=\"/Game/Path/M_A,/Game/Path/MI_B\""));
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    int32 ExportedCount = 0;

    for (const FString& AssetPath : AssetPaths)
    {
        // Try MaterialInstance first, then Material
        UMaterialInstance* MaterialInstance = LoadObject<UMaterialInstance>(nullptr, *AssetPath);
        if (MaterialInstance)
        {
            TSharedPtr<FJsonObject> JsonObject = ExportMaterialInstance(MaterialInstance);
            if (JsonObject.IsValid())
            {
                UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
                if (ExportTarget.Save(JsonObject.ToSharedRef()))
                {
                    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
                    ExportedCount++;
                }
            }
            continue;
        }

        UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
        if (Material)
        {
            TSharedPtr<FJsonObject> JsonObject = ExportMaterial(Material);
            if (JsonObject.IsValid())
            {
                UAssetWorkbench::FExportTarget ExportTarget(AssetPath);
                if (ExportTarget.Save(JsonObject.ToSharedRef()))
                {
                    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Exported: %s -> %s"), *AssetPath, *ExportTarget.GetPath());
                    ExportedCount++;
                }
            }
            continue;
        }

        UE_LOG(LogUAssetWorkbenchExporter, Warning, TEXT("Failed to load Material or MaterialInstance: %s"), *AssetPath);
    }

    UE_LOG(LogUAssetWorkbenchExporter, Display, TEXT("Export complete. %d/%d materials exported."), ExportedCount, AssetPaths.Num());
    return ToExitCode(EUAssetWorkbenchExitType::Success);
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportMaterial(UMaterial* Material) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("Material"));
    Root->SetStringField(TEXT("MaterialName"), Material->GetName());
    Root->SetStringField(TEXT("AssetPath"), Material->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Global settings
    const EMaterialShadingModel ShadingModel = Material->GetShadingModels().GetFirstShadingModel();
    Root->SetStringField(TEXT("ShadingModel"), StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(static_cast<int64>(ShadingModel)));
    Root->SetStringField(TEXT("BlendMode"), StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(Material->BlendMode)));
    Root->SetBoolField(TEXT("TwoSided"), Material->IsTwoSided());
    Root->SetStringField(TEXT("MaterialDomain"), StaticEnum<EMaterialDomain>()->GetNameStringByValue(static_cast<int64>(Material->MaterialDomain)));
    Root->SetNumberField(TEXT("OpacityMaskClipValue"), Material->OpacityMaskClipValue);
    Root->SetBoolField(TEXT("bAutomaticallySetUsageInEditor"), Material->bAutomaticallySetUsageInEditor != 0);

    // Usage flags decide which permutations compile, and EditMaterialAsset writes them, so an export has
    // to show them. UMaterial::GetUsageName is not exported, hence the reflected property names.
    TArray<TSharedPtr<FJsonValue>> UsageFlagsArray;
    for (TFieldIterator<FBoolProperty> It(UMaterial::StaticClass()); It; ++It)
    {
        if (!It->GetName().StartsWith(TEXT("bUsedWith")) || It->HasAnyPropertyFlags(CPF_Deprecated))
        {
            continue;
        }

        if (It->GetPropertyValue_InContainer(Material))
        {
            UsageFlagsArray.Add(MakeShared<FJsonValueString>(It->GetName()));
        }
    }
    Root->SetArrayField(TEXT("UsageFlags"), UsageFlagsArray);

    // Expressions (node graph)
    TArray<TSharedPtr<FJsonValue>> ExpressionsArray;
    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (Expression)
        {
            TSharedPtr<FJsonObject> ExprObj = ExportExpression(Expression);
            if (ExprObj.IsValid())
            {
                ExpressionsArray.Add(MakeShared<FJsonValueObject>(ExprObj));
            }
        }
    }
    Root->SetArrayField(TEXT("Expressions"), ExpressionsArray);

    // Expression connections (input pins on the material output node)
    TSharedPtr<FJsonObject> OutputConnections = MakeShared<FJsonObject>();

    auto ExportMaterialInput = [&](const FExpressionInput& Input, const FString& PinName)
    {
        if (Input.Expression)
        {
            TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
            ConnObj->SetStringField(TEXT("Expression"), Input.Expression->GetName());
            ConnObj->SetStringField(TEXT("ExpressionClass"), Input.Expression->GetClass()->GetName());
            ConnObj->SetNumberField(TEXT("OutputIndex"), Input.OutputIndex);
            OutputConnections->SetObjectField(PinName, ConnObj);
        }
    };

    // UE 5 moved per-shading-model material inputs onto an editor-only data subobject,
    // GetEditorOnlyData() is the only access path, the direct UMaterial members are gone.
    ExportMaterialInput(Material->GetEditorOnlyData()->BaseColor, TEXT("BaseColor"));
    ExportMaterialInput(Material->GetEditorOnlyData()->Metallic, TEXT("Metallic"));
    ExportMaterialInput(Material->GetEditorOnlyData()->Specular, TEXT("Specular"));
    ExportMaterialInput(Material->GetEditorOnlyData()->Roughness, TEXT("Roughness"));
    ExportMaterialInput(Material->GetEditorOnlyData()->Normal, TEXT("Normal"));
    ExportMaterialInput(Material->GetEditorOnlyData()->EmissiveColor, TEXT("EmissiveColor"));
    ExportMaterialInput(Material->GetEditorOnlyData()->Opacity, TEXT("Opacity"));
    ExportMaterialInput(Material->GetEditorOnlyData()->OpacityMask, TEXT("OpacityMask"));
    ExportMaterialInput(Material->GetEditorOnlyData()->WorldPositionOffset, TEXT("WorldPositionOffset"));
    ExportMaterialInput(Material->GetEditorOnlyData()->AmbientOcclusion, TEXT("AmbientOcclusion"));

    if (OutputConnections->Values.Num() > 0)
    {
        Root->SetObjectField(TEXT("OutputConnections"), OutputConnections);
    }

    return Root;
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportMaterialInstance(UMaterialInstance* MaterialInstance) const
{
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    Root->SetStringField(TEXT("ExporterVersion"), UASSET_WORKBENCH_VERSION_STRING);
    Root->SetStringField(TEXT("ExportType"), TEXT("MaterialInstance"));
    Root->SetStringField(TEXT("MaterialInstanceName"), MaterialInstance->GetName());
    Root->SetStringField(TEXT("AssetPath"), MaterialInstance->GetPathName());
    Root->SetStringField(TEXT("ExportTimestamp"), FDateTime::Now().ToString());

    // Parent material
    if (MaterialInstance->Parent)
    {
        Root->SetStringField(TEXT("Parent"), MaterialInstance->Parent->GetName());
        Root->SetStringField(TEXT("ParentPath"), MaterialInstance->Parent->GetPathName());
    }

    // Scalar parameters
    TArray<TSharedPtr<FJsonValue>> ScalarsArray;
    for (const FScalarParameterValue& Param : MaterialInstance->ScalarParameterValues)
    {
        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("Name"), Param.ParameterInfo.Name.ToString());
        ParamObj->SetNumberField(TEXT("Value"), Param.ParameterValue);
        ScalarsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    }
    Root->SetArrayField(TEXT("ScalarParameters"), ScalarsArray);

    // Vector parameters
    TArray<TSharedPtr<FJsonValue>> VectorsArray;
    for (const FVectorParameterValue& Param : MaterialInstance->VectorParameterValues)
    {
        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("Name"), Param.ParameterInfo.Name.ToString());
        ParamObj->SetStringField(TEXT("Value"), Param.ParameterValue.ToString());
        VectorsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    }
    Root->SetArrayField(TEXT("VectorParameters"), VectorsArray);

    // Texture parameters
    TArray<TSharedPtr<FJsonValue>> TexturesArray;
    for (const FTextureParameterValue& Param : MaterialInstance->TextureParameterValues)
    {
        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("Name"), Param.ParameterInfo.Name.ToString());
        if (Param.ParameterValue)
        {
            ParamObj->SetStringField(TEXT("Texture"), Param.ParameterValue->GetName());
            ParamObj->SetStringField(TEXT("TexturePath"), Param.ParameterValue->GetPathName());
        }
        else
        {
            ParamObj->SetStringField(TEXT("Texture"), TEXT("None"));
        }
        TexturesArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    }
    Root->SetArrayField(TEXT("TextureParameters"), TexturesArray);

    // Static Switch parameters
    TArray<TSharedPtr<FJsonValue>> StaticSwitchArray;
    for (const FStaticSwitchParameter& Param : MaterialInstance->GetStaticParameters().StaticSwitchParameters)
    {
        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("Name"), Param.ParameterInfo.Name.ToString());
        ParamObj->SetBoolField(TEXT("Value"), Param.Value);
        ParamObj->SetBoolField(TEXT("Override"), Param.bOverride);
        StaticSwitchArray.Add(MakeShared<FJsonValueObject>(ParamObj));
    }
    Root->SetArrayField(TEXT("StaticSwitchParameters"), StaticSwitchArray);

    // Only the overrides actually flagged, an instance that overrides nothing writes an empty object.
    const FMaterialInstanceBasePropertyOverrides& Overrides = MaterialInstance->BasePropertyOverrides;
    TSharedPtr<FJsonObject> OverridesObj = MakeShared<FJsonObject>();
    if (Overrides.bOverride_BlendMode)
    {
        OverridesObj->SetStringField(TEXT("BlendMode"), StaticEnum<EBlendMode>()->GetNameStringByValue(Overrides.BlendMode.GetValue()));
    }
    if (Overrides.bOverride_ShadingModel)
    {
        OverridesObj->SetStringField(TEXT("ShadingModel"), StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(Overrides.ShadingModel.GetValue()));
    }
    if (Overrides.bOverride_TwoSided)
    {
        OverridesObj->SetBoolField(TEXT("TwoSided"), Overrides.TwoSided != 0);
    }
    if (Overrides.bOverride_OpacityMaskClipValue)
    {
        OverridesObj->SetNumberField(TEXT("OpacityMaskClipValue"), Overrides.OpacityMaskClipValue);
    }
    Root->SetObjectField(TEXT("BasePropertyOverrides"), OverridesObj);

    return Root;
}

TSharedPtr<FJsonObject> UMaterialExportCommandlet::ExportExpression(UMaterialExpression* Expression) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

    Obj->SetStringField(TEXT("Name"), Expression->GetName());
    Obj->SetStringField(TEXT("Class"), Expression->GetClass()->GetName());
    Obj->SetStringField(TEXT("Description"), Expression->GetDescription());

    // Parameter-specific data
    if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expression))
    {
        Obj->SetStringField(TEXT("ParameterName"), ScalarParam->ParameterName.ToString());
        Obj->SetNumberField(TEXT("DefaultValue"), ScalarParam->DefaultValue);
        Obj->SetStringField(TEXT("Group"), ScalarParam->Group.ToString());
    }
    else if (UMaterialExpressionVectorParameter* VectorParam = Cast<UMaterialExpressionVectorParameter>(Expression))
    {
        Obj->SetStringField(TEXT("ParameterName"), VectorParam->ParameterName.ToString());
        Obj->SetStringField(TEXT("DefaultValue"), VectorParam->DefaultValue.ToString());
        Obj->SetStringField(TEXT("Group"), VectorParam->Group.ToString());
    }
    else if (UMaterialExpressionTextureSampleParameter* TextureParam = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
    {
        Obj->SetStringField(TEXT("ParameterName"), TextureParam->ParameterName.ToString());
        if (TextureParam->Texture)
        {
            Obj->SetStringField(TEXT("DefaultTexture"), TextureParam->Texture->GetPathName());
        }
        Obj->SetStringField(TEXT("Group"), TextureParam->Group.ToString());
    }
    else if (UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(Expression))
    {
        // The HLSL body, the whole reason this branch exists. Node-graph export alone hides it.
        Obj->SetStringField(TEXT("Code"), CustomExpr->Code);
        Obj->SetStringField(TEXT("OutputType"), CustomOutputTypeToString(CustomExpr->OutputType));

        if (CustomExpr->AdditionalDefines.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> DefinesArray;
            for (const FCustomDefine& Define : CustomExpr->AdditionalDefines)
            {
                TSharedPtr<FJsonObject> DefineObj = MakeShared<FJsonObject>();
                DefineObj->SetStringField(TEXT("Name"), Define.DefineName);
                DefineObj->SetStringField(TEXT("Value"), Define.DefineValue);
                DefinesArray.Add(MakeShared<FJsonValueObject>(DefineObj));
            }
            Obj->SetArrayField(TEXT("AdditionalDefines"), DefinesArray);
        }

        if (CustomExpr->IncludeFilePaths.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> IncludesArray;
            for (const FString& IncludePath : CustomExpr->IncludeFilePaths)
            {
                IncludesArray.Add(MakeShared<FJsonValueString>(IncludePath));
            }
            Obj->SetArrayField(TEXT("IncludeFilePaths"), IncludesArray);
        }

        if (CustomExpr->AdditionalOutputs.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> OutputsArray;
            for (const FCustomOutput& Output : CustomExpr->AdditionalOutputs)
            {
                TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
                OutputObj->SetStringField(TEXT("Name"), Output.OutputName.ToString());
                OutputObj->SetStringField(TEXT("OutputType"), CustomOutputTypeToString(Output.OutputType));
                OutputsArray.Add(MakeShared<FJsonValueObject>(OutputObj));
            }
            Obj->SetArrayField(TEXT("AdditionalOutputs"), OutputsArray);
        }
    }

    // Input connections
    {
        TArray<TSharedPtr<FJsonValue>> InputsArray;
        for (FExpressionInputIterator It(Expression); It; ++It)
        {
            FExpressionInput* Input = It.Input;
            if (Input && Input->Expression)
            {
                TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
                InputObj->SetStringField(TEXT("InputName"), Expression->GetInputName(It.Index).ToString());
                InputObj->SetStringField(TEXT("ConnectedTo"), Input->Expression->GetName());
                InputObj->SetStringField(TEXT("ConnectedClass"), Input->Expression->GetClass()->GetName());
                InputObj->SetNumberField(TEXT("OutputIndex"), Input->OutputIndex);
                InputsArray.Add(MakeShared<FJsonValueObject>(InputObj));
            }
        }
        if (InputsArray.Num() > 0)
        {
            Obj->SetArrayField(TEXT("Inputs"), InputsArray);
        }
    }

    return Obj;
}

