#include "MonolithSourceControlActions.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h" // LogMonolith

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"

namespace MonolithSourceControlInternal
{
	// Resolve a /Game long package name to its on-disk file, trying the asset extension
	// first, then the map extension. bOutOnDisk stays false (best-effort asset-extension
	// guess still returned) when neither exists yet — a package that has never been saved
	// has nothing on disk for source control to track.
	FString ResolveDiskPath(const FString& PackageName, bool& bOutOnDisk)
	{
		bOutOnDisk = false;
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		FString AssetFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFilename, FPackageName::GetAssetPackageExtension()))
		{
			if (PlatformFile.FileExists(*AssetFilename))
			{
				bOutOnDisk = true;
				return AssetFilename;
			}
		}

		FString MapFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, MapFilename, FPackageName::GetMapPackageExtension()))
		{
			if (PlatformFile.FileExists(*MapFilename))
			{
				bOutOnDisk = true;
				return MapFilename;
			}
		}

		return AssetFilename;
	}

	// Force-refresh then check out one file if it needs it. Merges result fields into
	// OutFields. Never shows UI — Execute()/GetState() talk to the SCC provider directly.
	void CheckoutOneFile(ISourceControlProvider& Provider, const FString& Filename, const TSharedPtr<FJsonObject>& OutFields)
	{
		TArray<FString> Files{ Filename };

		// The cached state can be stale for a file Monolith itself is about to modify —
		// force a fresh read so "already checked out" isn't a false negative.
		Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files, EConcurrency::Synchronous);

		TArray<FSourceControlStateRef> States;
		if (Provider.GetState(Files, States, EStateCacheUsage::Use) != ECommandResult::Succeeded || States.Num() == 0)
		{
			OutFields->SetBoolField(TEXT("source_controlled"), false);
			return;
		}

		const FSourceControlStateRef& State = States[0];
		const bool bSourceControlled = State->IsSourceControlled();
		OutFields->SetBoolField(TEXT("source_controlled"), bSourceControlled);
		if (!bSourceControlled)
		{
			return; // Not tracked — nothing to check out.
		}

		if (State->IsCheckedOut() || State->IsAdded())
		{
			OutFields->SetBoolField(TEXT("checked_out"), true);
			return;
		}

		FString OtherUser;
		if (State->IsCheckedOutOther(&OtherUser))
		{
			// Someone else holds it. Report the conflict rather than fighting the lock —
			// the caller's own write will fail naturally if the file is actually locked.
			OutFields->SetBoolField(TEXT("checked_out"), false);
			OutFields->SetStringField(TEXT("checked_out_by_other"), OtherUser);
			return;
		}

		if (!State->CanCheckout())
		{
			OutFields->SetBoolField(TEXT("checked_out"), false);
			OutFields->SetStringField(TEXT("checkout_error"), TEXT("File is not in a checkout-able state (deleted/ignored/unknown upstream)."));
			return;
		}

		const ECommandResult::Type Result = Provider.Execute(ISourceControlOperation::Create<FCheckOut>(), Files, EConcurrency::Synchronous);
		OutFields->SetBoolField(TEXT("checked_out"), Result == ECommandResult::Succeeded);
		if (Result != ECommandResult::Succeeded)
		{
			OutFields->SetStringField(TEXT("checkout_error"), TEXT("Provider checkout operation failed or was cancelled."));
		}
	}

	bool ExtractPackages(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutNames, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("packages"), Arr) || !Arr || Arr->Num() == 0)
		{
			OutError = TEXT("Missing required parameter: packages (non-empty array of long package names)");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			FString Name;
			if (Val.IsValid() && Val->TryGetString(Name) && !Name.IsEmpty())
			{
				OutNames.AddUnique(Name);
			}
		}
		if (OutNames.Num() == 0)
		{
			OutError = TEXT("packages array contained no valid package names");
			return false;
		}
		return true;
	}
}

