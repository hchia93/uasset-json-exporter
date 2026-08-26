#include "Audit/AuditTextureCommandlet.h"

// Editor-only by design: drives the editor Asset Registry. Trap any Runtime-type drift early.
static_assert(WITH_EDITOR, "UAssetWorkbench commandlets are editor-only, keep the uplugin Module Type=Editor.");

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/EngineTypes.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Engine/TextureLODSettings.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/EnumClassFlags.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"
#include "UAssetWorkbenchVersion.h"

namespace
{
    constexpr int32 kSizeBudgetCharacter = 2048;
    constexpr int32 kSizeBudgetProp = 2048;
    constexpr int32 kSizeBudgetWorld = 2048;
    constexpr int32 kSizeBudgetVFX = 1024;

    constexpr int32 kNeverStreamMinSize = 1024;
    constexpr int32 kUncompressedMaxSize = 512;

    // Texture to material to instance to mesh to blueprint is the longest chain worth classifying.
    constexpr int32 kReferencerWalkDepth = 4;
    constexpr int32 kDependencyWalkDepth = 6;

    const TCHAR* kAllRules[] =
    {
        TEXT("T1"), TEXT("T2"), TEXT("T3"), TEXT("T4"), TEXT("T5"),
        TEXT("T6"), TEXT("T7"), TEXT("T8"), TEXT("T9"), TEXT("T10"),
        TEXT("T11"), TEXT("T12"), TEXT("T13"), TEXT("T14"), TEXT("T15")
    };

    const TCHAR* kSeverityError = TEXT("Error");
    const TCHAR* kSeverityWarning = TEXT("Warning");
    const TCHAR* kSeverityInfo = TEXT("Info");

    enum class ETextureUsage : uint8
    {
        None = 0,
        UI = 1 << 0,
        Character = 1 << 1,
        Prop = 1 << 2,
        World = 1 << 3,
        VFX = 1 << 4
    };
    ENUM_CLASS_FLAGS(ETextureUsage);

    struct FTextureSettings
    {
        TextureGroup LODGroup = TEXTUREGROUP_World;
        TextureCompressionSettings Compression = TC_Default;
        TextureMipGenSettings MipGen = TMGS_FromTextureGroup;
        ETexturePowerOfTwoSetting::Type PowerOfTwoMode = ETexturePowerOfTwoSetting::None;
        ETextureLossyCompressionAmount LossyCompression = TLCA_Default;
        bool bSRGB = false;
        bool bNeverStream = false;
        bool bVirtualTextureStreaming = false;
        bool bFlipGreenChannel = false;
        bool bPowerOfTwo = true;
        int32 MaxTextureSize = 0;
        int32 SourceX = 0;
        int32 SourceY = 0;
        int32 BuiltX = 0;
        int32 BuiltY = 0;
    };

    struct FTextureRecord
    {
        FString AssetPath;
        FName PackageName;
        FTextureSettings Settings;
        ETextureUsage Usage = ETextureUsage::None;
        bool bHasReferencer = false;
        bool bLoaded = false;
    };

    struct FFinding
    {
        FString Asset;
        FString Rule;
        FString Severity;
        FString Property;
        FString Current;
        FString Expected;
        FString Context;
    };

    struct FAuditOptions
    {
        TArray<FString> EntryAssets;
        FString ScanDir;
        FString ReportPath;
        TSet<FString> Rules;
    };

    struct FAuditState
    {
        TMap<FString, FTextureRecord> Records;
        TArray<FString> RecordOrder;
        TArray<FFinding> Findings;
        TSet<FString> SeenFindings;
        const ITargetPlatform* Platform = nullptr;
        const UTextureLODSettings* LODSettings = nullptr;
    };

    bool IsRuleActive(const FAuditOptions& Options, const TCHAR* Rule)
    {
        return Options.Rules.IsEmpty() || Options.Rules.Contains(Rule);
    }

    FString EnumName(const UEnum* Enum, int64 Value)
    {
        return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
    }

    FString GroupName(TextureGroup Value)
    {
        return EnumName(StaticEnum<TextureGroup>(), Value);
    }

    FString CompressionName(TextureCompressionSettings Value)
    {
        return EnumName(StaticEnum<TextureCompressionSettings>(), Value);
    }

    FString MipGenName(TextureMipGenSettings Value)
    {
        return EnumName(StaticEnum<TextureMipGenSettings>(), Value);
    }

    FString PowerOfTwoName(ETexturePowerOfTwoSetting::Type Value)
    {
        return EnumName(StaticEnum<ETexturePowerOfTwoSetting::Type>(), Value);
    }

    FString LossyName(ETextureLossyCompressionAmount Value)
    {
        return EnumName(StaticEnum<ETextureLossyCompressionAmount>(), Value);
    }

    FString SamplerName(EMaterialSamplerType Value)
    {
        return EnumName(StaticEnum<EMaterialSamplerType>(), Value);
    }

    const TCHAR* BoolText(bool bValue)
    {
        return bValue ? TEXT("true") : TEXT("false");
    }

    bool IsNormalSampler(EMaterialSamplerType Sampler)
    {
        return Sampler == SAMPLERTYPE_Normal || Sampler == SAMPLERTYPE_VirtualNormal;
    }

    bool IsMaskSampler(EMaterialSamplerType Sampler)
    {
        return Sampler == SAMPLERTYPE_Masks || Sampler == SAMPLERTYPE_VirtualMasks;
    }

    bool IsAlphaSampler(EMaterialSamplerType Sampler)
    {
        return Sampler == SAMPLERTYPE_Alpha || Sampler == SAMPLERTYPE_VirtualAlpha;
    }

