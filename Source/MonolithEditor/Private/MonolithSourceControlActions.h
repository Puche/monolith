#pragma once

#include "MonolithToolRegistry.h"

class FJsonObject;

// Headless (no-UI) Source Control integration. Wraps ISourceControlModule directly so
// Monolith-driven writes never trigger the engine's synchronous "Check Out Files?" modal —
// that modal's nested Slate loop blocks the game thread, which starves the in-process MCP
// HTTP server (see MonolithEditorModule's PART C modal watcher, which only detects this
// class of hang after the fact). Registered under the existing "editor" namespace rather
// than a new namespace so it surfaces via the already-wired editor_query tool without
// requiring changes to the Python stdio proxy's seed tool list.
class FMonolithSourceControlActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleSourceControlStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSourceControlCheckout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSourceControlRevert(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Headless checkout of a single package's on-disk file, called directly (no HTTP
	 * round-trip) from save_packages' auto_checkout path. No-ops cleanly — sets only
	 * source_controlled=false on OutFields — when source control isn't enabled, the
	 * package isn't tracked, or it has never been saved to disk. Never raises UI.
	 * Merges {source_controlled, checked_out, checked_out_by_other?, checkout_error?}
	 * into OutFields for the caller's own per-package response row.
	 */
	static void CheckoutPackageForWrite(const FString& PackageName, const TSharedPtr<FJsonObject>& OutFields);
};
