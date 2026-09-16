// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetClassInspectHandler.cpp
// Returns all editable properties for UMG widget classes, with struct field recursion.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Utils/PropertyInspection.h"

#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"


using namespace WidgetAuthoringHelpers;

namespace
{
    void CollectProperties(UStruct* Struct, const FString& Prefix, int32 Depth, int32 MaxDepth, TArray<TSharedPtr<FJsonValue>>& OutProps)
    {
        for (TFieldIterator<FProperty> PropIt(Struct, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

            FString PropName = Prefix.IsEmpty() ? Prop->GetName() : Prefix + TEXT(".") + Prop->GetName();
            FString PropType = GetPropertyCppTypeWithParams(Prop);

            TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
            PropObj->SetStringField(TEXT("name"), PropName);
            PropObj->SetStringField(TEXT("type"), PropType);
            OutProps.Add(MakeShared<FJsonValueObject>(PropObj));

            if (Depth < MaxDepth)
            {
                if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
                {
                    if (StructProp->Struct)
                    {
                        CollectProperties(StructProp->Struct, PropName, Depth + 1, MaxDepth, OutProps);
                    }
                }
            }
        }
    }
}


// ---- widget.get_class_properties ----
REGISTER_RPC_HANDLER("widget.get_class_properties", "widget",
    "Return all editable properties for one or more UMG widget classes, with struct field recursion",
    RPC_PARAMS(
        RPC_PARAM_REQ("classes", "array",  "UMG class names, e.g. [\"Button\", \"TextBlock\"]"),
        RPC_PARAM_OPT("maxDepth", "number", "Struct nesting depth for recursive enumeration (default 2)")
    ))
{
    const TArray<TSharedPtr<FJsonValue>>* ClassesPtr = Ctx.GetArray(TEXT("classes"));
    if (!ClassesPtr || ClassesPtr->IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Required parameter 'classes' is missing or empty"));
        return true;
    }

    int32 MaxDepth = Ctx.GetInt(TEXT("maxDepth"), 2);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    for (const TSharedPtr<FJsonValue>& ClassVal : *ClassesPtr)
    {
        FString ClassName;
        if (!ClassVal.IsValid() || !ClassVal->TryGetString(ClassName) || ClassName.IsEmpty())
            continue;

        UClass* Class = ResolveWidgetClass(ClassName);
        if (!Class)
        {
            // Return an empty array so callers know the class was not found
            Result->SetArrayField(ClassName, TArray<TSharedPtr<FJsonValue>>());
            continue;
        }

        TArray<TSharedPtr<FJsonValue>> Props;
        CollectProperties(Class, FString(), 0, MaxDepth, Props);
        Result->SetArrayField(ClassName, Props);
    }

    Ctx.SendSuccess(Result);
    return true;
}
