#include "Export/EdGraphJsonSerializer.h"

#include "Animation/AnimationAsset.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_BlendSpaceGraphBase.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateConduitNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputKey.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Select.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchString.h"
#include "K2Node_Timeline.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
    const TCHAR* PinContainerTypeName(EPinContainerType ContainerType)
    {
        switch (ContainerType)
        {
        case EPinContainerType::Array:
            return TEXT("Array");
        case EPinContainerType::Set:
            return TEXT("Set");
        case EPinContainerType::Map:
            return TEXT("Map");
        default:
            return TEXT("None");
        }
    }

    // SubType is the bare name a reader recognizes, SubTypePath is what an EditBlueprint spec takes back.
    void AppendPinTypeFields(const FEdGraphPinType& PinType, const TSharedPtr<FJsonObject>& PinObj)
    {
        PinObj->SetStringField(TEXT("Type"), PinType.PinCategory.ToString());

        if (PinType.PinSubCategoryObject.IsValid())
        {
            PinObj->SetStringField(TEXT("SubType"), PinType.PinSubCategoryObject->GetName());
            PinObj->SetStringField(TEXT("SubTypePath"), PinType.PinSubCategoryObject->GetPathName());
        }

        PinObj->SetStringField(TEXT("Container"), PinContainerTypeName(PinType.ContainerType));
    }

    void AppendUserDefinedPins(const UK2Node_EditablePinBase* Node, TArray<TSharedPtr<FJsonValue>>& OutArray)
    {
        if (!Node)
        {
            return;
        }

        for (const TSharedPtr<FUserPinInfo>& PinInfo : Node->UserDefinedPins)
        {
            if (!PinInfo.IsValid())
            {
                continue;
            }

            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("Name"), PinInfo->PinName.ToString());
            AppendPinTypeFields(PinInfo->PinType, PinObj);
            PinObj->SetBoolField(TEXT("IsReference"), PinInfo->PinType.bIsReference);

            if (!PinInfo->PinDefaultValue.IsEmpty())
            {
                PinObj->SetStringField(TEXT("Default"), PinInfo->PinDefaultValue);
            }

            OutArray.Add(MakeShared<FJsonValueObject>(PinObj));
        }
    }

    void AppendNodePins(const UEdGraphNode* Node, EEdGraphPinDirection Direction, TArray<TSharedPtr<FJsonValue>>& OutArray)
    {
        if (!Node)
        {
            return;
        }

        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != Direction || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }

            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetStringField(TEXT("Name"), Pin->PinName.ToString());
            AppendPinTypeFields(Pin->PinType, PinObj);
            PinObj->SetBoolField(TEXT("IsReference"), Pin->PinType.bIsReference);

            OutArray.Add(MakeShared<FJsonValueObject>(PinObj));
        }
    }

    TSharedPtr<FJsonObject> MakePinTypeObject(const FEdGraphPinType& PinType)
    {
        TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
        TypeObj->SetStringField(TEXT("Type"), PinType.PinCategory.ToString());

        if (PinType.PinSubCategoryObject.IsValid())
        {
            TypeObj->SetStringField(TEXT("SubType"), PinType.PinSubCategoryObject->GetName());
        }
        else if (!PinType.PinSubCategory.IsNone())
        {
            TypeObj->SetStringField(TEXT("SubType"), PinType.PinSubCategory.ToString());
        }

        return TypeObj;
    }

    TSharedPtr<FJsonObject> MakeTerminalTypeObject(const FEdGraphTerminalType& TerminalType)
    {
        TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
        TypeObj->SetStringField(TEXT("Type"), TerminalType.TerminalCategory.ToString());

        if (TerminalType.TerminalSubCategoryObject.IsValid())
        {
            TypeObj->SetStringField(TEXT("SubType"), TerminalType.TerminalSubCategoryObject->GetName());
        }
        else if (!TerminalType.TerminalSubCategory.IsNone())
        {
            TypeObj->SetStringField(TEXT("SubType"), TerminalType.TerminalSubCategory.ToString());
        }

        return TypeObj;
    }

    void AddEventNetFlags(uint32 FunctionFlags, const TSharedPtr<FJsonObject>& NodeObj)
    {
        if ((FunctionFlags & FUNC_NetFuncFlags) == 0)
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> FlagsArray;
        if ((FunctionFlags & FUNC_NetServer) != 0)
        {
            FlagsArray.Add(MakeShared<FJsonValueString>(TEXT("Server")));
        }
        if ((FunctionFlags & FUNC_NetClient) != 0)
        {
            FlagsArray.Add(MakeShared<FJsonValueString>(TEXT("Client")));
        }
        if ((FunctionFlags & FUNC_NetMulticast) != 0)
        {
            FlagsArray.Add(MakeShared<FJsonValueString>(TEXT("Multicast")));
        }
        if ((FunctionFlags & FUNC_NetReliable) != 0)
        {
            FlagsArray.Add(MakeShared<FJsonValueString>(TEXT("Reliable")));
        }

        if (FlagsArray.Num() > 0)
        {
            NodeObj->SetArrayField(TEXT("NetFlags"), FlagsArray);
        }
    }

    FString EffectivePinValue(const UEdGraphPin* Pin)
    {
        if (Pin->DefaultObject)
        {
            return Pin->DefaultObject->GetPathName();
        }
        if (!Pin->DefaultTextValue.IsEmpty())
        {
            return Pin->DefaultTextValue.ToString();
        }

        return Pin->DefaultValue;
    }

    // Node-level view of what a call actually passes, reading it off the pin array costs a second pass.
    TSharedPtr<FJsonObject> ExportCallArgs(const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input || Pin->bHidden || Pin->LinkedTo.Num() > 0)
            {
                continue;
            }

            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }

            // Bare Target resolves from graph context, an entry for it carries nothing
            const bool bIsSelfPin = Pin->PinName == UEdGraphSchema_K2::PN_Self || Pin->PinName == FName(TEXT("Target"));
            if (bIsSelfPin && !Pin->DefaultObject)
            {
                continue;
            }

            const FString Value = EffectivePinValue(Pin);
            if (Value.IsEmpty())
            {
                continue;
            }

            Args->SetStringField(Pin->PinName.ToString(), Value);
        }

        return Args->Values.Num() > 0 ? Args : nullptr;
    }

    // A sub-graph carries no tag of its own, the node that owns it is the only thing that names it.
    const TCHAR* GraphTypeForOwningNode(const UEdGraphNode* Node)
    {
        if (Node->IsA(UK2Node_Composite::StaticClass()))
        {
            return TEXT("Composite");
        }
        if (Node->IsA(UAnimGraphNode_StateMachineBase::StaticClass()))
        {
            return TEXT("StateMachine");
        }
        if (Node->IsA(UAnimGraphNode_BlendSpaceGraphBase::StaticClass()))
        {
            return TEXT("BlendSpace");
        }
        if (Node->IsA(UAnimStateNode::StaticClass()))
        {
            return TEXT("State");
        }
        if (Node->IsA(UAnimStateConduitNode::StaticClass()))
        {
            return TEXT("Conduit");
        }
        if (Node->IsA(UAnimStateTransitionNode::StaticClass()))
        {
            return TEXT("TransitionRule");
        }

        return TEXT("SubGraph");
    }

    // Every settable field, not a delta. A missing key has to mean the property does not exist,
    // a diff made a defaulted bLoopAnimation indistinguishable from an unexported one.
    TSharedPtr<FJsonObject> ExportAnimNodeSettings(const UAnimGraphNode_Base* AnimNode)
    {
        FStructProperty* NodeProp = AnimNode->GetFNodeProperty();
        if (!NodeProp || !NodeProp->Struct)
        {
            return nullptr;
        }

        const void* NodePtr = NodeProp->ContainerPtrToValuePtr<void>(AnimNode);

        FStructOnScope Defaults(NodeProp->Struct);
        const void* DefaultsPtr = Defaults.GetStructMemory();

        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();

        for (TFieldIterator<FProperty> PropIt(NodeProp->Struct); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(NodePtr);
            const void* DefaultPtr = Prop->ContainerPtrToValuePtr<void>(DefaultsPtr);

            // A touched struct or array keeps the delta form it already exported with. A defaulted one
            // passes its own address, the one input UE text export reads as "expand every field".
            const bool bIsDefault = Prop->Identical(ValuePtr, DefaultPtr, PPF_DeepComparison);
            const void* DeltaPtr = bIsDefault ? ValuePtr : DefaultPtr;

            FString Value;
            Prop->ExportTextItem_Direct(Value, ValuePtr, DeltaPtr, nullptr, PPF_None);
            if (!Value.IsEmpty())
            {
                Settings->SetStringField(Prop->GetName(), Value);
            }
        }

        return Settings->Values.Num() > 0 ? Settings : nullptr;
    }

    const TCHAR* PropertyBindingTypeName(EAnimGraphNodePropertyBindingType BindingType)
    {
        switch (BindingType)
        {
        case EAnimGraphNodePropertyBindingType::Property:
            return TEXT("Property");
        case EAnimGraphNodePropertyBindingType::Function:
            return TEXT("Function");
        default:
            return TEXT("None");
        }
    }

    // Bindings moved onto an instanced UAnimGraphNodeBinding whose concrete subclass owns the map behind a
    // private header, so the object and the map both come out by reflection.
    bool OpenBindingMap(const UAnimGraphNode_Base* AnimNode, const UObject*& OutBindingObject, const FMapProperty*& OutMapProperty)
    {
        const FObjectProperty* BindingProp = FindFProperty<FObjectProperty>(AnimNode->GetClass(), TEXT("Binding"));
        if (!BindingProp)
        {
            return false;
        }

        OutBindingObject = BindingProp->GetObjectPropertyValue(BindingProp->ContainerPtrToValuePtr<void>(AnimNode));
        if (!OutBindingObject)
        {
            return false;
        }

        OutMapProperty = FindFProperty<FMapProperty>(OutBindingObject->GetClass(), TEXT("PropertyBindings"));
        if (!OutMapProperty || !CastField<FNameProperty>(OutMapProperty->KeyProp))
        {
            return false;
        }

        const FStructProperty* ValueProp = CastField<FStructProperty>(OutMapProperty->ValueProp);
        return ValueProp && ValueProp->Struct && ValueProp->Struct->GetFName() == TEXT("AnimGraphNodePropertyBinding");
    }

    void AddPropertyBindings(const UAnimGraphNode_Base* AnimNode, const TSharedPtr<FJsonObject>& NodeObj)
    {
        const UObject* BindingObject = nullptr;
        const FMapProperty* MapProp = nullptr;
        if (!OpenBindingMap(AnimNode, BindingObject, MapProp))
        {
            return;
        }

        FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(BindingObject));

        TSharedPtr<FJsonObject> Bindings = MakeShared<FJsonObject>();

        for (FScriptMapHelper::FIterator It(MapHelper); It; ++It)
        {
            const FName* PropertyName = reinterpret_cast<const FName*>(MapHelper.GetKeyPtr(It));
            const FAnimGraphNodePropertyBinding* Binding = reinterpret_cast<const FAnimGraphNodePropertyBinding*>(MapHelper.GetValuePtr(It));
            if (!PropertyName || !Binding)
            {
                continue;
            }

            TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
            BindingObj->SetStringField(TEXT("Path"), FString::Join(Binding->PropertyPath, TEXT(".")));
            BindingObj->SetStringField(TEXT("PathAsText"), Binding->PathAsText.ToString());
            BindingObj->SetStringField(TEXT("Type"), PropertyBindingTypeName(Binding->Type));
            BindingObj->SetBoolField(TEXT("bIsBound"), Binding->bIsBound);
            BindingObj->SetBoolField(TEXT("bIsPromotion"), Binding->bIsPromotion);

            if (!Binding->ContextId.IsNone())
            {
                BindingObj->SetStringField(TEXT("ContextId"), Binding->ContextId.ToString());
            }

            if (!Binding->CompiledContext.IsEmpty())
            {
                BindingObj->SetStringField(TEXT("CompiledContext"), Binding->CompiledContext.ToString());
            }

            if (Binding->ArrayIndex != INDEX_NONE)
            {
                BindingObj->SetNumberField(TEXT("ArrayIndex"), Binding->ArrayIndex);
            }

            Bindings->SetObjectField(PropertyName->ToString(), BindingObj);
        }

        if (Bindings->Values.Num() > 0)
        {
            NodeObj->SetObjectField(TEXT("Bindings"), Bindings);
        }
    }

    // Settings carries the struct value either way, exposure says whether a pin overrides it at runtime.
    void AddExposedPins(const UAnimGraphNode_Base* AnimNode, const TSharedPtr<FJsonObject>& NodeObj)
    {
        TArray<TSharedPtr<FJsonValue>> ExposedArray;
        TArray<TSharedPtr<FJsonValue>> HiddenArray;

        for (const FOptionalPinFromProperty& OptionalPin : AnimNode->ShowPinForProperties)
        {
            if (OptionalPin.PropertyName.IsNone())
            {
                continue;
            }

            TSharedRef<FJsonValueString> PinName = MakeShared<FJsonValueString>(OptionalPin.PropertyName.ToString());
            if (OptionalPin.bShowPin)
            {
                ExposedArray.Add(PinName);
            }
            else
            {
                HiddenArray.Add(PinName);
            }
        }

        if (ExposedArray.Num() > 0)
        {
            NodeObj->SetArrayField(TEXT("ExposedPins"), ExposedArray);
        }

        if (HiddenArray.Num() > 0)
        {
            NodeObj->SetArrayField(TEXT("HiddenOptionalPins"), HiddenArray);
        }
    }

    // Property Access node ships behind a private header in a Developer plugin, reflection is the only route in
    const TArray<FString>* GetPropertyAccessSegments(const UEdGraphNode* Node)
    {
        if (!Node || Node->GetClass()->GetName() != TEXT("K2Node_PropertyAccess"))
        {
            return nullptr;
        }

        const FArrayProperty* PathProp = FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("Path"));
        if (!PathProp || !CastField<FStrProperty>(PathProp->Inner))
        {
            return nullptr;
        }

        const TArray<FString>* Path = PathProp->ContainerPtrToValuePtr<TArray<FString>>(Node);
        return (Path && !Path->IsEmpty()) ? Path : nullptr;
    }

    void AddPropertyAccessPath(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj)
    {
        if (Node->GetClass()->GetName() != TEXT("K2Node_PropertyAccess"))
        {
            return;
        }

        if (const FTextProperty* TextPathProp = FindFProperty<FTextProperty>(Node->GetClass(), TEXT("TextPath")))
        {
            const FText* TextPath = TextPathProp->ContainerPtrToValuePtr<FText>(Node);
            if (TextPath && !TextPath->IsEmpty())
            {
                NodeObj->SetStringField(TEXT("PropertyPath"), TextPath->ToString());
            }
        }

        const TArray<FString>* Path = GetPropertyAccessSegments(Node);
        if (!Path)
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> SegmentsArray;
        for (const FString& Segment : *Path)
        {
            SegmentsArray.Add(MakeShared<FJsonValueString>(Segment));
        }
        NodeObj->SetArrayField(TEXT("PropertyPathSegments"), SegmentsArray);

        // TextPath is display text ("Sprint State Is Turning"), the joined segments are what the binding resolves
        NodeObj->SetStringField(TEXT("ResolvedPath"), FString::Join(*Path, TEXT(".")));
    }
}

