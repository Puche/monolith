#include "MonolithInsightsModule.h"
#include "MonolithInsightsActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"

DEFINE_LOG_CATEGORY(LogMonolithInsights);

#define LOCTEXT_NAMESPACE "FMonolithInsightsModule"

void FMonolithInsightsModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableInsights) return;

	FMonolithInsightsActions::RegisterActions(FMonolithToolRegistry::Get());
	UE_LOG(LogMonolithInsights, Log, TEXT("Monolith — Insights module loaded (4 actions)"));
}

void FMonolithInsightsModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("insights"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithInsightsModule, MonolithInsights)
