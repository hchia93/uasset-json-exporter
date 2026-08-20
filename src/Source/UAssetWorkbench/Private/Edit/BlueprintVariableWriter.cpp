#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace
{
    // Spec type word to pin category. Names follow what the editor's variable type dropdown shows,
    // not the PC_ constants, so a spec reads like the UI it replaces.
    bool ResolvePinCategory(const FString& Type, FName& OutCategory, FName& OutSubCategory)
    {
        OutSubCategory = NAME_None;

        static const TMap<FString, FName> Simple =
        {
            { TEXT("bool"),        UEdGraphSchema_K2::PC_Boolean },
            { TEXT("byte"),        UEdGraphSchema_K2::PC_Byte },
            { TEXT("enum"),        UEdGraphSchema_K2::PC_Byte },
            { TEXT("int"),         UEdGraphSchema_K2::PC_Int },
            { TEXT("int64"),       UEdGraphSchema_K2::PC_Int64 },
            { TEXT("string"),      UEdGraphSchema_K2::PC_String },
            { TEXT("name"),        UEdGraphSchema_K2::PC_Name },
            { TEXT("text"),        UEdGraphSchema_K2::PC_Text },
            { TEXT("object"),      UEdGraphSchema_K2::PC_Object },
            { TEXT("class"),       UEdGraphSchema_K2::PC_Class },
            { TEXT("softobject"),  UEdGraphSchema_K2::PC_SoftObject },
            { TEXT("softclass"),   UEdGraphSchema_K2::PC_SoftClass },
            { TEXT("struct"),      UEdGraphSchema_K2::PC_Struct },
        };

        if (const FName* Found = Simple.Find(Type))
        {
            OutCategory = *Found;
            return true;
        }

        // Floats are PC_Real plus a width subcategory, "float" alone would land on a wildcard width.
        if (Type == TEXT("float") || Type == TEXT("real"))
        {
            OutCategory = UEdGraphSchema_K2::PC_Real;
            OutSubCategory = UEdGraphSchema_K2::PC_Float;
            return true;
        }

        if (Type == TEXT("double"))
        {
            OutCategory = UEdGraphSchema_K2::PC_Real;
            OutSubCategory = UEdGraphSchema_K2::PC_Double;
            return true;
        }

        return false;
    }

    bool ResolveContainerType(const FString& Container, EPinContainerType& OutContainer)
    {
        if (Container.IsEmpty() || Container == TEXT("None"))
        {
            OutContainer = EPinContainerType::None;
            return true;
        }
        if (Container == TEXT("Array"))
        {
            OutContainer = EPinContainerType::Array;
            return true;
        }
        if (Container == TEXT("Set"))
        {
            OutContainer = EPinContainerType::Set;
            return true;
        }
        if (Container == TEXT("Map"))
        {
            OutContainer = EPinContainerType::Map;
            return true;
        }

        return false;
    }

    bool BuildPinType(const FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, FEdGraphPinType& OutType)
    {
        FString Type;
        if (!Desc->TryGetStringField(TEXT("Type"), Type))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable Add needs a Type"), *Context.AssetPath);
            return false;
        }

        FName Category;
        FName SubCategory;
        if (!ResolvePinCategory(Type, Category, SubCategory))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown variable Type '%s'"), *Context.AssetPath, *Type);
            return false;
        }

        OutType.PinCategory = Category;
        OutType.PinSubCategory = SubCategory;

        const bool bNeedsSubObject = Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
            || Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass
            || Category == UEdGraphSchema_K2::PC_Struct || Type == TEXT("enum");

        FString SubType;
        if (Desc->TryGetStringField(TEXT("SubType"), SubType))
        {
            UObject* Resolved = LoadObject<UObject>(nullptr, *SubType);
            if (!Resolved)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot resolve SubType '%s'"), *Context.AssetPath, *SubType);
                return false;
            }
            OutType.PinSubCategoryObject = Resolved;
        }
        else if (bNeedsSubObject)
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Type '%s' needs a SubType"), *Context.AssetPath, *Type);
            return false;
        }

        FString Container;
        Desc->TryGetStringField(TEXT("Container"), Container);

        EPinContainerType ContainerType = EPinContainerType::None;
        if (!ResolveContainerType(Container, ContainerType))
        {
            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown Container '%s'"), *Context.AssetPath, *Container);
            return false;
        }
        OutType.ContainerType = ContainerType;

        return true;
    }

    class FBlueprintVariableWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Variables");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Variables must be an array of operations"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                FString Name;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Name"), Name))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable operation needs Op and Name"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: variable %s %s"), *Context.Blueprint->GetName(), *Op, *Name);
                ++Context.Ops;

                if (!Context.bApply)
                {
                    continue;
                }

                if (!ApplyOne(Context, Op, Name, Desc))
                {
                    return false;
                }
            }

            Context.bNeedsStructuralRecompile = true;
            return true;
        }

    private:
        bool ApplyOne(FBlueprintEditContext& Context, const FString& Op, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            const FName VarName(*Name);

            if (Op == TEXT("Add"))
            {
                if (FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName) != INDEX_NONE)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: already has a variable named '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                FEdGraphPinType PinType;
                if (!BuildPinType(Context, Desc, PinType))
                {
                    return false;
                }

                FString DefaultValue;
                Desc->TryGetStringField(TEXT("Default"), DefaultValue);

                if (!FBlueprintEditorUtils::AddMemberVariable(Context.Blueprint, VarName, PinType, DefaultValue))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddMemberVariable failed for '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                FString Category;
                if (Desc->TryGetStringField(TEXT("Category"), Category))
                {
                    FBlueprintEditorUtils::SetBlueprintVariableCategory(Context.Blueprint, VarName, nullptr, FText::FromString(Category), /* bDontRecompile */ true);
                }

                return true;
            }

            if (Op == TEXT("Remove"))
            {
                if (FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName) == INDEX_NONE)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no variable named '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                FBlueprintEditorUtils::RemoveMemberVariable(Context.Blueprint, VarName);
                return true;
            }

            if (Op == TEXT("Rename"))
            {
                FString NewName;
                if (!Desc->TryGetStringField(TEXT("NewName"), NewName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: variable Rename needs a NewName"), *Context.AssetPath);
                    return false;
                }

                if (FBlueprintEditorUtils::FindNewVariableIndex(Context.Blueprint, VarName) == INDEX_NONE)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no variable named '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                FBlueprintEditorUtils::RenameMemberVariable(Context.Blueprint, VarName, FName(*NewName));
                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown variable Op '%s'"), *Context.AssetPath, *Op);
            return false;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintVariableWriter()
{
    return MakeUnique<FBlueprintVariableWriter>();
}
