#include "MonolithInsightsActions.h"
#include "MonolithInsightsModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"

#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/TimingProfiler.h"

// ============================================================================
// Registration
// ============================================================================

void FMonolithInsightsActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("insights"), TEXT("list_traces"),
		TEXT("List .utrace files in the local trace store (or an override directory), newest first"),
		FMonolithActionHandler::CreateStatic(&FMonolithInsightsActions::ListTraces),
		FParamSchemaBuilder()
			.OptionalDiskPath(TEXT("directory"),
				TEXT("Override directory to scan (default: local UnrealTrace store, %LOCALAPPDATA%/UnrealEngine/Common/UnrealTrace/Store/001)"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Max traces to return, newest first"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("insights"), TEXT("session_info"),
		TEXT("Open a .utrace file in-process (TraceServices, NOT UnrealInsights.exe) and report duration + thread list. "
			 "Analysis runs asynchronously and the session is cached by trace path — call again on the same trace "
			 "to poll progress on a huge file (analysis_complete=false means keep polling)."),
		FMonolithActionHandler::CreateStatic(&FMonolithInsightsActions::SessionInfo),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("trace"),
				TEXT("Path to a .utrace file, or a bare filename resolved against the local trace store"))
			.Optional(TEXT("max_threads"), TEXT("number"), TEXT("Cap on returned thread entries (0 = no cap)"), TEXT("0"))
			.Optional(TEXT("max_wait_seconds"), TEXT("number"), TEXT("How long to wait for analysis to finish before returning partial results (huge traces: poll again after)"), TEXT("20"))
			.Build());

	Registry.RegisterAction(TEXT("insights"), TEXT("get_frame"),
		TEXT("Export the full call-tree (all CPU threads, timer/depth/start/end) for exactly one frame, resolved "
			 "in-process via TraceServices' native IFrameProvider — no manual second-range guessing. "
			 "Equivalent to Insights' TimingInsights.ExportTimingEvents but windowed to a single frame index. "
			 "Analysis is cached by trace path — safe to call repeatedly against the same trace."),
		FMonolithActionHandler::CreateStatic(&FMonolithInsightsActions::GetFrame),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("trace"),
				TEXT("Path to a .utrace file, or a bare filename resolved against the local trace store"))
			.Required(TEXT("frame_index"), TEXT("number"), TEXT("0-based frame index (use find_hitches or session_info's duration + an approximate frame rate to pick one — the response reports frame_count for the trace)"))
			.Optional(TEXT("frame_type"), TEXT("string"), TEXT("'Game' or 'Rendering'"), TEXT("Game"))
			.Optional(TEXT("threads"), TEXT("string"), TEXT("Comma-separated substring filter on thread name (e.g. 'GameThread,RenderThread') — empty = all threads"))
			.Optional(TEXT("max_events"), TEXT("number"), TEXT("Cap on returned events across all threads (0 = no cap)"), TEXT("2000"))
			.Optional(TEXT("min_duration_ms"), TEXT("number"), TEXT("Only return events at least this long, in milliseconds — filters the noise floor of tiny nested calls out so max_events is spent on what actually matters (0 = no filter)"), TEXT("0"))
			.Optional(TEXT("max_wait_seconds"), TEXT("number"), TEXT("How long to wait for analysis to reach this frame before giving up (huge traces: poll again after)"), TEXT("20"))
			.Build());

	Registry.RegisterAction(TEXT("insights"), TEXT("find_hitches"),
		TEXT("Scan every frame in the trace and return the N slowest by wall-clock duration, plus the average "
			 "frame time — the actual 'what's slow in this trace' entry point. Feed a returned frame_index into "
			 "get_frame to see exactly what ran during that hitch."),
		FMonolithActionHandler::CreateStatic(&FMonolithInsightsActions::FindHitches),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("trace"),
				TEXT("Path to a .utrace file, or a bare filename resolved against the local trace store"))
			.Optional(TEXT("frame_type"), TEXT("string"), TEXT("'Game' or 'Rendering'"), TEXT("Game"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("How many of the slowest frames to return"), TEXT("20"))
			.Optional(TEXT("min_ms"), TEXT("number"), TEXT("Only consider frames at least this many milliseconds long (0 = no filter)"), TEXT("0"))
			.Optional(TEXT("max_wait_seconds"), TEXT("number"), TEXT("How long to wait for analysis to finish before returning partial results"), TEXT("20"))
			.Build());
}