TSharedPtr<FJsonObject> EdGraphJson::ExportFunctionSignature(const UEdGraph* Graph, const UBlueprint* Blueprint, bool bInterfaceImplementation)
{
    if (!Graph)
    {
        return nullptr;
    }

    const UK2Node_FunctionEntry* FunctionEntry = nullptr;
    const UK2Node_FunctionResult* FunctionResult = nullptr;
    const UK2Node_Tunnel* TunnelEntry = nullptr;
    const UK2Node_Tunnel* TunnelExit = nullptr;

    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            FunctionEntry = Entry;
            continue;
        }

        if (const UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
        {
            FunctionResult = Result;
            continue;
        }

        // Macro entry and exit are both tunnels, only the allowed pin direction tells them apart.
        if (const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
        {
            if (Tunnel->bCanHaveOutputs)
            {
                TunnelEntry = Tunnel;
            }
            if (Tunnel->bCanHaveInputs)
            {
                TunnelExit = Tunnel;
            }
        }
    }

    TSharedPtr<FJsonObject> Signature = MakeShared<FJsonObject>();

    // Graph name doubles as the function name, a parent hit means the BP overrides an inherited function.
    const bool bOverride = Blueprint && Blueprint->ParentClass && Blueprint->ParentClass->FindFunctionByName(Graph->GetFName()) != nullptr;

    TArray<TSharedPtr<FJsonValue>> InputsArray;
    AppendUserDefinedPins(FunctionEntry, InputsArray);
    AppendUserDefinedPins(TunnelEntry, InputsArray);

    TArray<TSharedPtr<FJsonValue>> OutputsArray;
    AppendUserDefinedPins(FunctionResult, OutputsArray);
    AppendUserDefinedPins(TunnelExit, OutputsArray);

    // An inherited signature leaves UserDefinedPins empty, the params live on the parent UFunction. Read node pins instead.
    const bool bInheritedSignature = InputsArray.IsEmpty() && OutputsArray.IsEmpty() && (bOverride || bInterfaceImplementation);
    if (bInheritedSignature)
    {
        AppendNodePins(FunctionEntry, EGPD_Output, InputsArray);
        AppendNodePins(FunctionResult, EGPD_Input, OutputsArray);
    }

    Signature->SetArrayField(TEXT("Inputs"), InputsArray);
    Signature->SetArrayField(TEXT("Outputs"), OutputsArray);

    if (bInheritedSignature)
    {
        Signature->SetStringField(TEXT("SignatureSource"), TEXT("Inherited"));
    }

    if (FunctionEntry)
    {
        TArray<TSharedPtr<FJsonValue>> LocalVariablesArray;
        for (const FBPVariableDescription& LocalVariable : FunctionEntry->LocalVariables)
        {
            TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
            VarObj->SetStringField(TEXT("Name"), LocalVariable.VarName.ToString());
            AppendPinTypeFields(LocalVariable.VarType, VarObj);

            if (!LocalVariable.DefaultValue.IsEmpty())
            {
                VarObj->SetStringField(TEXT("Default"), LocalVariable.DefaultValue);
            }

            LocalVariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
        }

        if (LocalVariablesArray.Num() > 0)
        {
            Signature->SetArrayField(TEXT("LocalVariables"), LocalVariablesArray);
        }

        const int32 ExtraFlags = FunctionEntry->GetExtraFlags();

        if (ExtraFlags & FUNC_BlueprintPure)
        {
            Signature->SetBoolField(TEXT("Pure"), true);
        }
        if (ExtraFlags & FUNC_Const)
        {
            Signature->SetBoolField(TEXT("Const"), true);
        }
        if (ExtraFlags & FUNC_Static)
        {
            Signature->SetBoolField(TEXT("Static"), true);
        }

        const TCHAR* Access = TEXT("Public");
        if (ExtraFlags & FUNC_Private)
        {
            Access = TEXT("Private");
        }
        else if (ExtraFlags & FUNC_Protected)
        {
            Access = TEXT("Protected");
        }
        Signature->SetStringField(TEXT("Access"), Access);

        if (FunctionEntry->MetaData.bCallInEditor)
        {
            Signature->SetBoolField(TEXT("CallInEditor"), true);
        }
        if (!FunctionEntry->MetaData.Category.IsEmpty())
        {
            Signature->SetStringField(TEXT("Category"), FunctionEntry->MetaData.Category.ToString());
        }
        if (!FunctionEntry->MetaData.Keywords.IsEmpty())
        {
            Signature->SetStringField(TEXT("Keywords"), FunctionEntry->MetaData.Keywords.ToString());
        }
        if (!FunctionEntry->MetaData.ToolTip.IsEmpty())
        {
            Signature->SetStringField(TEXT("Tooltip"), FunctionEntry->MetaData.ToolTip.ToString());
        }
    }

    if (bOverride)
    {
        Signature->SetBoolField(TEXT("Override"), true);
    }

    return Signature;
}

