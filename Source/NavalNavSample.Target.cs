// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

using UnrealBuildTool;

/** Standalone game target (cooked builds, no editor). */
public class NavalNavSampleTarget : TargetRules
{
	public NavalNavSampleTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;

		ExtraModuleNames.Add("NavalNav");
	}
}