// ============================================================================
// Helpers
// ============================================================================

FString FMonolithInsightsActions::GetDefaultTraceStoreDir()
{
	// Matches FWindowsPlatformProcess::UserSettingsDir() == %LOCALAPPDATA% on Windows
	// (FOLDERID_LocalAppData) — same directory UnrealInsights.exe itself defaults to.
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("UnrealEngine/Common/UnrealTrace/Store/001"));
}

ETraceFrameType FMonolithInsightsActions::ParseFrameType(const FString& FrameTypeName)
{
	if (FrameTypeName.Equals(TEXT("Rendering"), ESearchCase::IgnoreCase))
	{
		return ETraceFrameType::TraceFrameType_Rendering;
	}
	return ETraceFrameType::TraceFrameType_Game;
}

TMap<FString, TSharedPtr<const TraceServices::IAnalysisSession>>& FMonolithInsightsActions::GetSessionCache()
{
	static TMap<FString, TSharedPtr<const TraceServices::IAnalysisSession>> Cache;
	return Cache;
}

FCriticalSection& FMonolithInsightsActions::GetCacheLock()
{
	static FCriticalSection Lock;
	return Lock;
}

TSharedPtr<const TraceServices::IAnalysisSession> FMonolithInsightsActions::GetOrStartAnalysis(const FString& TracePath)
{
	FScopeLock Lock(&GetCacheLock());

	if (TSharedPtr<const TraceServices::IAnalysisSession>* Existing = GetSessionCache().Find(TracePath))
	{
		return *Existing;
	}

	ITraceServicesModule& TraceServicesModule = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
	TSharedPtr<TraceServices::IAnalysisService> AnalysisService = TraceServicesModule.GetAnalysisService();
	if (!AnalysisService.IsValid())
	{
		return nullptr;
	}

	// Non-blocking: parsing continues on TraceServices' own worker threads. A 4.5GB/562M-scope
	// real PS5 trace took ~50s to fully analyze — long enough that the old blocking `Analyze()`
	// call held the game thread hostage for that whole time and the MCP client gave up before
	// the HTTP response could even be sent (see memory project_insights_large_trace_async_analysis).
	TSharedPtr<const TraceServices::IAnalysisSession> Session = AnalysisService->StartAnalysis(*TracePath);
	if (Session.IsValid())
	{
		GetSessionCache().Add(TracePath, Session);
	}
	return Session;
}

bool FMonolithInsightsActions::WaitForAnalysis(const TraceServices::IAnalysisSession& Session, double MaxWaitSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + FMath::Max(MaxWaitSeconds, 0.0);
	for (;;)
	{
		bool bComplete = false;
		{
			// TraceServices is explicitly designed to be read safely while analysis is still
			// in progress (it's how Insights' own live-session view works) — this short
			// read-scope per poll is cheap and safe.
			TraceServices::FAnalysisSessionReadScope ReadScope(Session);
			bComplete = Session.IsAnalysisComplete();
		}
		if (bComplete)
		{
			return true;
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.25f);
	}
}

FString FMonolithInsightsActions::ResolveTracePath(const FString& Trace)
{
	if (Trace.Contains(TEXT("/")) || Trace.Contains(TEXT("\\")))
	{
		// Already a path (relative or absolute) — use as-is.
		return Trace;
	}

	// Bare filename — resolve against the trace store, adding .utrace if missing.
	FString FileName = Trace.EndsWith(TEXT(".utrace")) ? Trace : Trace + TEXT(".utrace");
	return FPaths::Combine(GetDefaultTraceStoreDir(), FileName);
}

