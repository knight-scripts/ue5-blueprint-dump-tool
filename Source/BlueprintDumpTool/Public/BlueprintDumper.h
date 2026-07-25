// Copyright (c) 2026 knight-scripts. MIT License.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;

/**
 * Dumps a generic Blueprint's structure (components, variables, EventGraph,
 * functions) to a human-readable text file.
 *
 * Console command: DumpBP /Game/Path/To/Blueprint
 * Output: {ProjectDir}/Saved/AnimBPDumps/{Name}_Dump.txt
 *
 * DumpBP accepts NON-Blueprint assets too — it hands them to FAssetDumper, which
 * dispatches by class (DataAsset, BehaviorTree, Curve, Struct, ...).
 */
class FBlueprintDumper
{
public:
	/** Console command entry point. Parses args, calls DumpBlueprint, writes file. */
	static void ExecuteCommand(const TArray<FString>& Args);

	/** Load by path, then dispatch by asset type. Returns the full dump as a string. */
	static FString DumpBlueprint(const FString& AssetPath);

	/** The Blueprint-specific dump, on an already-loaded asset. */
	static FString DumpBlueprintObject(UBlueprint* Blueprint);
};