void FMonolithSourceControlActions::CheckoutPackageForWrite(const FString& PackageName, const TSharedPtr<FJsonObject>& OutFields)
{
	if (!ISourceControlModule::Get().IsEnabled())
	{
		OutFields->SetBoolField(TEXT("source_controlled"), false);
		return;
	}

	bool bOnDisk = false;
	const FString Filename = MonolithSourceControlInternal::ResolveDiskPath(PackageName, bOnDisk);
	if (!bOnDisk)
	{
		OutFields->SetBoolField(TEXT("source_controlled"), false);
		return;
	}

	MonolithSourceControlInternal::CheckoutOneFile(ISourceControlModule::Get().GetProvider(), Filename, OutFields);
}

void FMonolithSourceControlActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("editor"), TEXT("source_control_status"),
		TEXT("Report Source Control status for the given packages — headless, never raises the engine's Check Out / status UI. Returns source_control_enabled plus per-package {package, disk_path, on_disk, source_controlled, checked_out, checked_out_other, checked_out_by, added, current}. Use to check whether a checkout would be needed before save_packages/source_control_checkout."),
		FMonolithActionHandler::CreateStatic(&HandleSourceControlStatus),
		FParamSchemaBuilder()
			.Required(TEXT("packages"), TEXT("array"), TEXT("Array of long package names (e.g. [\"/Game/Tests/Monolith/DA_Foo\"]) to query."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("source_control_checkout"),
		TEXT("Headlessly check out the given packages via ISourceControlProvider — the programmatic equivalent of accepting the engine's 'Check Out Files?' dialog, but silent (that dialog's nested Slate loop blocks the game thread and freezes the MCP server, so it must never be shown for an MCP-driven write). No-op per-package when the package isn't source-controlled or has never been saved to disk. save_packages already does this automatically via auto_checkout (default true) — call this directly when you need a package writable ahead of a non-save edit, or want to check out several packages up front."),
		FMonolithActionHandler::CreateStatic(&HandleSourceControlCheckout),
		FParamSchemaBuilder()
			.Required(TEXT("packages"), TEXT("array"), TEXT("Array of long package names to check out."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("source_control_revert"),
		TEXT("Revert the given packages in Source Control — discards any pending checkout/edit for them upstream. DESTRUCTIVE for local SCC changes (does not touch in-memory UPackage dirty state, which is a separate concept). Use to undo an accidental auto_checkout or source_control_checkout call."),
		FMonolithActionHandler::CreateStatic(&HandleSourceControlRevert),
		FParamSchemaBuilder()
			.Required(TEXT("packages"), TEXT("array"), TEXT("Array of long package names to revert."))
			.Build());

	UE_LOG(LogMonolith, Log, TEXT("MonolithEditor: registered 3 source-control actions"));
}

FMonolithActionResult FMonolithSourceControlActions::HandleSourceControlStatus(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithSourceControlInternal;

	TArray<FString> PackageNames;
	FString Err;
	if (!ExtractPackages(Params, PackageNames, Err))
	{
		return FMonolithActionResult::Error(Err);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const bool bEnabled = ISourceControlModule::Get().IsEnabled();
	Result->SetBoolField(TEXT("source_control_enabled"), bEnabled);

	TArray<TSharedPtr<FJsonValue>> Rows;
	ISourceControlProvider* Provider = bEnabled ? &ISourceControlModule::Get().GetProvider() : nullptr;

	for (const FString& PackageName : PackageNames)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), PackageName);

		bool bOnDisk = false;
		const FString Filename = ResolveDiskPath(PackageName, bOnDisk);
		Row->SetStringField(TEXT("disk_path"), Filename);
		Row->SetBoolField(TEXT("on_disk"), bOnDisk);

		if (Provider && bOnDisk)
		{
			TArray<FString> Files{ Filename };
			TArray<FSourceControlStateRef> States;
			Provider->Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files, EConcurrency::Synchronous);
			if (Provider->GetState(Files, States, EStateCacheUsage::Use) == ECommandResult::Succeeded && States.Num() > 0)
			{
				const FSourceControlStateRef& State = States[0];
				FString OtherUser;
				const bool bCheckedOutOther = State->IsCheckedOutOther(&OtherUser);
				Row->SetBoolField(TEXT("source_controlled"), State->IsSourceControlled());
				Row->SetBoolField(TEXT("checked_out"), State->IsCheckedOut());
				Row->SetBoolField(TEXT("checked_out_other"), bCheckedOutOther);
				if (bCheckedOutOther) { Row->SetStringField(TEXT("checked_out_by"), OtherUser); }
				Row->SetBoolField(TEXT("added"), State->IsAdded());
				Row->SetBoolField(TEXT("current"), State->IsCurrent());
			}
			else
			{
				Row->SetStringField(TEXT("error"), TEXT("Failed to fetch source control state"));
			}
		}
		else
		{
			Row->SetBoolField(TEXT("source_controlled"), false);
		}

		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	Result->SetArrayField(TEXT("packages"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceControlActions::HandleSourceControlCheckout(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithSourceControlInternal;

	TArray<FString> PackageNames;
	FString Err;
	if (!ExtractPackages(Params, PackageNames, Err))
	{
		return FMonolithActionResult::Error(Err);
	}

	if (!ISourceControlModule::Get().IsEnabled())
	{
		return FMonolithActionResult::Error(TEXT("Source control is not enabled in this editor session."));
	}
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 CheckedOutCount = 0;
	for (const FString& PackageName : PackageNames)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), PackageName);

		bool bOnDisk = false;
		const FString Filename = ResolveDiskPath(PackageName, bOnDisk);
		Row->SetStringField(TEXT("disk_path"), Filename);
		Row->SetBoolField(TEXT("on_disk"), bOnDisk);

		if (bOnDisk)
		{
			CheckoutOneFile(Provider, Filename, Row);
		}
		else
		{
			Row->SetBoolField(TEXT("source_controlled"), false);
			Row->SetBoolField(TEXT("checked_out"), false);
		}

		bool bCheckedOut = false;
		Row->TryGetBoolField(TEXT("checked_out"), bCheckedOut);
		if (bCheckedOut) { ++CheckedOutCount; }

		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), CheckedOutCount == PackageNames.Num());
	Result->SetNumberField(TEXT("checked_out_count"), CheckedOutCount);
	Result->SetArrayField(TEXT("packages"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceControlActions::HandleSourceControlRevert(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithSourceControlInternal;

	TArray<FString> PackageNames;
	FString Err;
	if (!ExtractPackages(Params, PackageNames, Err))
	{
		return FMonolithActionResult::Error(Err);
	}

	if (!ISourceControlModule::Get().IsEnabled())
	{
		return FMonolithActionResult::Error(TEXT("Source control is not enabled in this editor session."));
	}
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	TArray<FString> Filenames;
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FString& PackageName : PackageNames)
	{
		bool bOnDisk = false;
		const FString Filename = ResolveDiskPath(PackageName, bOnDisk);

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), PackageName);
		Row->SetStringField(TEXT("disk_path"), Filename);
		Row->SetBoolField(TEXT("on_disk"), bOnDisk);
		if (bOnDisk)
		{
			Filenames.Add(Filename);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	if (Filenames.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("None of the requested packages resolved to an on-disk file."));
	}

	const ECommandResult::Type Result = Provider.Execute(ISourceControlOperation::Create<FRevert>(), Filenames, EConcurrency::Synchronous);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), Result == ECommandResult::Succeeded);
	Out->SetArrayField(TEXT("packages"), Rows);
	if (Result != ECommandResult::Succeeded)
	{
		Out->SetStringField(TEXT("error"), TEXT("Provider revert operation failed or was cancelled."));
	}
	return FMonolithActionResult::Success(Out);
}