// ============================================================================
// Action: list_traces
// Params: { "directory"?: "...", "limit"?: number }
// ============================================================================

FMonolithActionResult FMonolithInsightsActions::ListTraces(const TSharedPtr<FJsonObject>& Params)
{
	FString Directory = Params->HasField(TEXT("directory")) ? Params->GetStringField(TEXT("directory")) : FString();
	if (Directory.IsEmpty())
	{
		Directory = GetDefaultTraceStoreDir();
	}

	int32 Limit = 50;
	Params->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Max(Limit, 1);

	if (!FPaths::DirectoryExists(Directory))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("list_traces: directory not found: '%s'"), *Directory));
	}

	TArray<FString> TraceFiles;
	IFileManager::Get().FindFiles(TraceFiles, *FPaths::Combine(Directory, TEXT("*.utrace")), true, false);

	struct FTraceEntry
	{
		FString Name;
		FString Path;
		int64 SizeBytes;
		FDateTime ModifiedUtc;
	};

	TArray<FTraceEntry> Entries;
	Entries.Reserve(TraceFiles.Num());
	for (const FString& FileName : TraceFiles)
	{
		FString FullPath = FPaths::Combine(Directory, FileName);
		Entries.Add(FTraceEntry{
			FileName,
			FullPath,
			IFileManager::Get().FileSize(*FullPath),
			IFileManager::Get().GetTimeStamp(*FullPath) });
	}

	Entries.Sort([](const FTraceEntry& A, const FTraceEntry& B) { return A.ModifiedUtc > B.ModifiedUtc; });

	TArray<TSharedPtr<FJsonValue>> TracesArray;
	for (int32 Index = 0; Index < Entries.Num() && Index < Limit; ++Index)
	{
		const FTraceEntry& Entry = Entries[Index];
		auto TraceJson = MakeShared<FJsonObject>();
		TraceJson->SetStringField(TEXT("name"), Entry.Name);
		TraceJson->SetStringField(TEXT("path"), Entry.Path);
		TraceJson->SetNumberField(TEXT("size_bytes"), static_cast<double>(Entry.SizeBytes));
		TraceJson->SetStringField(TEXT("modified_utc"), Entry.ModifiedUtc.ToIso8601());
		TracesArray.Add(MakeShared<FJsonValueObject>(TraceJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("directory"), Directory);
	ResultJson->SetNumberField(TEXT("total_found"), Entries.Num());
	ResultJson->SetNumberField(TEXT("returned"), TracesArray.Num());
	ResultJson->SetArrayField(TEXT("traces"), TracesArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: session_info
// Params: { "trace": "...", "max_threads"?: number }
// ============================================================================

FMonolithActionResult FMonolithInsightsActions::SessionInfo(const TSharedPtr<FJsonObject>& Params)
{
	const FString TraceParam = Params->GetStringField(TEXT("trace"));
	if (TraceParam.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("session_info: 'trace' is required"));
	}

	const FString TracePath = ResolveTracePath(TraceParam);
	if (!FPaths::FileExists(TracePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("session_info: trace file not found: '%s'"), *TracePath));
	}

	int32 MaxThreads = 0;
	Params->TryGetNumberField(TEXT("max_threads"), MaxThreads);

	double MaxWaitSeconds = 20.0;
	Params->TryGetNumberField(TEXT("max_wait_seconds"), MaxWaitSeconds);

	TSharedPtr<const TraceServices::IAnalysisSession> Session = GetOrStartAnalysis(TracePath);
	if (!Session.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("session_info: failed to start analysis for '%s'"), *TracePath));
	}

	WaitForAnalysis(*Session, MaxWaitSeconds); // result folded into analysis_complete below; partial reads are safe.

	TraceServices::FAnalysisSessionReadScope ReadScope(*Session);

	TArray<TSharedPtr<FJsonValue>> ThreadsArray;
	int32 ThreadCount = 0;
	const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session);
	ThreadProvider.EnumerateThreads([&ThreadsArray, &ThreadCount, MaxThreads](const TraceServices::FThreadInfo& ThreadInfo)
	{
		++ThreadCount;
		if (MaxThreads > 0 && ThreadsArray.Num() >= MaxThreads)
		{
			return;
		}
		auto ThreadJson = MakeShared<FJsonObject>();
		ThreadJson->SetNumberField(TEXT("id"), ThreadInfo.Id);
		ThreadJson->SetStringField(TEXT("name"), ThreadInfo.Name ? ThreadInfo.Name : TEXT(""));
		ThreadJson->SetStringField(TEXT("group"), ThreadInfo.GroupName ? ThreadInfo.GroupName : TEXT(""));
		ThreadsArray.Add(MakeShared<FJsonValueObject>(ThreadJson));
	});

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("trace_path"), TracePath);
	ResultJson->SetStringField(TEXT("name"), Session->GetName());
	ResultJson->SetNumberField(TEXT("duration_seconds"), Session->GetDurationSeconds());
	ResultJson->SetBoolField(TEXT("analysis_complete"), Session->IsAnalysisComplete());
	ResultJson->SetNumberField(TEXT("thread_count"), ThreadCount);
	ResultJson->SetArrayField(TEXT("threads"), ThreadsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_frame
// Params: { "trace": "...", "frame_index": number, "frame_type"?: "Game"|"Rendering",
//           "threads"?: "GameThread,RenderThread", "max_events"?: number }
// ============================================================================

FMonolithActionResult FMonolithInsightsActions::GetFrame(const TSharedPtr<FJsonObject>& Params)
{
	const FString TraceParam = Params->GetStringField(TEXT("trace"));
	if (TraceParam.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("get_frame: 'trace' is required"));
	}

	const FString TracePath = ResolveTracePath(TraceParam);
	if (!FPaths::FileExists(TracePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("get_frame: trace file not found: '%s'"), *TracePath));
	}

	double FrameIndexParam = 0.0;
	if (!Params->TryGetNumberField(TEXT("frame_index"), FrameIndexParam))
	{
		return FMonolithActionResult::Error(TEXT("get_frame: 'frame_index' is required"));
	}
	const uint64 FrameIndex = static_cast<uint64>(FMath::Max(FrameIndexParam, 0.0));

	const ETraceFrameType FrameType = ParseFrameType(
		Params->HasField(TEXT("frame_type")) ? Params->GetStringField(TEXT("frame_type")) : FString());
	const TCHAR* FrameTypeName = (FrameType == ETraceFrameType::TraceFrameType_Rendering) ? TEXT("Rendering") : TEXT("Game");

	TArray<FString> ThreadFilters;
	if (Params->HasField(TEXT("threads")))
	{
		Params->GetStringField(TEXT("threads")).ParseIntoArray(ThreadFilters, TEXT(","), true);
		for (FString& Filter : ThreadFilters)
		{
			Filter.TrimStartAndEndInline();
		}
	}

	int32 MaxEvents = 2000;
	Params->TryGetNumberField(TEXT("max_events"), MaxEvents);

	double MinDurationMs = 0.0;
	Params->TryGetNumberField(TEXT("min_duration_ms"), MinDurationMs);

	double MaxWaitSeconds = 20.0;
	Params->TryGetNumberField(TEXT("max_wait_seconds"), MaxWaitSeconds);

	TSharedPtr<const TraceServices::IAnalysisSession> Session = GetOrStartAnalysis(TracePath);
	if (!Session.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("get_frame: failed to start analysis for '%s'"), *TracePath));
	}

	const bool bAnalysisComplete = WaitForAnalysis(*Session, MaxWaitSeconds);

	TraceServices::FAnalysisSessionReadScope ReadScope(*Session);

	// Native frame index -> [StartTime, EndTime) resolution. This is the whole point of this
	// action: no need to infer frame boundaries from a root GameThread timer event — Insights'
	// own frame-marker events already give us exact per-frame timestamps.
	const TraceServices::IFrameProvider& FrameProvider = TraceServices::ReadFrameProvider(*Session);
	const uint64 FrameCount = FrameProvider.GetFrameCount(FrameType);
	const TraceServices::FFrame* Frame = FrameProvider.GetFrame(FrameType, FrameIndex);
	if (Frame == nullptr)
	{
		// Two full Printf calls rather than a ternary picking the format string: UE's
		// compile-time format-string checker (TCheckedFormatString) requires a literal
		// argument, not a runtime-selected `const TCHAR*`.
		const FString ErrorMessage = bAnalysisComplete
			? FString::Printf(TEXT("get_frame: frame_index %llu out of range — trace has %llu frames of type '%s'"),
				FrameIndex, FrameCount, FrameTypeName)
			: FString::Printf(TEXT("get_frame: frame_index %llu not yet reached — analysis still in progress, %llu frames of type '%s' processed so far, try again"),
				FrameIndex, FrameCount, FrameTypeName);
		return FMonolithActionResult::Error(ErrorMessage);
	}

	const double FrameStartTime = Frame->StartTime;
	const double FrameEndTime = Frame->EndTime;

	const TraceServices::ITimingProfilerProvider* TimingProvider = TraceServices::ReadTimingProfilerProvider(*Session);
	if (TimingProvider == nullptr)
	{
		return FMonolithActionResult::Error(TEXT("get_frame: no timing profiler data in this trace"));
	}

	TArray<TSharedPtr<FJsonValue>> EventsArray;
	int32 EventsSeen = 0;
	bool bTruncated = false;

	// `ReadTimers` hands us the timer-name reader for the duration of the callback only, so
	// all the thread/timeline walking below happens nested inside it rather than resolving
	// timer names afterward from a dangling reference.
	TimingProvider->ReadTimers([&](const TraceServices::ITimingProfilerTimerReader& TimerReader)
	{
		const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(*Session);
		ThreadProvider.EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
		{
			if (bTruncated)
			{
				return; // EnumerateThreads has no early-stop; skip remaining work cheaply instead.
			}

			const FString ThreadName = ThreadInfo.Name ? ThreadInfo.Name : TEXT("");
			if (ThreadFilters.Num() > 0)
			{
				bool bMatches = false;
				for (const FString& Filter : ThreadFilters)
				{
					if (ThreadName.Contains(Filter))
					{
						bMatches = true;
						break;
					}
				}
				if (!bMatches)
				{
					return;
				}
			}

			uint32 TimelineIndex = 0;
			if (!TimingProvider->GetCpuThreadTimelineIndex(ThreadInfo.Id, TimelineIndex))
			{
				return; // Thread has no CPU timing-profiler timeline (e.g. a thread with no captured scopes).
			}

			TimingProvider->ReadTimeline(TimelineIndex, [&](const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
			{
				Timeline.EnumerateEvents(FrameStartTime, FrameEndTime,
					[&](double EventStartTime, double EventEndTime, uint32 Depth, const TraceServices::FTimingProfilerEvent& Event) -> TraceServices::EEventEnumerate
					{
						++EventsSeen;
						const double EventDurationMs = (EventEndTime - EventStartTime) * 1000.0;
						if (MinDurationMs > 0.0 && EventDurationMs < MinDurationMs)
						{
							return TraceServices::EEventEnumerate::Continue; // Doesn't count toward max_events — filtered out, not truncated.
						}
						if (MaxEvents > 0 && EventsArray.Num() >= MaxEvents)
						{
							bTruncated = true;
							return TraceServices::EEventEnumerate::Stop;
						}

						const TraceServices::FTimingProfilerTimer* Timer = TimerReader.GetTimer(Event.TimerIndex);

						auto EventJson = MakeShared<FJsonObject>();
						EventJson->SetNumberField(TEXT("thread_id"), ThreadInfo.Id);
						EventJson->SetStringField(TEXT("thread_name"), ThreadName);
						EventJson->SetNumberField(TEXT("timer_id"), Event.TimerIndex);
						EventJson->SetStringField(TEXT("timer_name"), (Timer && Timer->Name) ? Timer->Name : TEXT(""));
						EventJson->SetNumberField(TEXT("start_time"), EventStartTime);
						EventJson->SetNumberField(TEXT("end_time"), EventEndTime);
						EventJson->SetNumberField(TEXT("duration"), EventEndTime - EventStartTime);
						EventJson->SetNumberField(TEXT("depth"), Depth);
						EventsArray.Add(MakeShared<FJsonValueObject>(EventJson));

						return TraceServices::EEventEnumerate::Continue;
					});
			});
		});
	});

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("trace_path"), TracePath);
	ResultJson->SetStringField(TEXT("frame_type"), FrameTypeName);
	ResultJson->SetBoolField(TEXT("analysis_complete"), bAnalysisComplete);
	ResultJson->SetNumberField(TEXT("frame_index"), static_cast<double>(Frame->Index));
	ResultJson->SetNumberField(TEXT("frame_count"), static_cast<double>(FrameCount));
	ResultJson->SetNumberField(TEXT("start_time"), FrameStartTime);
	ResultJson->SetNumberField(TEXT("end_time"), FrameEndTime);
	ResultJson->SetNumberField(TEXT("duration_ms"), (FrameEndTime - FrameStartTime) * 1000.0);
	ResultJson->SetNumberField(TEXT("event_count"), EventsArray.Num());
	ResultJson->SetNumberField(TEXT("events_seen"), EventsSeen);
	ResultJson->SetBoolField(TEXT("truncated"), bTruncated);
	ResultJson->SetArrayField(TEXT("events"), EventsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: find_hitches
// Params: { "trace": "...", "frame_type"?: "Game"|"Rendering", "limit"?: number,
//           "min_ms"?: number, "max_wait_seconds"?: number }
// ============================================================================

FMonolithActionResult FMonolithInsightsActions::FindHitches(const TSharedPtr<FJsonObject>& Params)
{
	const FString TraceParam = Params->GetStringField(TEXT("trace"));
	if (TraceParam.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("find_hitches: 'trace' is required"));
	}

	const FString TracePath = ResolveTracePath(TraceParam);
	if (!FPaths::FileExists(TracePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("find_hitches: trace file not found: '%s'"), *TracePath));
	}

	const ETraceFrameType FrameType = ParseFrameType(
		Params->HasField(TEXT("frame_type")) ? Params->GetStringField(TEXT("frame_type")) : FString());
	const TCHAR* FrameTypeName = (FrameType == ETraceFrameType::TraceFrameType_Rendering) ? TEXT("Rendering") : TEXT("Game");

	int32 Limit = 20;
	Params->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Max(Limit, 1);

	double MinMs = 0.0;
	Params->TryGetNumberField(TEXT("min_ms"), MinMs);

	double MaxWaitSeconds = 20.0;
	Params->TryGetNumberField(TEXT("max_wait_seconds"), MaxWaitSeconds);

	TSharedPtr<const TraceServices::IAnalysisSession> Session = GetOrStartAnalysis(TracePath);
	if (!Session.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("find_hitches: failed to start analysis for '%s'"), *TracePath));
	}

	const bool bAnalysisComplete = WaitForAnalysis(*Session, MaxWaitSeconds);

	TraceServices::FAnalysisSessionReadScope ReadScope(*Session);

	const TraceServices::IFrameProvider& FrameProvider = TraceServices::ReadFrameProvider(*Session);
	const uint64 FrameCount = FrameProvider.GetFrameCount(FrameType);

	struct FFrameDuration
	{
		uint64 Index;
		double StartTime;
		double EndTime;
		double DurationMs;
	};

	TArray<FFrameDuration> Durations;
	Durations.Reserve(static_cast<int32>(FMath::Min<uint64>(FrameCount, 2000000)));

	double TotalMs = 0.0;
	uint64 FiniteFrameCount = 0;

	// The trace's last frame is unterminated (EndTime == +inf, i.e. no matching end event) if
	// capture stopped mid-frame — a clean disconnect, but also exactly what happens when the
	// process crashes: the frame that was running at the moment of the crash never closes.
	// Worth surfacing explicitly rather than letting it silently poison the average/sort with inf.
	bool bLastFrameUnterminated = false;
	uint64 UnterminatedFrameIndex = 0;
	double UnterminatedFrameStartTime = 0.0;

	FrameProvider.EnumerateFrames(FrameType, 0, FrameCount, [&](const TraceServices::FFrame& Frame)
	{
		const double Ms = (Frame.EndTime - Frame.StartTime) * 1000.0;
		if (!FMath::IsFinite(Ms))
		{
			bLastFrameUnterminated = true;
			UnterminatedFrameIndex = Frame.Index;
			UnterminatedFrameStartTime = Frame.StartTime;
			return;
		}
		TotalMs += Ms;
		++FiniteFrameCount;
		if (Ms >= MinMs)
		{
			Durations.Add(FFrameDuration{ Frame.Index, Frame.StartTime, Frame.EndTime, Ms });
		}
	});

	Durations.Sort([](const FFrameDuration& A, const FFrameDuration& B) { return A.DurationMs > B.DurationMs; });

	TArray<TSharedPtr<FJsonValue>> HitchesArray;
	for (int32 Index = 0; Index < Durations.Num() && Index < Limit; ++Index)
	{
		const FFrameDuration& D = Durations[Index];
		auto HitchJson = MakeShared<FJsonObject>();
		HitchJson->SetNumberField(TEXT("frame_index"), static_cast<double>(D.Index));
		HitchJson->SetNumberField(TEXT("start_time"), D.StartTime);
		HitchJson->SetNumberField(TEXT("end_time"), D.EndTime);
		HitchJson->SetNumberField(TEXT("duration_ms"), D.DurationMs);
		HitchesArray.Add(MakeShared<FJsonValueObject>(HitchJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("trace_path"), TracePath);
	ResultJson->SetStringField(TEXT("frame_type"), FrameTypeName);
	ResultJson->SetBoolField(TEXT("analysis_complete"), bAnalysisComplete);
	ResultJson->SetNumberField(TEXT("frame_count"), static_cast<double>(FrameCount));
	ResultJson->SetNumberField(TEXT("average_frame_ms"), FiniteFrameCount > 0 ? TotalMs / static_cast<double>(FiniteFrameCount) : 0.0);
	ResultJson->SetNumberField(TEXT("frames_over_threshold"), Durations.Num());
	ResultJson->SetBoolField(TEXT("trace_ends_mid_frame"), bLastFrameUnterminated);
	if (bLastFrameUnterminated)
	{
		// Likely the crash/disconnect point: this frame started but capture stopped before it closed.
		auto UnterminatedJson = MakeShared<FJsonObject>();
		UnterminatedJson->SetNumberField(TEXT("frame_index"), static_cast<double>(UnterminatedFrameIndex));
		UnterminatedJson->SetNumberField(TEXT("start_time"), UnterminatedFrameStartTime);
		ResultJson->SetObjectField(TEXT("unterminated_frame"), UnterminatedJson);
	}
	ResultJson->SetArrayField(TEXT("hitches"), HitchesArray);

	return FMonolithActionResult::Success(ResultJson);
}