FString EdGraphJson::GetPropertyAccessPath(const UEdGraphNode* Node)
{
    const TArray<FString>* Path = GetPropertyAccessSegments(Node);
    return Path ? FString::Join(*Path, TEXT(".")) : FString();
}

void EdGraphJson::AddK2NodeFields(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj)
{
    if (const UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
    {
        FName VarName = VarNode->VariableReference.GetMemberName();
        if (!VarName.IsNone())
        {
            NodeObj->SetStringField(TEXT("VariableName"), VarName.ToString());
        }
        if (UClass* VarOwner = VarNode->VariableReference.GetMemberParentClass())
        {
            NodeObj->SetStringField(TEXT("VariableOwner"), VarOwner->GetName());
        }
    }

    if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
    {
        if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
        {
            NodeObj->SetStringField(TEXT("MacroName"), MacroGraph->GetName());
            if (UPackage* MacroPkg = MacroGraph->GetOutermost())
            {
                NodeObj->SetStringField(TEXT("MacroPackage"), MacroPkg->GetName());
            }
        }

        TSharedPtr<FJsonObject> Args = ExportCallArgs(Node);
        if (Args.IsValid())
        {
            NodeObj->SetObjectField(TEXT("Args"), Args);
        }
    }

    if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
    {
        FName FunctionName = CallNode->FunctionReference.GetMemberName();
        if (!FunctionName.IsNone())
        {
            NodeObj->SetStringField(TEXT("FunctionName"), FunctionName.ToString());
        }

        UClass* MemberParent = CallNode->FunctionReference.GetMemberParentClass();
        if (MemberParent)
        {
            NodeObj->SetStringField(TEXT("FunctionOwner"), MemberParent->GetName());
        }

        // bIsPureFunc / bIsConstFunc / bIsInterfaceCall deprecated in 5.5, same state resolves off the node and the owner
        if (CallNode->IsNodePure())
        {
            NodeObj->SetBoolField(TEXT("Pure"), true);
        }

        const UFunction* TargetFunction = CallNode->GetTargetFunction();
        if (TargetFunction && TargetFunction->HasAnyFunctionFlags(FUNC_Const))
        {
            NodeObj->SetBoolField(TEXT("Const"), true);
        }

        if (MemberParent && MemberParent->HasAnyClassFlags(CLASS_Interface))
        {
            NodeObj->SetBoolField(TEXT("Interface"), true);
        }

        if (CallNode->FunctionReference.IsSelfContext())
        {
            NodeObj->SetBoolField(TEXT("SelfContext"), true);
        }

        TSharedPtr<FJsonObject> Args = ExportCallArgs(Node);
        if (Args.IsValid())
        {
            NodeObj->SetObjectField(TEXT("Args"), Args);
        }
    }

    if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
    {
        if (CastNode->TargetType)
        {
            NodeObj->SetStringField(TEXT("CastTarget"), CastNode->TargetType->GetPathName());
        }

        if (CastNode->IsNodePure())
        {
            NodeObj->SetBoolField(TEXT("PureCast"), true);
        }
    }

    if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
    {
        FName EventName = EventNode->EventReference.GetMemberName();
        if (!EventName.IsNone())
        {
            NodeObj->SetStringField(TEXT("EventName"), EventName.ToString());
        }
    }

    if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
    {
        // EventReference stays empty on a custom event, CustomFunctionName is the only name it carries
        if (!CustomEventNode->CustomFunctionName.IsNone())
        {
            NodeObj->SetStringField(TEXT("EventName"), CustomEventNode->CustomFunctionName.ToString());
        }

        if (CustomEventNode->bCallInEditor)
        {
            NodeObj->SetBoolField(TEXT("CallInEditor"), true);
        }

        AddEventNetFlags(CustomEventNode->FunctionFlags, NodeObj);
    }

    if (const UK2Node_ComponentBoundEvent* ComponentEventNode = Cast<UK2Node_ComponentBoundEvent>(Node))
    {
        if (!ComponentEventNode->ComponentPropertyName.IsNone())
        {
            NodeObj->SetStringField(TEXT("ComponentPropertyName"), ComponentEventNode->ComponentPropertyName.ToString());
        }

        if (!ComponentEventNode->DelegatePropertyName.IsNone())
        {
            NodeObj->SetStringField(TEXT("DelegatePropertyName"), ComponentEventNode->DelegatePropertyName.ToString());
        }
    }

    if (const UK2Node_ActorBoundEvent* ActorEventNode = Cast<UK2Node_ActorBoundEvent>(Node))
    {
        if (!ActorEventNode->DelegatePropertyName.IsNone())
        {
            NodeObj->SetStringField(TEXT("DelegatePropertyName"), ActorEventNode->DelegatePropertyName.ToString());
        }

        if (ActorEventNode->EventOwner)
        {
            NodeObj->SetStringField(TEXT("EventOwner"), ActorEventNode->EventOwner->GetPathName());
        }
    }

    if (const UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
    {
        if (!TimelineNode->TimelineName.IsNone())
        {
            NodeObj->SetStringField(TEXT("TimelineName"), TimelineNode->TimelineName.ToString());
        }
    }

    if (const UK2Node_CreateDelegate* CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node))
    {
        FName SelectedFunctionName = CreateDelegateNode->GetFunctionName();
        if (!SelectedFunctionName.IsNone())
        {
            NodeObj->SetStringField(TEXT("SelectedFunctionName"), SelectedFunctionName.ToString());
        }
    }

    if (const UK2Node_SpawnActorFromClass* SpawnNode = Cast<UK2Node_SpawnActorFromClass>(Node))
    {
        const UEdGraphPin* ClassPin = SpawnNode->GetClassPin();
        if (ClassPin && ClassPin->DefaultObject)
        {
            NodeObj->SetStringField(TEXT("SpawnClass"), ClassPin->DefaultObject->GetPathName());
        }
    }

    if (const UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Node))
    {
        // IndexPinType is private and GetIndexPin asserts, the live pin carries the same type
        if (const UEdGraphPin* IndexPin = SelectNode->FindPin(TEXT("Index"), EGPD_Input))
        {
            NodeObj->SetObjectField(TEXT("IndexPinType"), MakePinTypeObject(IndexPin->PinType));
        }
    }

    if (const UK2Node_SwitchEnum* SwitchEnumNode = Cast<UK2Node_SwitchEnum>(Node))
    {
        NodeObj->SetStringField(TEXT("SwitchType"), TEXT("Enum"));

        if (SwitchEnumNode->Enum)
        {
            NodeObj->SetStringField(TEXT("Enum"), SwitchEnumNode->Enum->GetPathName());
        }
    }
    else if (Node->IsA(UK2Node_SwitchInteger::StaticClass()))
    {
        NodeObj->SetStringField(TEXT("SwitchType"), TEXT("Integer"));
    }
    else if (Node->IsA(UK2Node_SwitchString::StaticClass()))
    {
        NodeObj->SetStringField(TEXT("SwitchType"), TEXT("String"));
    }
    else if (Node->IsA(UK2Node_SwitchName::StaticClass()))
    {
        NodeObj->SetStringField(TEXT("SwitchType"), TEXT("Name"));
    }

    if (const UK2Node_InputAction* InputActionNode = Cast<UK2Node_InputAction>(Node))
    {
        if (!InputActionNode->InputActionName.IsNone())
        {
            NodeObj->SetStringField(TEXT("InputAction"), InputActionNode->InputActionName.ToString());
        }
    }

    if (const UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(Node))
    {
        if (!InputKeyNode->InputKey.GetFName().IsNone())
        {
            NodeObj->SetStringField(TEXT("InputKey"), InputKeyNode->InputKey.GetFName().ToString());
        }
    }

    if (Node->IsA(UK2Node_Knot::StaticClass()))
    {
        NodeObj->SetBoolField(TEXT("Reroute"), true);
    }
}

