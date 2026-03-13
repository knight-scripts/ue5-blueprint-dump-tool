// Copyright (c) 2026 knight-scripts. MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Dumps a generic Blueprint's structure (components, variables, EventGraph,
 * functions) to a human-readable text file.
 *
 * Console command: DumpBP /Game/Path/To/Blueprint
 * Output: {ProjectDir}/Saved/AnimBPDumps/{Name}_Dump.txt
 */
class FBlueprintDumper
{
public:
	/** Console command entry point. Parses args, calls DumpBlueprint, writes file. */
	static void ExecuteCommand(const TArray<FString>& Args);

	/** Main orchestrator. Returns the full dump as a string. */
	static FString DumpBlueprint(const FString& AssetPath);
};
