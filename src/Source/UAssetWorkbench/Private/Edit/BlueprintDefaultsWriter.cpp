#include "Edit/BlueprintWriter.h"
#include "UAssetWorkbenchModule.h"
#include "UAssetWorkbenchUtil.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "UObject/UObjectHash.h"

namespace
{
    USCS_Node* FindOwnSCSNode(UBlueprint* Blueprint, const FString& ComponentName)
    {
        if (!Blueprint->SimpleConstructionScript)
        {
            return nullptr;
        }

        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node && Node->ComponentTemplate && Node->GetVariableName().ToString() == ComponentName)
            {
                return Node;
            }
        }

        return nullptr;
    }

    USCS_Node* FindInheritedSCSNode(UBlueprint* Blueprint, const FString& ComponentName)
    {
        for (UClass* Ancestor = Blueprint->ParentClass; Ancestor; Ancestor = Ancestor->GetSuperClass())
        {
            UBlueprintGeneratedClass* GeneratedAncestor = Cast<UBlueprintGeneratedClass>(Ancestor);
            if (!GeneratedAncestor || !GeneratedAncestor->SimpleConstructionScript)
            {
                continue;
            }

            for (USCS_Node* Node : GeneratedAncestor->SimpleConstructionScript->GetAllNodes())
            {
                if (Node && Node->ComponentTemplate && Node->GetVariableName().ToString() == ComponentName)
                {
                    return Node;
                }
            }
        }

        return nullptr;
    }

    // A component a parent Blueprint declares is not editable in place. This Blueprint keeps its own
    // override template for it, which the Components panel creates the same way on first edit.
    UActorComponent* GetOrCreateOverrideTemplate(UBlueprint* Blueprint, USCS_Node* InheritedNode)
    {
        const FComponentKey Key(InheritedNode);
        const bool bCanOverride = Key.IsValid() && Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(Key.GetComponentOwner());
        if (!bCanOverride)
        {
            return nullptr;
        }

        UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(/* bCreateIfNecessary */ true);
        if (!Handler)
        {
            return nullptr;
        }

        if (UActorComponent* Existing = Handler->GetOverridenComponentTemplate(Key))
        {
            return Existing;
        }

        return Handler->CreateOverridenComponentTemplate(Key);
    }

    UObject* ResolveTargetObject(UBlueprint* Blueprint, const FString& ComponentName)
    {
        UObject* DefaultObject = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        if (ComponentName.IsEmpty())
        {
            return DefaultObject;
        }

        // A component the Blueprint declares itself lives on its SCS node. The CDO gets a copy only
        // after a compile, and under a _GEN_VARIABLE name, so the template is the thing to write to.
        if (USCS_Node* OwnNode = FindOwnSCSNode(Blueprint, ComponentName))
        {
            return OwnNode->ComponentTemplate;
        }

        if (USCS_Node* InheritedNode = FindInheritedSCSNode(Blueprint, ComponentName))
        {
            return GetOrCreateOverrideTemplate(Blueprint, InheritedNode);
        }

        // Inherited native component, where the Blueprint keeps its override as a CDO sub-object.
        return DefaultObject ? StaticFindObjectFast(UObject::StaticClass(), DefaultObject, FName(*ComponentName)) : nullptr;
    }

    FString JoinOrNone(const TArray<FString>& Names)
    {
        return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("<none>");
    }

    FString DescribeOwnComponents(UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        if (Blueprint->SimpleConstructionScript)
        {
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node)
                {
                    Names.Add(Node->GetVariableName().ToString());
                }
            }
        }

        return JoinOrNone(Names);
    }

    FString DescribeInheritedComponents(UBlueprint* Blueprint)
    {
        TArray<FString> Names;
        for (UClass* Ancestor = Blueprint->ParentClass; Ancestor; Ancestor = Ancestor->GetSuperClass())
        {
            UBlueprintGeneratedClass* GeneratedAncestor = Cast<UBlueprintGeneratedClass>(Ancestor);
            if (!GeneratedAncestor || !GeneratedAncestor->SimpleConstructionScript)
            {
                continue;
            }

            for (USCS_Node* Node : GeneratedAncestor->SimpleConstructionScript->GetAllNodes())
            {
                if (Node)
                {
                    Names.Add(Node->GetVariableName().ToString());
                }
            }
        }

        return JoinOrNone(Names);
    }

    FString DescribeNativeSubObjects(UBlueprint* Blueprint)
    {
        UObject* DefaultObject = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
        if (!DefaultObject)
        {
            return TEXT("<none>");
        }

        TArray<FString> Names;
        ForEachObjectWithOuter(DefaultObject, [&Names](UObject* SubObject)
        {
            Names.Add(SubObject->GetName());
        }, /* bIncludeNestedObjects */ false);

        return JoinOrNone(Names);
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

                UObject* TargetObject = ResolveTargetObject(Context.Blueprint, ComponentName);
                if (!TargetObject)
                {
                    UE_LOG(LogUAssetWorkbenchEditor, Error, TEXT("%s has no component named '%s'. Own: %s. Inherited: %s. Native: %s"), *Context.AssetPath, *ComponentName, *DescribeOwnComponents(Context.Blueprint), *DescribeInheritedComponents(Context.Blueprint), *DescribeNativeSubObjects(Context.Blueprint));
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
