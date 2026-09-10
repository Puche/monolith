using UnrealBuildTool;

public class MonolithInsights : ModuleRules
{
	public MonolithInsights(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"UnrealEd",
			"Json",
			"JsonUtilities",
			// In-process .utrace analysis — bypasses UnrealInsights.exe entirely.
			// See Docs (or memory `project_insights_headless_export_broken`) for why:
			// UnrealInsights.exe's `-ExecOnAnalysisCompleteCmd` headless CSV export
			// never fires on our toolchain (repro'd on UE 5.6 and 5.8), matching
			// unresolved community reports. `TraceServices::IAnalysisService::Analyze()`
			// gives a synchronous, in-process `IAnalysisSession` instead — same data,
			// no external process, no broken callback.
			"TraceAnalysis",
			"TraceServices",
			"TraceLog"
		});
	}
}