    bool IsSingleChannelSampler(EMaterialSamplerType Sampler)
    {
        const bool bGrayscale = Sampler == SAMPLERTYPE_Grayscale || Sampler == SAMPLERTYPE_VirtualGrayscale;
        const bool bLinearGrayscale = Sampler == SAMPLERTYPE_LinearGrayscale || Sampler == SAMPLERTYPE_VirtualLinearGrayscale;
        return bGrayscale || bLinearGrayscale || IsAlphaSampler(Sampler);
    }

    bool IsLinearOnlyCompression(TextureCompressionSettings Compression)
    {
        const bool bMaskLike = Compression == TC_Masks || Compression == TC_Alpha || Compression == TC_Normalmap;
        const bool bFloat = Compression == TC_HDR || Compression == TC_HDR_F32 || Compression == TC_HDR_Compressed;
        const bool bScalar = Compression == TC_SingleFloat || Compression == TC_HalfFloat;
        return bMaskLike || bFloat || bScalar;
    }

    bool IsNormalMapGroup(TextureGroup Group)
    {
        const bool bWorld = Group == TEXTUREGROUP_WorldNormalMap;
        const bool bCharacter = Group == TEXTUREGROUP_CharacterNormalMap;
        const bool bWeapon = Group == TEXTUREGROUP_WeaponNormalMap;
        const bool bVehicle = Group == TEXTUREGROUP_VehicleNormalMap;
        const bool bImpostor = Group == TEXTUREGROUP_ImpostorNormalDepth;
        return bWorld || bCharacter || bWeapon || bVehicle || bImpostor;
    }

    FString UsageText(ETextureUsage Usage)
    {
        TArray<FString> Parts;
        if (EnumHasAnyFlags(Usage, ETextureUsage::UI))
        {
            Parts.Add(TEXT("UI"));
        }
        if (EnumHasAnyFlags(Usage, ETextureUsage::Character))
        {
            Parts.Add(TEXT("Character"));
        }
        if (EnumHasAnyFlags(Usage, ETextureUsage::Prop))
        {
            Parts.Add(TEXT("Prop"));
        }
        if (EnumHasAnyFlags(Usage, ETextureUsage::World))
        {
            Parts.Add(TEXT("World"));
        }
        if (EnumHasAnyFlags(Usage, ETextureUsage::VFX))
        {
            Parts.Add(TEXT("VFX"));
        }

        return Parts.IsEmpty() ? FString(TEXT("Unknown")) : FString::Join(Parts, TEXT("+"));
    }

    // 0 means no size budget applies, which is what an unclassified or UI-only texture gets.
    int32 ResolveSizeBudget(ETextureUsage Usage)
    {
        if (Usage == ETextureUsage::VFX)
        {
            return kSizeBudgetVFX;
        }
        if (EnumHasAnyFlags(Usage, ETextureUsage::Character))
        {
            return kSizeBudgetCharacter;
        }
        if (EnumHasAnyFlags(Usage, ETextureUsage::Prop))
        {
            return kSizeBudgetProp;
        }
        if (EnumHasAnyFlags(Usage, ETextureUsage::World))
        {
            return kSizeBudgetWorld;
        }

        return 0;
    }

    TextureGroup ResolveExpectedNormalGroup(ETextureUsage Usage)
    {
        if (EnumHasAnyFlags(Usage, ETextureUsage::Character))
        {
            return TEXTUREGROUP_CharacterNormalMap;
        }

        return TEXTUREGROUP_WorldNormalMap;
    }

    TextureMipGenSettings ResolveEffectiveMipGen(const FTextureSettings& Settings, const UTextureLODSettings* LODSettings)
    {
        if (Settings.MipGen != TMGS_FromTextureGroup)
        {
            return Settings.MipGen;
        }
        if (!LODSettings)
        {
            return TMGS_SimpleAverage;
        }

        return LODSettings->GetTextureLODGroup(Settings.LODGroup).MipGenSettings;
    }

    FString ToPackageName(const FString& AssetPath)
    {
        FString PackageName = AssetPath;
        int32 ObjectDelimiter = INDEX_NONE;
        if (PackageName.FindChar(TEXT('.'), ObjectDelimiter))
        {
            PackageName.LeftInline(ObjectDelimiter);
        }

        return PackageName;
    }

    bool IsTextureAsset(const FAssetData& Asset)
    {
        return Asset.AssetClassPath.GetAssetName() == FName(TEXT("Texture2D"));
    }

    bool IsMaterialAsset(const FAssetData& Asset)
    {
        const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
        return ClassName == TEXT("Material") || ClassName.StartsWith(TEXT("MaterialInstance"));
    }

    ETextureUsage ClassifyConsumer(const FAssetData& Asset)
    {
        const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();

        if (ClassName == TEXT("SkeletalMesh"))
        {
            return ETextureUsage::Character;
        }
        if (ClassName == TEXT("StaticMesh"))
        {
            return ETextureUsage::Prop;
        }
        if (ClassName == TEXT("World"))
        {
            return ETextureUsage::World;
        }
        if (ClassName.StartsWith(TEXT("Niagara")))
        {
            return ETextureUsage::VFX;
        }

        const bool bWidget = ClassName.StartsWith(TEXT("WidgetBlueprint")) || ClassName == TEXT("UserWidget");
        const bool bSlate = ClassName.StartsWith(TEXT("Slate")) || ClassName == TEXT("Font");
        if (bWidget || bSlate)
        {
            return ETextureUsage::UI;
        }

        return ETextureUsage::None;
    }