void EdGraphJson::AddAnimNodeFields(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj)
{
    if (const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
    {
        if (const UAnimationAsset* Asset = AnimNode->GetAnimationAsset())
        {
            NodeObj->SetStringField(TEXT("AnimationAsset"), Asset->GetPathName());
        }

        TSharedPtr<FJsonObject> Settings = ExportAnimNodeSettings(AnimNode);
        if (Settings.IsValid())
        {
            NodeObj->SetObjectField(TEXT("Settings"), Settings);
        }

        AddPropertyBindings(AnimNode, NodeObj);
        AddExposedPins(AnimNode, NodeObj);
    }

    AddPropertyAccessPath(Node, NodeObj);
}

bool EdGraphJson::GetPropertyBinding(const UEdGraphNode* Node, const FName PropertyName, FString& OutPath, FString& OutType)
{
    const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
    const UObject* BindingObject = nullptr;
    const FMapProperty* MapProp = nullptr;
    if (!AnimNode || !OpenBindingMap(AnimNode, BindingObject, MapProp))
    {
        return false;
    }

    FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(BindingObject));

    for (FScriptMapHelper::FIterator It(MapHelper); It; ++It)
    {
        const FName* Key = reinterpret_cast<const FName*>(MapHelper.GetKeyPtr(It));
        const FAnimGraphNodePropertyBinding* Binding = reinterpret_cast<const FAnimGraphNodePropertyBinding*>(MapHelper.GetValuePtr(It));
        if (!Key || !Binding || *Key != PropertyName || !Binding->bIsBound)
        {
            continue;
        }

        OutPath = FString::Join(Binding->PropertyPath, TEXT("."));
        OutType = PropertyBindingTypeName(Binding->Type);
        return true;
    }

    return false;
}

