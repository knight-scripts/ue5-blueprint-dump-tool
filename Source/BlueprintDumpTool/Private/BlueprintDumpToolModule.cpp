// Copyright (c) 2026 knight-scripts. MIT License.

#include "BlueprintDumpToolModule.h"
#include "AnimBPDumper.h"
#include "BlueprintDumper.h"
#include "AssetDumper.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "FBlueprintDumpToolModule"

void FBlueprintDumpToolModule::StartupModule()
{
	DumpAnimBPCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("DumpAnimBP"),
		TEXT("Dumps an AnimBP's graph structure to a text file. Usage: DumpAnimBP /Game/Path/To/AnimBP"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FAnimBPDumper::ExecuteCommand),
		ECVF_Default
	);

	DumpBPCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("DumpBP"),
		TEXT("Dumps an asset's structure to a text file (Blueprint, DataAsset, BehaviorTree, Curve, Struct, ...). Usage: DumpBP /Game/Path/To/Asset"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FBlueprintDumper::ExecuteCommand),
		ECVF_Default
	);

	DumpBPFolderCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("DumpBPFolder"),
		TEXT("Batch-dumps every asset under a content path, with a manifest. Usage: DumpBPFolder /Game/Path [-recursive] [-filter=ClassName]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FAssetDumper::ExecuteFolderCommand),
		ECVF_Default
	);
}

void FBlueprintDumpToolModule::ShutdownModule()
{
	if (DumpAnimBPCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(DumpAnimBPCommand);
		DumpAnimBPCommand = nullptr;
	}

	if (DumpBPCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(DumpBPCommand);
		DumpBPCommand = nullptr;
	}

	if (DumpBPFolderCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(DumpBPFolderCommand);
		DumpBPFolderCommand = nullptr;
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintDumpToolModule, BlueprintDumpTool)