    void WalkReferencers(IAssetRegistry& Registry, FName PackageName, int32 Depth, TSet<FName>& Visited, ETextureUsage& OutUsage, bool& bOutHasReferencer)
    {
        if (Depth <= 0)
        {
            return;
        }

        TArray<FName> Referencers;
        Registry.GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

        for (FName Referencer : Referencers)
        {
            const FString ReferencerName = Referencer.ToString();
            if (Referencer == PackageName || ReferencerName.StartsWith(TEXT("/Script/")))
            {
                continue;
            }

            bOutHasReferencer = true;

            if (Visited.Contains(Referencer))
            {
                continue;
            }
            Visited.Add(Referencer);

            TArray<FAssetData> Assets;
            Registry.GetAssetsByPackageName(Referencer, Assets, true);

            ETextureUsage Local = ETextureUsage::None;
            for (const FAssetData& Asset : Assets)
            {
                Local |= ClassifyConsumer(Asset);
            }
            OutUsage |= Local;

            // A layer that classifies as nothing, a material or a blueprint, is pass-through, keep climbing.
            if (Local == ETextureUsage::None)
            {
                bool bIgnored = false;
                WalkReferencers(Registry, Referencer, Depth - 1, Visited, OutUsage, bIgnored);
            }
        }
    }

    template <typename TEnumType>
    bool ReadEnumTag(const FAssetData& Asset, const TCHAR* TagName, TEnumType& OutValue)
    {
        FString Raw;
        if (!Asset.GetTagValue(FName(TagName), Raw))
        {
            return false;
        }

        const UEnum* Enum = StaticEnum<TEnumType>();
        const int64 Value = Enum ? Enum->GetValueByNameString(Raw) : INDEX_NONE;
        if (Value == INDEX_NONE)
        {
            return false;
        }

        OutValue = static_cast<TEnumType>(Value);
        return true;
    }

    bool ReadBoolTag(const FAssetData& Asset, const TCHAR* TagName, bool& OutValue)
    {
        FString Raw;
        if (!Asset.GetTagValue(FName(TagName), Raw))
        {
            return false;
        }

        OutValue = Raw.ToBool();
        return true;
    }

    bool ReadIntTag(const FAssetData& Asset, const TCHAR* TagName, int32& OutValue)
    {
        FString Raw;
        if (!Asset.GetTagValue(FName(TagName), Raw))
        {
            return false;
        }

        OutValue = FCString::Atoi(*Raw);
        return true;
    }

    bool ReadDimensionsTag(const FAssetData& Asset, int32& OutX, int32& OutY)
    {
        FString Raw;
        if (!Asset.GetTagValue(FName(TEXT("Dimensions")), Raw))
        {
            return false;
        }

        FString Left;
        FString Right;
        if (!Raw.Split(TEXT("x"), &Left, &Right))
        {
            return false;
        }

        OutX = FCString::Atoi(*Left);
        OutY = FCString::Atoi(*Right);
        return OutX > 0 && OutY > 0;
    }

    // False once any tag the rules need is absent, which forces the record into the load pass.
    bool ReadSettingsFromTags(const FAssetData& Asset, FTextureSettings& OutSettings)
    {
        bool bComplete = true;

        bComplete &= ReadEnumTag(Asset, TEXT("LODGroup"), OutSettings.LODGroup);
        bComplete &= ReadEnumTag(Asset, TEXT("CompressionSettings"), OutSettings.Compression);
        bComplete &= ReadEnumTag(Asset, TEXT("MipGenSettings"), OutSettings.MipGen);
        bComplete &= ReadEnumTag(Asset, TEXT("PowerOfTwoMode"), OutSettings.PowerOfTwoMode);
        bComplete &= ReadBoolTag(Asset, TEXT("SRGB"), OutSettings.bSRGB);
        bComplete &= ReadBoolTag(Asset, TEXT("NeverStream"), OutSettings.bNeverStream);
        bComplete &= ReadBoolTag(Asset, TEXT("VirtualTextureStreaming"), OutSettings.bVirtualTextureStreaming);
        bComplete &= ReadIntTag(Asset, TEXT("MaxTextureSize"), OutSettings.MaxTextureSize);
        bComplete &= ReadDimensionsTag(Asset, OutSettings.SourceX, OutSettings.SourceY);

        OutSettings.bPowerOfTwo = FMath::IsPowerOfTwo(OutSettings.SourceX) && FMath::IsPowerOfTwo(OutSettings.SourceY);
        if (OutSettings.PowerOfTwoMode != ETexturePowerOfTwoSetting::None)
        {
            OutSettings.bPowerOfTwo = true;
        }

        return bComplete;
    }

