#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"

namespace
{
    UObject* ResolveTargetObject(UBlueprint* Blueprint, const FString& ComponentName)
    {
        UObject* DefaultObject = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        if (ComponentName.IsEmpty())
        {
            return DefaultObject;
        }

        // A component the Blueprint declares itself lives on its SCS node. The CDO gets a copy only
        // after a compile, and under a _GEN_VARIABLE name, so the template is the thing to write to.
        if (Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node && Node->ComponentTemplate && Node->GetVariableName().ToString() == ComponentName)
                {
                    return Node->ComponentTemplate;
                }
            }
        }

        // Inherited native component, where the Blueprint keeps its override as a CDO sub-object.
        return DefaultObject ? StaticFindObjectFast(UObject::StaticClass(), DefaultObject, FName(*ComponentName)) : nullptr;
    }

    class FBlueprintDefaultsWriter : public IBlueprintWriter
    {
    public:
        virtual const TCHAR* GetSpecKey() const override
        {
            return TEXT("Defaults");
        }

        virtual bool Apply(FBlueprintEditContext& Context, const TSharedPtr<FJsonValue>& Section) override
        {
            const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
            if (!Section->TryGetArray(Entries))
            {
                UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Defaults must be an array of scopes"), *Context.AssetPath);
                return false;
            }

            for (const TSharedPtr<FJsonValue>& Value : *Entries)
            {
                const TSharedPtr<FJsonObject>& Desc = Value->AsObject();
                if (!Desc.IsValid())
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Defaults carries an entry that is not an object"), *Context.AssetPath);
                    return false;
                }

                const TSharedPtr<FJsonObject>* Properties = nullptr;
                if (!Desc->TryGetObjectField(TEXT("Properties"), Properties))
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s: Defaults entry has no Properties"), *Context.AssetPath);
                    return false;
                }

                // Empty component means the actor CDO itself.
                FString ComponentName;
                Desc->TryGetStringField(TEXT("Component"), ComponentName);

                const FString Scope = ComponentName.IsEmpty() ? TEXT("<self>") : ComponentName;
                UE_LOG(LogUAssetWorkbenchEditor, Display, TEXT("  %s: defaults on %s (%d propert(ies))"), *Context.Blueprint->GetName(), *Scope, (*Properties)->Values.Num());
                Context.Ops += (*Properties)->Values.Num();

                if (!Context.bApply)
                {
                    continue;
                }

                UObject* TargetObject = ResolveTargetObject(Context.Blueprint, ComponentName);
                if (!TargetObject)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no component named '%s'"), *Context.AssetPath, *ComponentName);
                    return false;
                }

                TargetObject->Modify();

                int32 Failures = 0;
                UAssetWorkbench::ApplyProperties(TargetObject, *Properties, Failures);
                if (Failures > 0)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%d propert(ies) failed on %s"), Failures, *Context.AssetPath);
                    return false;
                }
            }

            Context.bNeedsStructuralRecompile = true;
            return true;
        }
    };
}

TUniquePtr<IBlueprintWriter> MakeBlueprintDefaultsWriter()
{
    return MakeUnique<FBlueprintDefaultsWriter>();
}
