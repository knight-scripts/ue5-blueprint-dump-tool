// Copyright (c) 2026 knight-scripts. MIT License.

using UnrealBuildTool;

public class BlueprintDumpTool : ModuleRules
{
	public BlueprintDumpTool(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"UnrealEd",
				"BlueprintGraph",
				"AnimGraph",
				"Kismet",
			}
		);
	}
}
