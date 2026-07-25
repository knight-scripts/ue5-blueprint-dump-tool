// Copyright (c) 2026 knight-scripts. MIT License.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraphNode;
class UEdGraphPin;
class UObject;
struct FEdGraphPinType;

/**
 * Shared utilities for dumping Blueprint structure to text.
 *
 * Core capability: exec pin chain walking (forward through white exec pins)
 * — the counterpart to AnimBPDumper's pose chain walking (backward through pose links).
 *
 * Used by both AnimBPDumper (EventGraph, Functions, Variables) and BlueprintDumper.
 */
class FBlueprintDumpUtils
{
public:
	// --- Pin helpers ---

	/** Check if a pin is an exec (white) pin. */
	static bool IsExecPin(const UEdGraphPin* Pin);

	/** Get all output exec pins from a node (for branching logic). */
	static TArray<UEdGraphPin*> GetExecOutputPins(UEdGraphNode* Node);

	/** Convert an FEdGraphPinType to a human-readable type string. */
	static FString PinTypeToString(const FEdGraphPinType& PinType);

	// --- Core: exec chain walker ---

	/** Walk forward through exec pins from a node, building a text representation.
	 *  NodeBudget bounds total nodes per walk (insurance against generated/macro-heavy graphs). */
	static void WalkExecChain(UEdGraphNode* Node, int32 Depth, FString& Output, TSet<UEdGraphNode*>& Visited, int32& NodeBudget);

	/** Get a human-readable label for a K2 (Blueprint logic) node. */
	static FString GetK2NodeLabel(UEdGraphNode* Node);

	/** Summarize data (non-exec) input pins and their values/connections. */
	static FString GetDataInputSummary(UEdGraphNode* Node);

	// --- Expression resolution (shared with AnimBPDumper) ---

	/** Map operator function names to symbols (e.g., "BooleanAND" -> "&&"). */
	static FString ResolveOperatorSymbol(const FString& FuncName);

	/** Resolve enum pin value — converts byte indices to display names. */
	static FString ResolveEnumPinValue(const UEdGraphPin* Pin);

	/** Extract operator from node title (e.g., "Equal (Enum)" -> "=="). */
	static FString ResolveOperatorFromTitle(const FString& Title);

	/** Default depth for data-pin expression reconstruction. Function-argument recursion
	 *  (Lerp(Clamp(SafeDivide(...)))) routinely runs 4-5 levels deep, so 4 truncated the
	 *  exact formulas M1 exists to recover. Raised 6 -> 8 after the 2026-07-26 verification
	 *  found GASP's CanSprint truncating at 7; truncation is now MARKED with "(...)", so if
	 *  8 is still short it shows up in the dump instead of reading as a zero-arg call. */
	static constexpr int32 DefaultExpressionDepth = 8;

	/** Recursively build expression string walking backward from a pin.
	 *  No visited set: valid BP data graphs are DAGs (the compiler rejects true cycles),
	 *  so shared subexpressions are legal and MaxDepth alone bounds the walk. */
	static FString BuildExpressionFromPin(UEdGraphPin* Pin, int32 MaxDepth = 10, int32 CurrentDepth = 0);

	/** THE "what drives this pin" entry point — every walker and pin formatter routes
	 *  through here. Three hand-copied versions of this used to short-circuit function
	 *  calls to a bare "FuncName()", discarding the whole formula. */
	static FString ResolvePinSourceLabel(UEdGraphPin* LinkedPin, int32 MaxDepth = DefaultExpressionDepth);

	/** Literal value of an UNCONNECTED pin: enum-resolved, strings/names quoted, object
	 *  literals by name. Empty when the pin carries no value. */
	static FString FormatLiteralPinValue(const UEdGraphPin* Pin);

	// --- Asset path normalization ---

	/** Normalize asset path for LoadObject — handles plugin paths, /All prefix, .AssetName suffix. */
	static FString NormalizeAssetPath(const FString& InputPath);

	// --- Section dumpers (work on any UBlueprint) ---

	/** Dump all variables (NewVariables) from a Blueprint. */
	static void DumpVariables(UBlueprint* Blueprint, FString& Output);

	/** Dump all EventGraph pages — find entry events, walk exec chains. */
	static void DumpEventGraphs(UBlueprint* Blueprint, FString& Output);

	/** Dump all function graphs — signature + exec chain walk. */
	static void DumpFunctions(UBlueprint* Blueprint, FString& Output);

	/** Dump implemented interfaces. */
	static void DumpInterfaces(UBlueprint* Blueprint, FString& Output);

	/** Dump component tree from the Blueprint's CDO. */
	static void DumpComponentTree(UBlueprint* Blueprint, FString& Output);

	/** Dump CDO property overrides vs the parent class defaults — the Class
	 *  Defaults panel state (incl. inherited components' knobs and BP-added
	 *  component templates). A property not listed is at its inherited default. */
	static void DumpClassDefaultOverrides(UBlueprint* Blueprint, FString& Output);

	/** Diff an object's properties against a baseline (its archetype, or its class CDO
	 *  when no archetype applies), RECURSING into instanced sub-objects and instanced
	 *  content inside structs/arrays. That recursion is the whole authoring model of
	 *  details-panel-driven plugins — without it their tuning is invisible.
	 *  Returns the number of differing entries emitted. */
	static int32 DumpObjectPropertyDiffs(UObject* Object, const UObject* Baseline, const FString& LinePrefix, FString& Output);

	// --- Formatting ---

	/** Indentation helper for exec chain output. */
	static FString Indent(int32 Depth);
};