FEdGraphJsonSerializer::FEdGraphJsonSerializer(const FEdGraphJsonOptions& InOptions)
    : m_Options(InOptions)
{
}

void FEdGraphJsonSerializer::ExcludeGraph(const UEdGraph* Graph)
{
    if (Graph)
    {
        m_VisitedGraphs.Add(Graph);
    }
}

TSharedPtr<FJsonObject> FEdGraphJsonSerializer::ExportGraph(const UEdGraph* Graph, const TCHAR* GraphType)
{
    return ExportGraphAtDepth(Graph, GraphType, 0);
}

// Marks visited without testing it. A graph the caller names by hand always comes back, shared
// transition rules included, while recursion below still stops when it climbs back up to here.
TSharedPtr<FJsonObject> FEdGraphJsonSerializer::ExportGraphAtDepth(const UEdGraph* Graph, const TCHAR* GraphType, int32 SubGraphDepth)
{
    if (!Graph)
    {
        return nullptr;
    }

    m_VisitedGraphs.Add(Graph);

    TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
    GraphObj->SetStringField(TEXT("Name"), Graph->GetName());
    GraphObj->SetStringField(TEXT("GraphType"), GraphType);
    GraphObj->SetNumberField(TEXT("NodeCount"), Graph->Nodes.Num());

    // Separates a stub graph from one carrying real logic, so lean output filters candidates without -graphs.
    bool bHasLogic = false;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->LinkedTo.Num() > 0)
            {
                bHasLogic = true;
                break;
            }
        }
        if (bHasLogic)
        {
            break;
        }
    }
    GraphObj->SetBoolField(TEXT("HasLogic"), bHasLogic);

    if (!m_Options.bWithNodes)
    {
        return GraphObj;
    }

    TArray<TSharedPtr<FJsonValue>> NodesArray;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        TSharedPtr<FJsonObject> NodeObj = ExportNode(Node, SubGraphDepth);
        if (NodeObj.IsValid())
        {
            NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
        }
    }
    GraphObj->SetArrayField(TEXT("Nodes"), NodesArray);

    return GraphObj;
}