    void ReadSettingsFromTexture(UTexture* Texture, const ITargetPlatform* Platform, FTextureSettings& OutSettings)
    {
        OutSettings.LODGroup = Texture->LODGroup.GetValue();
        OutSettings.Compression = Texture->CompressionSettings.GetValue();
        OutSettings.MipGen = Texture->MipGenSettings.GetValue();
        OutSettings.PowerOfTwoMode = Texture->PowerOfTwoMode.GetValue();
        OutSettings.LossyCompression = Texture->LossyCompressionAmount.GetValue();
        OutSettings.bSRGB = Texture->SRGB != 0;
        OutSettings.bNeverStream = Texture->NeverStream != 0;
        OutSettings.bVirtualTextureStreaming = Texture->VirtualTextureStreaming != 0;
        OutSettings.bFlipGreenChannel = Texture->bFlipGreenChannel != 0;
        OutSettings.MaxTextureSize = Texture->MaxTextureSize;
        OutSettings.SourceX = static_cast<int32>(Texture->Source.GetSizeX());
        OutSettings.SourceY = static_cast<int32>(Texture->Source.GetSizeY());

        OutSettings.bPowerOfTwo = Texture->Source.AreAllBlocksPowerOfTwo() && FMath::IsPowerOfTwo(Texture->Source.GetVolumeSizeZ());
        const bool bPaddingApplied = Texture->PowerOfTwoMode != ETexturePowerOfTwoSetting::None;
        if (bPaddingApplied || Texture->Source.IsLongLatCubemap())
        {
            OutSettings.bPowerOfTwo = true;
        }

        if (Platform)
        {
            int32 IgnoredZ = 0;
            Texture->GetBuiltTextureSize(Platform, OutSettings.BuiltX, OutSettings.BuiltY, IgnoredZ);
        }
    }

    void AddFinding(FAuditState& State, const FString& DedupKey, FFinding&& Finding)
    {
        if (State.SeenFindings.Contains(DedupKey))
        {
            return;
        }

        State.SeenFindings.Add(DedupKey);
        State.Findings.Add(MoveTemp(Finding));
    }

    void AddTextureFinding(FAuditState& State, const FString& AssetPath, const TCHAR* Rule, const TCHAR* Severity, const TCHAR* Property, const FString& Current, const FString& Expected, const FString& Context)
    {
        FFinding Finding;
        Finding.Asset = AssetPath;
        Finding.Rule = Rule;
        Finding.Severity = Severity;
        Finding.Property = Property;
        Finding.Current = Current;
        Finding.Expected = Expected;
        Finding.Context = Context;

        AddFinding(State, FString::Printf(TEXT("%s|%s"), *AssetPath, Rule), MoveTemp(Finding));
    }
}

namespace
{
    bool ParseOptions(const FString& Params, FAuditOptions& OutOptions)
    {
        OutOptions.EntryAssets = UAssetWorkbench::ParseAssetPaths(Params);

        // A hand-rolled -assets= with an empty value makes FParse::Value swallow the next flag.
        OutOptions.EntryAssets.RemoveAll([](const FString& Path) { return Path.IsEmpty() || Path.StartsWith(TEXT("-")); });

        if (!FParse::Value(*Params, TEXT("-scandir="), OutOptions.ScanDir, false) || OutOptions.ScanDir.IsEmpty())
        {
            OutOptions.ScanDir = TEXT("/Game");
        }
        OutOptions.ScanDir.TrimQuotesInline();

        if (!FParse::Value(*Params, TEXT("-report="), OutOptions.ReportPath, false) || OutOptions.ReportPath.IsEmpty())
        {
            OutOptions.ReportPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Intermediate"), TEXT("AuditTexture"), TEXT("report.json"));
        }
        OutOptions.ReportPath.TrimQuotesInline();

        for (const FString& Rule : UAssetWorkbench::ParsePathList(Params, TEXT("-rules=")))
        {
            const FString Upper = Rule.ToUpper();
            bool bKnown = false;
            for (const TCHAR* Known : kAllRules)
            {
                if (Upper == Known)
                {
                    bKnown = true;
                    break;
                }
            }

            if (!bKnown)
            {
                UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Unknown rule %s. Accepted: T1 T2 T3 T4 T5 T6 T7 T8 T9 T10 T11 T12 T13 T14 T15"), *Rule);
                return false;
            }

            OutOptions.Rules.Add(Upper);
        }

        return true;
    }

    // Entry assets are walked forward through the dependency graph, a scan dir is a plain registry filter.
    bool CollectCandidateAssets(const FAuditOptions& Options, IAssetRegistry& Registry, TArray<FAssetData>& OutAssets)
    {
        if (Options.EntryAssets.IsEmpty())
        {
            FARFilter Filter;
            Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
            Filter.bRecursiveClasses = false;
            Filter.PackagePaths.Add(FName(*Options.ScanDir));
            Filter.bRecursivePaths = true;

            Registry.GetAssets(Filter, OutAssets);
            return true;
        }

        TSet<FName> Visited;
        TArray<FName> Frontier;

        for (const FString& Entry : Options.EntryAssets)
        {
            const FString EntryPackage = ToPackageName(Entry);
            if (!FPackageName::IsValidLongPackageName(EntryPackage) || !FPackageName::DoesPackageExist(EntryPackage))
            {
                UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Unknown asset path: %s"), *Entry);
                return false;
            }

            const FName EntryName(*EntryPackage);
            Visited.Add(EntryName);
            Frontier.Add(EntryName);
        }

        for (int32 Depth = 0; Depth < kDependencyWalkDepth && Frontier.Num() > 0; ++Depth)
        {
            TArray<FName> NextFrontier;

            for (FName Current : Frontier)
            {
                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Current, Assets, true);

                bool bIsTexture = false;
                for (const FAssetData& Asset : Assets)
                {
                    if (IsTextureAsset(Asset))
                    {
                        bIsTexture = true;
                        OutAssets.Add(Asset);
                    }
                }

                if (bIsTexture)
                {
                    continue;
                }

                TArray<FName> Dependencies;
                Registry.GetDependencies(Current, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);

                for (FName Dependency : Dependencies)
                {
                    const FString DependencyName = Dependency.ToString();
                    if (DependencyName.StartsWith(TEXT("/Script/")) || Visited.Contains(Dependency))
                    {
                        continue;
                    }

                    Visited.Add(Dependency);
                    NextFrontier.Add(Dependency);
                }
            }

            Frontier = MoveTemp(NextFrontier);
        }

