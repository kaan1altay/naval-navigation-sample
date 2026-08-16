// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.

using UnrealBuildTool;

/** Editor target. This is the target used for day-to-day iteration and automation tests. */
public class NavalNavSampleEditorTarget : TargetRules
{
	public NavalNavSampleEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;

		ExtraModuleNames.Add("NavalNav");
	}
}
