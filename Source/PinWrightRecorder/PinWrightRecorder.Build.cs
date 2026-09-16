// Copyright (c) 2026 Alexander Penkin. MIT License.

using UnrealBuildTool;

public class PinWrightRecorder : ModuleRules
{
    public PinWrightRecorder(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = true;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "Json"
        });
    }
}
