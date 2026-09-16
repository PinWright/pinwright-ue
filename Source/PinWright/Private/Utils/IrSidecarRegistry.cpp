// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/IrSidecarRegistry.h"

namespace IrSidecarRegistry
{
    namespace
    {
        TArray<FIrSidecarSpec>& MutableSpecs()
        {
            static TArray<FIrSidecarSpec> Specs;
            return Specs;
        }

        bool& SpecsNeedSort()
        {
            static bool bNeedSort = false;
            return bNeedSort;
        }

        int32 CompareText(const TCHAR* A, const TCHAR* B)
        {
            return FCString::Strcmp(A ? A : TEXT(""), B ? B : TEXT(""));
        }
    }

    FAutoRegisterIrSidecar::FAutoRegisterIrSidecar(FIrSidecarSpec InSpec)
    {
        MutableSpecs().Add(InSpec);
        SpecsNeedSort() = true;
    }

    TArray<FIrSidecarSpec> GetRegisteredIrSidecars()
    {
        TArray<FIrSidecarSpec>& Specs = MutableSpecs();
        if (SpecsNeedSort())
        {
            Specs.Sort([](const FIrSidecarSpec& A, const FIrSidecarSpec& B)
            {
                if (A.Priority != B.Priority)
                {
                    return A.Priority < B.Priority;
                }

                const int32 FileNameOrder = CompareText(A.FileName, B.FileName);
                if (FileNameOrder != 0)
                {
                    return FileNameOrder < 0;
                }

                return CompareText(A.Name, B.Name) < 0;
            });
            SpecsNeedSort() = false;
        }
        return Specs;
    }
}
