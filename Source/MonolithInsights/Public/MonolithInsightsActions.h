#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "ProfilingDebugging/MiscTrace.h" // ETraceFrameType
#include "HAL/CriticalSection.h"

namespace TraceServices { class IAnalysisSession; }

/**
 * Insights domain action handlers for Monolith.
 *
 * Reads .utrace files IN-PROCESS via TraceServices, rather than shelling out
 * to UnrealInsights.exe. The documented headless CLI export
 * (`-ExecOnAnalysisCompleteCmd`) does not fire on our toolchain — confirmed
 * broken on both UE 5.6 and UE 5.8 locally (see memory
 * `project_insights_headless_export_broken`) and matches unresolved reports
 * on Epic's community forum. This module works around that entirely by never
 * invoking the external binary.
 *
 * Analysis is ASYNCHRONOUS (`IAnalysisService::StartAnalysis`, not the
 * blocking `Analyze`) and sessions are cached by resolved trace path — a
 * 4.5GB/562M-scope real PS5 trace took ~50s to fully parse and blocked the
 * editor's game thread for that whole time under the original blocking
 * design, long enough that the MCP client gave up and the HTTP response
 * failed to send (`socket_send_failure`) even though the editor finished the
 * work fine. See memory `project_insights_large_trace_async_analysis`.
 */
class FMonolithInsightsActions
{
public:
	/** Register all insights actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Action handlers ---

	/** List .utrace files in the local trace store (or an override directory) */
	static FMonolithActionResult ListTraces(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Smoke-test / entry point for the TraceServices pipeline: opens a trace
	 * synchronously and reports duration + thread list. Validates that
	 * `Analyze()` -> `IAnalysisSession` -> provider reads work end-to-end
	 * before building frame-window export on top of it.
	 */
	static FMonolithActionResult SessionInfo(const TSharedPtr<FJsonObject>& Params);

	/**
	 * The actual "give me frame N" feature: resolves a frame index to its
	 * [StartTime, EndTime) window via `TraceServices::IFrameProvider` (native
	 * frame API — no need to infer frame boundaries from a root timer event),
	 * then walks every CPU thread's timing-profiler timeline within that
	 * window and returns the call-tree events as JSON (thread/timer/depth,
	 * equivalent to Insights' `TimingInsights.ExportTimingEvents` but scoped
	 * to one exact frame instead of a manual second range).
	 */
	static FMonolithActionResult GetFrame(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Performance-analysis entry point: scans every frame in the trace and returns the N
	 * slowest by wall-clock duration (native `IFrameProvider::EnumerateFrames`), plus the
	 * average frame time. Feed a returned `frame_index` into `get_frame` to see exactly
	 * what ran during that hitch.
	 */
	static FMonolithActionResult FindHitches(const TSharedPtr<FJsonObject>& Params);

private:
	/** Local UnrealTrace store dir: %LOCALAPPDATA%/UnrealEngine/Common/UnrealTrace/Store/001 */
	static FString GetDefaultTraceStoreDir();

	/** Resolve a `trace` param to a full disk path: as-is if it looks like a path, else joined to the trace store dir */
	static FString ResolveTracePath(const FString& Trace);

	/** Parse "Game" (default) or "Rendering" into ETraceFrameType; unrecognized values fall back to Game */
	static ETraceFrameType ParseFrameType(const FString& FrameTypeName);

	/**
	 * Returns a session for TracePath — from the cache if already opened, else kicks off
	 * `StartAnalysis()` (non-blocking; parsing continues on TraceServices' own worker
	 * threads) and caches it. Does NOT wait for completion — callers must check
	 * `Session->IsAnalysisComplete()` (or use `WaitForAnalysis` below) before assuming the
	 * data they read back is final. Never evicted in this version: repeat queries against a
	 * huge trace are cheap, but keeping many huge trace sessions open simultaneously would
	 * grow editor memory unbounded — acceptable tradeoff for now, revisit if it bites.
	 */
	static TSharedPtr<const TraceServices::IAnalysisSession> GetOrStartAnalysis(const FString& TracePath);

	/**
	 * Polls `Session.IsAnalysisComplete()` (under a proper read-scope — TraceServices is
	 * explicitly designed to be read safely while analysis is still in progress, same as
	 * Insights' own live-session view) until it's true or MaxWaitSeconds elapses. Returns
	 * whether it completed within the bound. Callers still take their own read-scope
	 * afterward for the actual data read.
	 */
	static bool WaitForAnalysis(const TraceServices::IAnalysisSession& Session, double MaxWaitSeconds);

	static TMap<FString, TSharedPtr<const TraceServices::IAnalysisSession>>& GetSessionCache();
	static FCriticalSection& GetCacheLock();
};
