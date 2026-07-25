// Copyright (c) 2026 knight-scripts. MIT License.

#include "BlueprintDumper.h"
#include "BlueprintDumpUtils.h"
#include "AssetDumper.h"

#include "Engine/Blueprint.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ============================================================================
// Console command entry point
// ============================================================================

void FBlueprintDumper::ExecuteCommand(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogTemp, Display, TEXT(""));
		UE_LOG(LogTemp, Display, TEXT("Usage: DumpBP /Game/Path/To/Asset"));
		UE_LOG(LogTemp, Display, TEXT(""));
		UE_LOG(LogTemp, Display, TEXT("  Dumps asset structure to Saved/AnimBPDumps/{Name}_Dump.txt"));
		UE_LOG(LogTemp, Display, TEXT("  Handles Blueprints, DataAssets, BehaviorTrees, Blackboards,"));
		UE_LOG(LogTemp, Display, TEXT("  Curves, DataTables, user structs/enums; anything else gets a"));
		UE_LOG(LogTemp, Display, TEXT("  generic property dump naming its class."));
		UE_LOG(LogTemp, Display, TEXT("  Example: DumpBP /Game/Characters/BP_MyCharacter"));
		UE_LOG(LogTemp, Display, TEXT("  Batch:   DumpBPFolder /Game/Path -recursive"));
		UE_LOG(LogTemp, Display, TEXT(""));
		return;
	}

	const FString& RawPath = Args[0];
	FString AssetPath = FBlueprintDumpUtils::NormalizeAssetPath(RawPath);
	FString Result = DumpBlueprint(AssetPath);

	if (Result.IsEmpty())
	{
		return;
	}

	FString AssetName = FPaths::GetBaseFilename(AssetPath);

	FString OutputDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("AnimBPDumps"));
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	FString OutputPath = FPaths::Combine(OutputDir, AssetName + TEXT("_Dump.txt"));

	if (FFileHelper::SaveStringToFile(Result, *OutputPath))
	{
		UE_LOG(LogTemp, Display, TEXT("Blueprint dump written to: %s"), *OutputPath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to write dump file: %s"), *OutputPath);
	}

	// Print first 200 lines to log
	TArray<FString> Lines;
	Result.ParseIntoArrayLines(Lines);
	int32 LinesToPrint = FMath::Min(Lines.Num(), 200);
	for (int32 i = 0; i < LinesToPrint; ++i)
	{
		UE_LOG(LogTemp, Display, TEXT("%s"), *Lines[i]);
	}
	if (Lines.Num() > 200)
	{
		UE_LOG(LogTemp, Display, TEXT("... (%d more lines in file)"), Lines.Num() - 200);
	}
}

// ============================================================================
// Main orchestrator
// ============================================================================

FString FBlueprintDumper::DumpBlueprint(const FString& AssetPath)
{
	UObject* Asset = FAssetDumper::LoadAsset(AssetPath);
	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("DumpBP: no asset found at '%s'"), *AssetPath);
		return FString();
	}

	// Not a Blueprint? Say what it actually IS and dump it anyway — the old code
	// reported "could not load" for an asset that loaded perfectly well, which read
	// as "missing" when it meant "unhandled type".
	return FAssetDumper::DumpLoadedAsset(Asset);
}

FString FBlueprintDumper::DumpBlueprintObject(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return FString();
	}

	FString Output;

	// Header
	Output += FString::Printf(TEXT("=== Blueprint: %s ===\n"), *Blueprint->GetName());

	if (Blueprint->ParentClass)
	{
		Output += FString::Printf(TEXT("Parent Class: %s\n"), *Blueprint->ParentClass->GetName());
	}

	Output += TEXT("\n");

	// Sections
	FBlueprintDumpUtils::DumpComponentTree(Blueprint, Output);
	FBlueprintDumpUtils::DumpClassDefaultOverrides(Blueprint, Output);
	FBlueprintDumpUtils::DumpVariables(Blueprint, Output);
	FBlueprintDumpUtils::DumpInterfaces(Blueprint, Output);
	FBlueprintDumpUtils::DumpEventGraphs(Blueprint, Output);
	FBlueprintDumpUtils::DumpFunctions(Blueprint, Output);

	return Output;
}
