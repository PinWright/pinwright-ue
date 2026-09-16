// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "IrCore/IrTypeSpec.h"

#include "Templates/UniquePtr.h"

static TUniquePtr<FIrTypeSpec> CloneChild(const TUniquePtr<FIrTypeSpec>& Src)
{
    if (!Src.IsValid())
    {
        return TUniquePtr<FIrTypeSpec>();
    }
    return MakeUnique<FIrTypeSpec>(*Src);
}

FIrTypeSpec::FIrTypeSpec(const FIrTypeSpec& Other)
    : Kind(Other.Kind)
    , InnerName(Other.InnerName)
    , SecondaryInnerName(Other.SecondaryInnerName)
    , Container(Other.Container)
    , ElementSpec(CloneChild(Other.ElementSpec))
    , KeySpec(CloneChild(Other.KeySpec))
    , bIsConst(Other.bIsConst)
    , bIsReference(Other.bIsReference)
{
}

FIrTypeSpec& FIrTypeSpec::operator=(const FIrTypeSpec& Other)
{
    if (this == &Other)
    {
        return *this;
    }

    Kind = Other.Kind;
    InnerName = Other.InnerName;
    SecondaryInnerName = Other.SecondaryInnerName;
    Container = Other.Container;
    ElementSpec = CloneChild(Other.ElementSpec);
    KeySpec = CloneChild(Other.KeySpec);
    bIsConst = Other.bIsConst;
    bIsReference = Other.bIsReference;
    return *this;
}

bool FIrTypeSpec::IsVoid() const
{
    return Kind == EIrTypeKind::Void
        && Container == EPinContainerType::None
        && !bIsConst
        && !bIsReference;
}

bool FIrTypeSpec::IsEmpty() const
{
    return Kind == EIrTypeKind::Unresolved
        && InnerName.IsNone()
        && SecondaryInnerName.IsNone()
        && Container == EPinContainerType::None
        && !ElementSpec.IsValid()
        && !KeySpec.IsValid()
        && !bIsConst
        && !bIsReference;
}

bool FIrTypeSpec::Equals(const FIrTypeSpec& Other) const
{
    if (Kind != Other.Kind) return false;
    if (InnerName != Other.InnerName) return false;
    if (SecondaryInnerName != Other.SecondaryInnerName) return false;
    if (Container != Other.Container) return false;
    if (bIsConst != Other.bIsConst) return false;
    if (bIsReference != Other.bIsReference) return false;

    const bool bHasElem = ElementSpec.IsValid();
    const bool bOtherHasElem = Other.ElementSpec.IsValid();
    if (bHasElem != bOtherHasElem) return false;
    if (bHasElem && !ElementSpec->Equals(*Other.ElementSpec)) return false;

    const bool bHasKey = KeySpec.IsValid();
    const bool bOtherHasKey = Other.KeySpec.IsValid();
    if (bHasKey != bOtherHasKey) return false;
    if (bHasKey && !KeySpec->Equals(*Other.KeySpec)) return false;

    return true;
}
