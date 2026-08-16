// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

using UnrealBuildTool;

/**
 * Single runtime module of the sample. Kept deliberately dependency-light: the navigation
 * core only needs Core/CoreUObject/Engine, so it stays cheap to port into another project.
 */
public class NavalNav : ModuleRules
{
	public NavalNav(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicIncludePaths.Add(ModuleDirectory);
		
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