        return true;
    }

    bool ShouldLoadRecord(const FAuditOptions& Options, const FTextureRecord& Record, const UTextureLODSettings* LODSettings, bool bTagsComplete)
    {
        if (!bTagsComplete)
        {
            return true;
        }

        const FTextureSettings& Settings = Record.Settings;
        const int32 Budget = ResolveSizeBudget(Record.Usage);
        const int32 SourceMax = FMath::Max(Settings.SourceX, Settings.SourceY);

        // Built size never exceeds source size, so a texture already inside budget cannot trip T7.
        if (IsRuleActive(Options, TEXT("T7")) && Budget > 0 && SourceMax > Budget)
        {
            return true;
        }

        const TextureMipGenSettings EffectiveMipGen = ResolveEffectiveMipGen(Settings, LODSettings);
        const bool bMipped = EffectiveMipGen != TMGS_NoMipmaps;
        if (IsRuleActive(Options, TEXT("T8")) && !Settings.bPowerOfTwo && Settings.PowerOfTwoMode == ETexturePowerOfTwoSetting::None && bMipped)
        {
            return true;
        }

        if (IsRuleActive(Options, TEXT("T13")) && Settings.Compression == TC_Normalmap)
        {
            return true;
        }

        return false;
    }

    void BuildRecords(const FAuditOptions& Options, IAssetRegistry& Registry, const TArray<FAssetData>& Assets, FAuditState& State)
    {
        for (const FAssetData& Asset : Assets)
        {
            const FString AssetPath = Asset.GetObjectPathString();
            if (State.Records.Contains(AssetPath))
            {
                continue;
            }

            FTextureRecord Record;
            Record.AssetPath = AssetPath;
            Record.PackageName = Asset.PackageName;

            const bool bTagsComplete = ReadSettingsFromTags(Asset, Record.Settings);

            TSet<FName> Visited;
            Visited.Add(Asset.PackageName);
            WalkReferencers(Registry, Asset.PackageName, kReferencerWalkDepth, Visited, Record.Usage, Record.bHasReferencer);

            const bool bNeedsLoad = ShouldLoadRecord(Options, Record, State.LODSettings, bTagsComplete);

            State.Records.Add(AssetPath, MoveTemp(Record));
            State.RecordOrder.Add(AssetPath);

            if (bNeedsLoad)
            {
                UTexture* Texture = LoadObject<UTexture>(nullptr, *AssetPath);
                if (!Texture)
                {
                    UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("Failed to load texture: %s"), *AssetPath);
                    continue;
                }

                FTextureRecord& Stored = State.Records[AssetPath];
                ReadSettingsFromTexture(Texture, State.Platform, Stored.Settings);
                Stored.bLoaded = true;
            }
        }
    }

    void EvaluateMaterial(const FAuditOptions& Options, FAuditState& State, UMaterialInterface* MaterialInterface)
    {
        UMaterial* BaseMaterial = MaterialInterface->GetMaterial();
        if (!BaseMaterial)
        {
            return;
        }

        const FString MaterialPath = MaterialInterface->GetPathName();
        TArray<TPair<UTexture*, EMaterialSamplerType>> Samples;

        // An instance only contributes its overrides, the parent contributes its own expressions on its own pass.
        if (UMaterialInstance* Instance = Cast<UMaterialInstance>(MaterialInterface))
        {
            for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
            {
                if (!Value.ParameterValue)
                {
                    continue;
                }

                for (UMaterialExpression* Expression : BaseMaterial->GetExpressions())
                {
                    UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression);
                    if (Parameter && Parameter->ParameterName == Value.ParameterInfo.Name)
                    {
                        Samples.Emplace(Value.ParameterValue, Parameter->SamplerType.GetValue());
                        break;
                    }
                }
            }
        }
        else
        {
            for (UMaterialExpression* Expression : BaseMaterial->GetExpressions())
            {
                UMaterialExpressionTextureBase* TextureExpression = Cast<UMaterialExpressionTextureBase>(Expression);
                if (!TextureExpression || !TextureExpression->Texture)
                {
                    continue;
                }

                Samples.Emplace(TextureExpression->Texture, TextureExpression->SamplerType.GetValue());
            }
        }

        TArray<UTexture*> NormalTextures;

        for (const TPair<UTexture*, EMaterialSamplerType>& Sample : Samples)
        {
            UTexture* Texture = Sample.Key;
            const FString AssetPath = Texture->GetPathName();
            FTextureRecord* Record = State.Records.Find(AssetPath);
            if (!Record)
            {
                continue;
            }

            if (!Record->bLoaded)
            {
                ReadSettingsFromTexture(Texture, State.Platform, Record->Settings);
                Record->bLoaded = true;
            }

            const EMaterialSamplerType Sampler = Sample.Value;
            const FTextureSettings& Settings = Record->Settings;

            if (IsRuleActive(Options, TEXT("T1")))
            {
                FString SamplerError;
                const EMaterialSamplerType Correct = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Texture);
                if (!UMaterialExpressionTextureBase::VerifySamplerType(AssetPath, Correct, Settings.bSRGB, Sampler, SamplerError))
                {
                    FFinding Finding;
                    Finding.Asset = AssetPath;
                    Finding.Rule = TEXT("T1");
                    Finding.Severity = kSeverityError;
                    Finding.Property = TEXT("SamplerType");
                    Finding.Current = SamplerName(Sampler);
                    Finding.Expected = SamplerName(Correct);
                    Finding.Context = FString::Printf(TEXT("%s : %s"), *MaterialPath, *SamplerError);

                    AddFinding(State, FString::Printf(TEXT("%s|T1|%s"), *AssetPath, *MaterialPath), MoveTemp(Finding));
                }
            }

            if (IsRuleActive(Options, TEXT("T2")) && IsNormalSampler(Sampler) && Settings.Compression != TC_Normalmap)
            {
                AddTextureFinding(State, AssetPath, TEXT("T2"), kSeverityError, TEXT("CompressionSettings"), CompressionName(Settings.Compression), TEXT("TC_Normalmap"), FString::Printf(TEXT("sampled as %s in %s"), *SamplerName(Sampler), *MaterialPath));
            }

            const bool bLinearSampler = IsNormalSampler(Sampler) || IsMaskSampler(Sampler);
            if (IsRuleActive(Options, TEXT("T3")) && bLinearSampler && Settings.bSRGB)
            {
                AddTextureFinding(State, AssetPath, TEXT("T3"), kSeverityError, TEXT("SRGB"), BoolText(true), TEXT("false"), FString::Printf(TEXT("sampled as %s in %s"), *SamplerName(Sampler), *MaterialPath));
            }

            if (IsRuleActive(Options, TEXT("T12")) && IsSingleChannelSampler(Sampler) && Settings.Compression == TC_Default)
            {
                const TCHAR* Expected = IsAlphaSampler(Sampler) ? TEXT("TC_Alpha") : TEXT("TC_Grayscale");
                AddTextureFinding(State, AssetPath, TEXT("T12"), kSeverityWarning, TEXT("CompressionSettings"), CompressionName(Settings.Compression), Expected, FString::Printf(TEXT("sampled as %s in %s"), *SamplerName(Sampler), *MaterialPath));
            }

            if (IsNormalSampler(Sampler))
            {
                NormalTextures.AddUnique(Texture);
            }
        }

        if (!IsRuleActive(Options, TEXT("T15")) || NormalTextures.Num() < 2)
        {
            return;
        }

        bool bAnyFlipped = false;
        bool bAnyStraight = false;
        for (UTexture* Texture : NormalTextures)
        {
            const bool bFlipped = Texture->bFlipGreenChannel != 0;
            bAnyFlipped |= bFlipped;
            bAnyStraight |= !bFlipped;
        }

        if (!bAnyFlipped || !bAnyStraight)
        {
            return;
        }

        for (UTexture* Texture : NormalTextures)
        {
            AddTextureFinding(State, Texture->GetPathName(), TEXT("T15"), kSeverityInfo, TEXT("bFlipGreenChannel"), BoolText(Texture->bFlipGreenChannel != 0), FString(), FString::Printf(TEXT("normal maps of %s disagree"), *MaterialPath));
        }
    }

    void RunMaterialPass(const FAuditOptions& Options, IAssetRegistry& Registry, FAuditState& State)
    {
        const bool bSamplerRules = IsRuleActive(Options, TEXT("T1")) || IsRuleActive(Options, TEXT("T2")) || IsRuleActive(Options, TEXT("T3"));
        const bool bChannelRules = IsRuleActive(Options, TEXT("T12")) || IsRuleActive(Options, TEXT("T15"));
        if (!bSamplerRules && !bChannelRules)
        {
            return;
        }

        TSet<FName> MaterialPackages;
        for (const FString& AssetPath : State.RecordOrder)
        {
            TArray<FName> Referencers;
            Registry.GetReferencers(State.Records[AssetPath].PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

            for (FName Referencer : Referencers)
            {
                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Referencer, Assets, true);

                for (const FAssetData& Asset : Assets)
                {
                    if (IsMaterialAsset(Asset))
                    {
                        MaterialPackages.Add(Referencer);
                    }
                }
            }
        }

        UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditTexture: loading %d referencing material(s) for usage rules"), MaterialPackages.Num());

        for (FName MaterialPackage : MaterialPackages)
        {
            TArray<FAssetData> Assets;
            Registry.GetAssetsByPackageName(MaterialPackage, Assets, true);

            for (const FAssetData& Asset : Assets)
            {
                if (!IsMaterialAsset(Asset))
                {
                    continue;
                }

                UMaterialInterface* MaterialInterface = LoadObject<UMaterialInterface>(nullptr, *Asset.GetObjectPathString());
                if (!MaterialInterface)
                {
                    UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("Failed to load material: %s"), *Asset.GetObjectPathString());
                    continue;
                }

                EvaluateMaterial(Options, State, MaterialInterface);
            }
        }
    }

    void RunTextureRules(const FAuditOptions& Options, FAuditState& State)
    {
        for (const FString& AssetPath : State.RecordOrder)
        {
            const FTextureRecord& Record = State.Records[AssetPath];
            const FTextureSettings& Settings = Record.Settings;

            const TextureMipGenSettings EffectiveMipGen = ResolveEffectiveMipGen(Settings, State.LODSettings);
            const bool bMipped = EffectiveMipGen != TMGS_NoMipmaps;
            const int32 Budget = ResolveSizeBudget(Record.Usage);
            const int32 SourceMax = FMath::Max(Settings.SourceX, Settings.SourceY);
            const int32 BuiltMax = FMath::Max(Settings.BuiltX, Settings.BuiltY);
            const FString SourceText = FString::Printf(TEXT("%dx%d"), Settings.SourceX, Settings.SourceY);

            if (IsRuleActive(Options, TEXT("T4")) && Settings.bSRGB && IsLinearOnlyCompression(Settings.Compression))
            {
                AddTextureFinding(State, AssetPath, TEXT("T4"), kSeverityError, TEXT("SRGB"), BoolText(true), TEXT("false"), FString::Printf(TEXT("CompressionSettings=%s is linear only"), *CompressionName(Settings.Compression)));
            }

            const bool bUIOnly = Record.Usage == ETextureUsage::UI;
            if (IsRuleActive(Options, TEXT("T5")) && bUIOnly && Settings.LODGroup != TEXTUREGROUP_UI)
            {
                AddTextureFinding(State, AssetPath, TEXT("T5"), kSeverityWarning, TEXT("LODGroup"), GroupName(Settings.LODGroup), TEXT("TEXTUREGROUP_UI"), TEXT("only UI assets reference it"));
            }

            if (IsRuleActive(Options, TEXT("T5")) && Settings.Compression == TC_Normalmap && !IsNormalMapGroup(Settings.LODGroup))
            {
                const TextureGroup ExpectedGroup = ResolveExpectedNormalGroup(Record.Usage);
                AddTextureFinding(State, AssetPath, TEXT("T5"), kSeverityWarning, TEXT("LODGroup"), GroupName(Settings.LODGroup), GroupName(ExpectedGroup), FString::Printf(TEXT("normal map, usage=%s"), *UsageText(Record.Usage)));
            }

            if (IsRuleActive(Options, TEXT("T6")) && Settings.LODGroup == TEXTUREGROUP_UI && bMipped)
            {
                AddTextureFinding(State, AssetPath, TEXT("T6"), kSeverityWarning, TEXT("MipGenSettings"), MipGenName(Settings.MipGen), TEXT("TMGS_NoMipmaps"), FString::Printf(TEXT("UI group resolves to %s"), *MipGenName(EffectiveMipGen)));
            }

            if (IsRuleActive(Options, TEXT("T7")) && Budget > 0 && BuiltMax > Budget)
            {
                AddTextureFinding(State, AssetPath, TEXT("T7"), kSeverityWarning, TEXT("MaxTextureSize"), FString::Printf(TEXT("%dx%d"), Settings.BuiltX, Settings.BuiltY), FString::FromInt(Budget), FString::Printf(TEXT("usage=%s budget=%d"), *UsageText(Record.Usage), Budget));
            }

            if (IsRuleActive(Options, TEXT("T8")) && !Settings.bPowerOfTwo && Settings.PowerOfTwoMode == ETexturePowerOfTwoSetting::None && bMipped)
            {
                AddTextureFinding(State, AssetPath, TEXT("T8"), kSeverityError, TEXT("PowerOfTwoMode"), PowerOfTwoName(Settings.PowerOfTwoMode), TEXT("PadToPowerOfTwo"), FString::Printf(TEXT("source %s with mips"), *SourceText));
            }

            const bool bUncompressed = Settings.Compression == TC_EditorIcon || Settings.Compression == TC_VectorDisplacementmap;
            if (IsRuleActive(Options, TEXT("T9")) && bUncompressed && SourceMax >= kUncompressedMaxSize)
            {
                AddTextureFinding(State, AssetPath, TEXT("T9"), kSeverityError, TEXT("CompressionSettings"), CompressionName(Settings.Compression), TEXT("TC_Default"), FString::Printf(TEXT("uncompressed at source %s"), *SourceText));
            }

            const bool bStreamExempt = Settings.LODGroup == TEXTUREGROUP_UI || Settings.LODGroup == TEXTUREGROUP_ColorLookupTable;
            if (IsRuleActive(Options, TEXT("T10")) && Settings.bNeverStream && !bStreamExempt && SourceMax >= kNeverStreamMinSize)
            {
                AddTextureFinding(State, AssetPath, TEXT("T10"), kSeverityWarning, TEXT("NeverStream"), BoolText(true), TEXT("false"), FString::Printf(TEXT("group=%s source %s"), *GroupName(Settings.LODGroup), *SourceText));
            }

            const bool bSmallGroupVT = Settings.LODGroup == TEXTUREGROUP_UI || Settings.LODGroup == TEXTUREGROUP_ColorLookupTable || Settings.LODGroup == TEXTUREGROUP_Pixels2D;
            if (IsRuleActive(Options, TEXT("T11")) && Settings.bVirtualTextureStreaming && bSmallGroupVT)
            {
                AddTextureFinding(State, AssetPath, TEXT("T11"), kSeverityWarning, TEXT("VirtualTextureStreaming"), BoolText(true), TEXT("false"), FString::Printf(TEXT("group=%s"), *GroupName(Settings.LODGroup)));
            }

            const bool bLossySet = Settings.LossyCompression != TLCA_Default && Settings.LossyCompression != TLCA_None;
            if (IsRuleActive(Options, TEXT("T13")) && Record.bLoaded && Settings.Compression == TC_Normalmap && bLossySet)
            {
                AddTextureFinding(State, AssetPath, TEXT("T13"), kSeverityInfo, TEXT("LossyCompressionAmount"), LossyName(Settings.LossyCompression), FString(), TEXT("RDO on a normal map"));
            }

            if (IsRuleActive(Options, TEXT("T14")) && !Record.bHasReferencer)
            {
                AddTextureFinding(State, AssetPath, TEXT("T14"), kSeverityInfo, TEXT(""), FString(), FString(), TEXT("no referencing package"));
            }
        }
    }

    bool WriteReport(const FAuditOptions& Options, const FAuditState& State, int32 Scanned, int32& OutErrors, int32& OutWarnings, int32& OutInfos)
    {
        TArray<TSharedPtr<FJsonValue>> FindingsJson;
        TArray<FString> SpecOrder;
        TMap<FString, TSharedPtr<FJsonObject>> SpecProperties;

        for (const FFinding& Finding : State.Findings)
        {
            if (Finding.Severity == kSeverityError)
            {
                ++OutErrors;
            }
            else if (Finding.Severity == kSeverityWarning)
            {
                ++OutWarnings;
            }
            else
            {
                ++OutInfos;
            }

            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("Asset"), Finding.Asset);
            Item->SetStringField(TEXT("Rule"), Finding.Rule);
            Item->SetStringField(TEXT("Severity"), Finding.Severity);
            Item->SetStringField(TEXT("Property"), Finding.Property);
            Item->SetStringField(TEXT("Current"), Finding.Current);
            Item->SetStringField(TEXT("Expected"), Finding.Expected);
            Item->SetStringField(TEXT("Context"), Finding.Context);
            FindingsJson.Add(MakeShared<FJsonValueObject>(Item));

            // Info carries no fix, and a rule without a deterministic Expected cannot be handed to a writer.
            const bool bActionable = Finding.Severity != kSeverityInfo && !Finding.Property.IsEmpty() && !Finding.Expected.IsEmpty();
            if (!bActionable || Finding.Rule == TEXT("T1"))
            {
                continue;
            }

            TSharedPtr<FJsonObject>& Properties = SpecProperties.FindOrAdd(Finding.Asset);
            if (!Properties.IsValid())
            {
                Properties = MakeShared<FJsonObject>();
                SpecOrder.Add(Finding.Asset);
            }

            if (!Properties->HasField(Finding.Property))
            {
                Properties->SetStringField(Finding.Property, Finding.Expected);
            }
        }

        TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("Error"), OutErrors);
        Summary->SetNumberField(TEXT("Warning"), OutWarnings);
        Summary->SetNumberField(TEXT("Info"), OutInfos);

        TArray<TSharedPtr<FJsonValue>> TargetsJson;
        for (const FString& AssetPath : SpecOrder)
        {
            TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
            Target->SetStringField(TEXT("AssetPath"), AssetPath);
            Target->SetObjectField(TEXT("Properties"), SpecProperties[AssetPath]);
            TargetsJson.Add(MakeShared<FJsonValueObject>(Target));
        }

        TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
        Spec->SetArrayField(TEXT("Targets"), TargetsJson);

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("RunName"), TEXT("AuditTexture"));
        Root->SetNumberField(TEXT("Scanned"), Scanned);
        Root->SetArrayField(TEXT("Findings"), FindingsJson);
        Root->SetObjectField(TEXT("Summary"), Summary);
        Root->SetObjectField(TEXT("Spec"), Spec);

        if (!UAssetWorkbench::SaveJsonToFile(Root, Options.ReportPath))
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Error, TEXT("Failed to write report: %s"), *Options.ReportPath);
            return false;
        }

        UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditTexture: report written: %s"), *Options.ReportPath);
        return true;
    }
}

