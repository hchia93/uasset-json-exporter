#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Interface.h"

namespace
{
    FString DescribeInterfaces(const UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            if (const UClass* InterfaceClass = InterfaceDesc.Interface.Get())
            {
                Names.Add(InterfaceClass->GetClassPathName().ToString());
            }
        }

        if (Names.IsEmpty())
        {
            return TEXT("<none>");
        }

        return FString::Join(Names, TEXT(", "));
    }

    bool IsImplemented(const UBlueprint* Blueprint, const UClass* InterfaceClass)
    {
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            if (InterfaceDesc.Interface.Get() == InterfaceClass)
            {
                return true;
            }
        }

        return false;
    }

    class FBlueprintInterfaceWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Interfaces");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Interfaces must be an array of ops"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                FString Path;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Interface"), Path))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Interfaces entry needs Op and Interface"), *Context.AssetPath);
                    return false;
                }

                if (!ApplyOp(Context, Desc, Op, Path))
                {
                    return false;
                }

                ++Context.Ops;
                Context.bNeedsStructuralRecompile = true;
            }

            return true;
        }

    private:
        bool ApplyOp(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Op, const FString& Path) const
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Path);
            }

            if (Op == TEXT("Remove"))
            {
                return ApplyRemove(Context, Desc, Path);
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown interface op '%s'. Expected Add or Remove"), *Context.AssetPath, *Op);
            return false;
        }

        // ImplementNewInterface asserts on a path it cannot resolve, so nothing reaches it unloaded or unchecked.
        // A Blueprint interface asset resolves to the Blueprint, its class is the generated one beside it.
        UClass* ResolveInterfaceClass(const FBlueprintEditContext& Context, const FString& Path) const
        {
            UClass* InterfaceClass = LoadObject<UClass>(nullptr, *Path);
            if (!InterfaceClass)
            {
                if (const UBlueprint* InterfaceBlueprint = LoadObject<UBlueprint>(nullptr, *Path))
                {
                    InterfaceClass = InterfaceBlueprint->GeneratedClass;
                }
            }

            if (!InterfaceClass)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot resolve interface '%s'. Implemented: %s"), *Context.AssetPath, *Path, *DescribeInterfaces(Context.Blueprint));
                return nullptr;
            }

            if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface) || InterfaceClass == UInterface::StaticClass())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: '%s' is not an interface class. Implemented: %s"), *Context.AssetPath, *Path, *DescribeInterfaces(Context.Blueprint));
                return nullptr;
            }

            return InterfaceClass;
        }

        bool ApplyAdd(FBlueprintEditContext& Context, const FString& Path) const
        {
            UClass* InterfaceClass = ResolveInterfaceClass(Context, Path);
            if (!InterfaceClass)
            {
                return false;
            }

            if (IsImplemented(Context.Blueprint, InterfaceClass))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s already implements '%s'. Implemented: %s"), *Context.AssetPath, *Path, *DescribeInterfaces(Context.Blueprint));
                return false;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: + interface '%s'"), *Context.Blueprint->GetName(), *InterfaceClass->GetClassPathName().ToString());

            if (!FBlueprintEditorUtils::ImplementNewInterface(Context.Blueprint, InterfaceClass->GetClassPathName()))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: ImplementNewInterface refused '%s', a graph or function of the same name is already there"), *Context.AssetPath, *Path);
                return false;
            }

            return true;
        }

        bool ApplyRemove(FBlueprintEditContext& Context, const TSharedPtr<FJsonObject>& Desc, const FString& Path) const
        {
            UClass* InterfaceClass = ResolveInterfaceClass(Context, Path);
            if (!InterfaceClass)
            {
                return false;
            }

            if (!IsImplemented(Context.Blueprint, InterfaceClass))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s does not implement '%s'. Implemented: %s"), *Context.AssetPath, *Path, *DescribeInterfaces(Context.Blueprint));
                return false;
            }

            bool bPreserveFunctions = false;
            Desc->TryGetBoolField(TEXT("PreserveFunctions"), bPreserveFunctions);

            UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: remove interface '%s' (PreserveFunctions %s)"), *Context.Blueprint->GetName(), *InterfaceClass->GetClassPathName().ToString(), bPreserveFunctions ? TEXT("true") : TEXT("false"));

            FBlueprintEditorUtils::RemoveInterface(Context.Blueprint, InterfaceClass->GetClassPathName(), bPreserveFunctions);
            return true;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintInterfaceWriter()
{
    return MakeUnique<FBlueprintInterfaceWriter>();
}
