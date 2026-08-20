#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "SubobjectData.h"
#include "SubobjectDataSubsystem.h"
#include "UObject/UObjectIterator.h"

namespace
{
    UClass* ResolveComponentClass(const FString& ClassPath)
    {
        if (UClass* Found = LoadClass<UActorComponent>(nullptr, *ClassPath))
        {
            return Found;
        }

        // Bare name fallback, so a spec can say "StaticMeshComponent" without the /Script prefix.
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (Candidate->IsChildOf(UActorComponent::StaticClass()) && Candidate->GetName() == ClassPath)
            {
                return Candidate;
            }
        }

        return nullptr;
    }

    FSubobjectDataHandle FindHandleByName(USubobjectDataSubsystem* Subsystem, const TArray<FSubobjectDataHandle>& Handles, const FString& Name)
    {
        for (const FSubobjectDataHandle& Handle : Handles)
        {
            FSubobjectData Data;
            if (Subsystem->K2_FindSubobjectDataFromHandle(Handle, Data) && Data.GetVariableName().ToString() == Name)
            {
                return Handle;
            }
        }

        return FSubobjectDataHandle::InvalidHandle;
    }

    class FBlueprintComponentWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Components");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
            if (!Section->TryGetArray(Operations))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Components must be an array of operations"), *Context.AssetPath);
                return false;
            }

            USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
            if (!Subsystem)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("SubobjectDataSubsystem unavailable"));
                return false;
            }

            TArray<FSubobjectDataHandle> Handles;
            Subsystem->K2_GatherSubobjectDataForBlueprint(Context.Blueprint, Handles);
            if (Handles.IsEmpty())
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no subobject data"), *Context.AssetPath);
                return false;
            }

            // First handle is the actor itself, every component hangs off it.
            const FSubobjectDataHandle RootHandle = Handles[0];

            for (const TSharedPtr<FJsonValue>& Value : *Operations)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                FString Op;
                FString Name;
                if (!Desc.IsValid() || !Desc->TryGetStringField(TEXT("Op"), Op) || !Desc->TryGetStringField(TEXT("Name"), Name))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: component operation needs Op and Name"), *Context.AssetPath);
                    return false;
                }

                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: component %s %s"), *Context.Blueprint->GetName(), *Op, *Name);
                ++Context.Ops;

                if (!Context.bApply)
                {
                    continue;
                }

                if (!ApplyOne(Context, Subsystem, Handles, RootHandle, Op, Name, Desc))
                {
                    return false;
                }
            }

            Context.bNeedsStructuralRecompile = true;
            return true;
        }

    private:
        bool ApplyOne(FBlueprintEditContext& Context, USubobjectDataSubsystem* Subsystem, TArray<FSubobjectDataHandle>& Handles,
            const FSubobjectDataHandle& RootHandle, const FString& Op, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            if (Op == TEXT("Add"))
            {
                return ApplyAdd(Context, Subsystem, Handles, RootHandle, Name, Desc);
            }

            if (Op == TEXT("Remove"))
            {
                const FSubobjectDataHandle Handle = FindHandleByName(Subsystem, Handles, Name);
                if (Handle == FSubobjectDataHandle::InvalidHandle)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no component named '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                if (Subsystem->DeleteSubobject(RootHandle, Handle, Context.Blueprint) <= 0)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: could not remove '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                Handles.Remove(Handle);
                return true;
            }

            if (Op == TEXT("Rename"))
            {
                FString NewName;
                if (!Desc->TryGetStringField(TEXT("NewName"), NewName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Rename needs a NewName"), *Context.AssetPath);
                    return false;
                }

                const FSubobjectDataHandle Handle = FindHandleByName(Subsystem, Handles, Name);
                if (Handle == FSubobjectDataHandle::InvalidHandle)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: no component named '%s'"), *Context.AssetPath, *Name);
                    return false;
                }

                if (!Subsystem->RenameSubobject(Handle, FText::FromString(NewName)))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: could not rename '%s' to '%s'"), *Context.AssetPath, *Name, *NewName);
                    return false;
                }

                return true;
            }

            if (Op == TEXT("Reparent"))
            {
                FString ParentName;
                if (!Desc->TryGetStringField(TEXT("Parent"), ParentName))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Reparent needs a Parent"), *Context.AssetPath);
                    return false;
                }

                const FSubobjectDataHandle Handle = FindHandleByName(Subsystem, Handles, Name);
                const FSubobjectDataHandle ParentHandle = FindHandleByName(Subsystem, Handles, ParentName);
                if (Handle == FSubobjectDataHandle::InvalidHandle || ParentHandle == FSubobjectDataHandle::InvalidHandle)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Reparent cannot resolve '%s' or '%s'"), *Context.AssetPath, *Name, *ParentName);
                    return false;
                }

                if (!Subsystem->AttachSubobject(ParentHandle, Handle))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: could not reparent '%s' under '%s'"), *Context.AssetPath, *Name, *ParentName);
                    return false;
                }

                return true;
            }

            UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: unknown component Op '%s'"), *Context.AssetPath, *Op);
            return false;
        }

        bool ApplyAdd(FBlueprintEditContext& Context, USubobjectDataSubsystem* Subsystem, TArray<FSubobjectDataHandle>& Handles,
            const FSubobjectDataHandle& RootHandle, const FString& Name, const TSharedPtr<FJsonObject>& Desc) const
        {
            FString ClassPath;
            if (!Desc->TryGetStringField(TEXT("Class"), ClassPath))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Add needs a Class"), *Context.AssetPath);
                return false;
            }

            UClass* ComponentClass = ResolveComponentClass(ClassPath);
            if (!ComponentClass)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot resolve component class '%s'"), *Context.AssetPath, *ClassPath);
                return false;
            }

            // A re-run must not quietly produce a second copy under an engine-suffixed name.
            if (FindHandleByName(Subsystem, Handles, Name) != FSubobjectDataHandle::InvalidHandle)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: already has a component named '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            FSubobjectDataHandle ParentHandle = RootHandle;
            FString ParentName;
            if (Desc->TryGetStringField(TEXT("Parent"), ParentName))
            {
                ParentHandle = FindHandleByName(Subsystem, Handles, ParentName);
                if (ParentHandle == FSubobjectDataHandle::InvalidHandle)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: parent '%s' not found"), *Context.AssetPath, *ParentName);
                    return false;
                }
            }

            UObject* AssetOverride = nullptr;
            FString AssetForComponent;
            if (Desc->TryGetStringField(TEXT("Asset"), AssetForComponent))
            {
                AssetOverride = LoadObject<UObject>(nullptr, *AssetForComponent);
                if (!AssetOverride)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: cannot load asset '%s'"), *Context.AssetPath, *AssetForComponent);
                    return false;
                }
            }

            FAddNewSubobjectParams AddParams;
            AddParams.ParentHandle = ParentHandle;
            AddParams.NewClass = ComponentClass;
            AddParams.BlueprintContext = Context.Blueprint;
            AddParams.AssetOverride = AssetOverride;

            FText FailReason;
            const FSubobjectDataHandle NewHandle = Subsystem->AddNewSubobject(AddParams, FailReason);
            if (NewHandle == FSubobjectDataHandle::InvalidHandle)
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: AddNewSubobject failed for '%s': %s"), *Context.AssetPath, *Name, *FailReason.ToString());
                return false;
            }

            // AddNewSubobject names from the class, the spec name is applied after the fact.
            if (!Subsystem->RenameSubobject(NewHandle, FText::FromString(Name)))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: could not rename new component to '%s'"), *Context.AssetPath, *Name);
                return false;
            }

            Handles.Add(NewHandle);
            return true;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintComponentWriter()
{
    return MakeUnique<FBlueprintComponentWriter>();
}
