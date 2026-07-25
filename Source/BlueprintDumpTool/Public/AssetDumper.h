// Copyright (c) 2026 knight-scripts. MIT License.

#pragma once

#include "CoreMinimal.h"

class UObject;

/**
 * Dumps NON-Blueprint assets, and drives batch (folder) dumping.
 *
 * The Blueprint dumpers only ever accepted UBlueprint, so every other asset type
 * was a silent no-op — a problem for any plugin that keeps its behaviour in data
 * assets, behavior trees and curves rather than in graphs.
 *
 * Dispatch order: AnimBlueprint -> Blueprint -> the specialised dumpers below ->
 * a generic "properties that differ from class defaults" fallback. The asset's
 * real class always appears in the header, so an unhandled type is loud.
 *
 * Console commands:
 *   DumpBP       /Game/Path/To/Asset
 *   DumpBPFolder /Game/Path [-recursive] [-filter=ClassName]
 */
class FAssetDumper
{
public:
	/** Load any asset by path, tolerating the Content-Browser path forms. */
	static UObject* LoadAsset(const FString& AssetPath);

	/** Dispatch an already-loaded asset to the right dumper. Never returns empty
	 *  for a valid object — worst case it is a generic property dump. */
	static FString DumpLoadedAsset(UObject* Asset);

	/** Short type label used in dump headers and the batch manifest. */
	static FString GetAssetTypeLabel(const UObject* Asset);

	/** Console command entry point for DumpBPFolder. */
	static void ExecuteFolderCommand(const TArray<FString>& Args);
};
