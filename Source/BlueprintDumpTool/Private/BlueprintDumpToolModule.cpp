// Copyright (c) 2026 knight-scripts. MIT License.

#include "BlueprintDumpToolModule.h"
#include "AnimBPDumper.h"
#include "BlueprintDumper.h"
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
		TEXT("Dumps a Blueprint's structure to a text file. Usage: DumpBP /Game/Path/To/Blueprint"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FBlueprintDumper::ExecuteCommand),
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
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintDumpToolModule, BlueprintDumpTool)