UAuditTextureCommandlet::UAuditTextureCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UAuditTextureCommandlet::Main(const FString& Params)
{
    if (UAssetWorkbench::AbortIfLiveEditor())
    {
        return ToExitCode(EUAssetWorkbenchExitType::EditorConflict);
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("UAssetWorkbench v%s - AuditTexture"), UASSET_WORKBENCH_VERSION_STRING);

    FAuditOptions Options;
    if (!ParseOptions(Params, Options))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditTexture: scanning asset registry..."));
    AssetRegistry.SearchAllAssets(true);

    FAuditState State;
    State.Platform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
    if (State.Platform)
    {
        State.LODSettings = &State.Platform->GetTextureLODSettings();
    }
    else
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditTexture: no running target platform, size and group rules fall back to source dimensions"));
    }

    TArray<FAssetData> Candidates;
    if (!CollectCandidateAssets(Options, AssetRegistry, Candidates))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    BuildRecords(Options, AssetRegistry, Candidates, State);

    if (State.RecordOrder.IsEmpty())
    {
        UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("AuditTexture: no textures in scope (assets=%d scandir=%s)"), Options.EntryAssets.Num(), *Options.ScanDir);
    }

    RunMaterialPass(Options, AssetRegistry, State);
    RunTextureRules(Options, State);

    int32 Errors = 0;
    int32 Warnings = 0;
    int32 Infos = 0;
    if (!WriteReport(Options, State, State.RecordOrder.Num(), Errors, Warnings, Infos))
    {
        return ToExitCode(EUAssetWorkbenchExitType::Failed);
    }

    for (const FFinding& Finding : State.Findings)
    {
        if (Finding.Severity == kSeverityError || Finding.Severity == kSeverityWarning)
        {
            UE_LOG(LogUAssetWorkbenchAuditor, Warning, TEXT("[%s] %s %s %s: %s -> %s (%s)"), *Finding.Severity, *Finding.Rule, *Finding.Asset, *Finding.Property, *Finding.Current, *Finding.Expected, *Finding.Context);
        }
    }

    UE_LOG(LogUAssetWorkbenchAuditor, Display, TEXT("AuditTexture: complete. scanned=%d errors=%d warnings=%d infos=%d"), State.RecordOrder.Num(), Errors, Warnings, Infos);

    const bool bIssues = Errors > 0 || Warnings > 0;
    return ToExitCode(bIssues ? EUAssetWorkbenchExitType::IssuesFound : EUAssetWorkbenchExitType::Success);
}