TSharedPtr<FJsonObject> FEdGraphJsonSerializer::ExportNode(const UEdGraphNode* Node, int32 SubGraphDepth)
{
    if (!Node)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

    NodeObj->SetStringField(TEXT("NodeId"), Node->NodeGuid.ToString());
    NodeObj->SetStringField(TEXT("Class"), Node->GetClass()->GetName());
    NodeObj->SetStringField(TEXT("Title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    NodeObj->SetNumberField(TEXT("PosX"), Node->NodePosX);
    NodeObj->SetNumberField(TEXT("PosY"), Node->NodePosY);

    if (!Node->NodeComment.IsEmpty())
    {
        NodeObj->SetStringField(TEXT("Comment"), Node->NodeComment);
    }

    const ENodeEnabledState EnabledState = Node->GetDesiredEnabledState();
    if (EnabledState != ENodeEnabledState::Enabled)
    {
        NodeObj->SetBoolField(TEXT("Enabled"), false);
        NodeObj->SetStringField(TEXT("EnabledState"), LexToString(EnabledState));
    }

    EdGraphJson::AddK2NodeFields(Node, NodeObj);
    EdGraphJson::AddAnimNodeFields(Node, NodeObj);

    TArray<TSharedPtr<FJsonValue>> PinsArray;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->bHidden && !m_Options.bIncludeHiddenPins)
        {
            continue;
        }

        TSharedPtr<FJsonObject> PinObj = ExportPin(Pin);
        if (PinObj.IsValid())
        {
            PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
        }
    }
    NodeObj->SetArrayField(TEXT("Pins"), PinsArray);

    if (m_Options.bRecurseSubGraphs && m_Options.bWithNodes)
    {
        AddSubGraphs(Node, NodeObj, SubGraphDepth);
    }

    return NodeObj;
}

