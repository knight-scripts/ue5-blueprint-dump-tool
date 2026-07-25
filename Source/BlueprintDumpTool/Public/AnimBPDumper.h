// Copyright (c) 2026 knight-scripts. MIT License.

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UAnimBlueprint;
class UAnimGraphNode_StateMachine;

/**
 * Dumps an AnimBP's full graph structure (state machines, transitions,
 * pose chains) to a human-readable text file.
 *
 * Console command: DumpAnimBP /Game/Path/To/AnimBP
 * Output: {ProjectDir}/Saved/AnimBPDumps/{Name}_Dump.txt
 */
class FAnimBPDumper
{
public:
	/** Console command entry point. Parses args, calls DumpAnimBP, writes file. */
	static void ExecuteCommand(const TArray<FString>& Args);

	/** Main orchestrator. Returns the full dump as a string. */
	static FString DumpAnimBP(const FString& AssetPath);

	/** Same, on an already-loaded asset (batch dumping loads once and dispatches). */
	static FString DumpAnimBPObject(UAnimBlueprint* AnimBP);

private:
	/** Recursive backward walk through pose link pins from a given node.
	 *  NodeBudget bounds total nodes per walk (insurance against pathological graphs). */
	static void WalkPoseChain(UEdGraphNode* Node, int32 Depth, FString& Output, TSet<UEdGraphNode*>& Visited, int32& NodeBudget);

	/** Dump a state machine: entry state, states, transitions, then each state's content. */
	static void DumpStateMachine(UAnimGraphNode_StateMachine* SMNode, int32 Depth, FString& Output, TSet<UEdGraphNode*>& Visited);

	/** Dump the content of a state's bound graph (calls WalkPoseChain on its result node). */
	static void DumpStateContent(UEdGraph* StateGraph, int32 Depth, FString& Output, TSet<UEdGraphNode*>& Visited);

	/** Reconstruct a boolean expression string from a transition rule graph. */
	static FString BuildTransitionRuleExpression(UEdGraph* TransitionGraph);

	/** Check if a pin is a pose link (FPoseLink / FComponentSpacePoseLink). */
	static bool IsPoseLinkPin(const UEdGraphPin* Pin);

	/** Get a human-readable label for an anim graph node. */
	static FString GetNodeLabel(UEdGraphNode* Node);

	/** Format non-pose data pins with their values/connections. */
	static FString FormatDataPins(UEdGraphNode* Node);

	/** Find the entry state name from a state machine graph. */
	static FString FindEntryStateName(UEdGraph* StateMachineGraph);

	/** Dump animation layer interfaces and their layer functions. */
	static void DumpAnimationLayers(UAnimBlueprint* AnimBP, FString& Output);

	/** Dump SaveCachedPose nodes in a graph that weren't visited during the main walk. */
	static void DumpUnvisitedCachedPoses(UEdGraph* Graph, int32 BaseDepth, FString& Output, TSet<UEdGraphNode*>& Visited);

	/** Indent helper. */
	static FString Indent(int32 Depth);
};