void FEdGraphJsonSerializer::AddSubGraphs(const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj, int32 SubGraphDepth)
{
    if (SubGraphDepth >= m_Options.MaxSubGraphDepth)
    {
        return;
    }

    TArray<UEdGraph*> SubGraphs = Node->GetSubGraphs();
    if (SubGraphs.IsEmpty())
    {
        return;
    }

    const TCHAR* SubGraphType = GraphTypeForOwningNode(Node);

    TArray<TSharedPtr<FJsonValue>> SubGraphsArray;
    for (const UEdGraph* SubGraph : SubGraphs)
    {
        if (!SubGraph || m_VisitedGraphs.Contains(SubGraph))
        {
            continue;
        }

        TSharedPtr<FJsonObject> SubGraphObj = ExportGraphAtDepth(SubGraph, SubGraphType, SubGraphDepth + 1);
        if (SubGraphObj.IsValid())
        {
            SubGraphsArray.Add(MakeShared<FJsonValueObject>(SubGraphObj));
        }
    }

    if (SubGraphsArray.Num() > 0)
    {
        NodeObj->SetArrayField(TEXT("SubGraphs"), SubGraphsArray);
    }
}

TSharedPtr<FJsonObject> FEdGraphJsonSerializer::ExportPin(const UEdGraphPin* Pin) const
{
    if (!Pin)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();

    PinObj->SetStringField(TEXT("Name"), Pin->PinName.ToString());
    PinObj->SetStringField(TEXT("Direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
    PinObj->SetStringField(TEXT("Type"), Pin->PinType.PinCategory.ToString());

    if (Pin->PinType.PinSubCategoryObject.IsValid())
    {
        PinObj->SetStringField(TEXT("SubType"), Pin->PinType.PinSubCategoryObject->GetName());
    }

    if (!Pin->PinType.PinSubCategory.IsNone())
    {
        PinObj->SetStringField(TEXT("SubCategory"), Pin->PinType.PinSubCategory.ToString());
    }

    if (Pin->PinType.ContainerType != EPinContainerType::None)
    {
        PinObj->SetStringField(TEXT("Container"), PinContainerTypeName(Pin->PinType.ContainerType));

        if (Pin->PinType.ContainerType == EPinContainerType::Map)
        {
            PinObj->SetObjectField(TEXT("ValueType"), MakeTerminalTypeObject(Pin->PinType.PinValueType));
        }
    }

    if (!Pin->DefaultValue.IsEmpty())
    {
        PinObj->SetStringField(TEXT("Default"), Pin->DefaultValue);
    }

    if (!Pin->DefaultTextValue.IsEmpty())
    {
        PinObj->SetStringField(TEXT("DefaultText"), Pin->DefaultTextValue.ToString());
    }

    if (Pin->DefaultObject)
    {
        PinObj->SetStringField(TEXT("DefaultObject"), Pin->DefaultObject->GetPathName());
    }

    // Present only where it disagrees with Default, that gap is what marks a user-edited value
    if (!Pin->AutogeneratedDefaultValue.IsEmpty() && Pin->AutogeneratedDefaultValue != Pin->DefaultValue)
    {
        PinObj->SetStringField(TEXT("AutogeneratedDefault"), Pin->AutogeneratedDefaultValue);
    }

    if (Pin->bAdvancedView)
    {
        PinObj->SetBoolField(TEXT("Advanced"), true);
    }

    if (Pin->PinType.bIsReference)
    {
        PinObj->SetBoolField(TEXT("Reference"), true);
    }

    if (Pin->bOrphanedPin)
    {
        PinObj->SetBoolField(TEXT("Orphaned"), true);
    }

    if (Pin->LinkedTo.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> LinksArray;
        for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (LinkedPin && LinkedPin->GetOwningNode())
            {
                TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
                LinkObj->SetStringField(TEXT("NodeId"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
                LinkObj->SetStringField(TEXT("NodeTitle"), LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                LinkObj->SetStringField(TEXT("PinName"), LinkedPin->PinName.ToString());
                LinksArray.Add(MakeShared<FJsonValueObject>(LinkObj));
            }
        }
        PinObj->SetArrayField(TEXT("LinkedTo"), LinksArray);
    }

    return PinObj;
}
