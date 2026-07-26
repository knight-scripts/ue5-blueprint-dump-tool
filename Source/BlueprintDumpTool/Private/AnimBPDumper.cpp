// Copyright (c) 2026 knight-scripts. MIT License.

#include "AnimBPDumper.h"
#include "BlueprintDumpUtils.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimNodeBase.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationGraph.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

// ============================================================================
// Anim node fidelity helpers (file-local)
//
// Anim graph nodes keep most of their configuration in the inner FAnimNode_*
// struct behind meta=(PinHiddenByDefault) — no visible pin ever exists, so a
// pin-only dump silently loses every such setting (Blend Stack is the worst
// case: ALL of its ~25 settings are hidden pins). These helpers surface:
//   (A) inner-struct settings that differ from the struct's defaults
//   (B) pin property bindings (the "Bind" dropdown) — a bound pin's DefaultValue
//       is stale; runtime reads the bound property instead
//   (C) member-function bindings (On Initial Update / Become Relevant / Update)
// ============================================================================

/** (A) Settings from the node's inner FAnimNode_* struct that differ from defaults.
 *  For Blend Stack nodes a small headline set is always shown, defaults included. */
static FString FormatAnimNodeSettings(UAnimGraphNode_Base* AnimNode, const FString& NodeLabel)
{
	FStructProperty* NodeProp = AnimNode->GetFNodeProperty();
	if (!NodeProp || !NodeProp->Struct)
	{
		return FString();
	}

	UScriptStruct* NodeStruct = NodeProp->Struct;
	const void* NodeData = NodeProp->ContainerPtrToValuePtr<void>(AnimNode);

	// Default-constructed instance of the same struct, for diffing
	FStructOnScope Defaults(NodeStruct);
	const void* DefaultData = Defaults.GetStructMemory();
	if (!NodeData || !DefaultData)
	{
		return FString();
	}

	// Blend Stack: its defining settings must be visible even when left at defaults
	const bool bIsBlendStack = NodeStruct->GetName().StartsWith(TEXT("AnimNode_BlendStack"));
	static const FName BlendStackAlwaysShow[] =
		{ TEXT("MaxActiveBlends"), TEXT("bUseInertialBlend"), TEXT("BlendTime"), TEXT("BlendProfile") };

	TArray<FString> Parts;
	for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
	{
		FProperty* Prop = *It;

		if (Prop->HasAnyPropertyFlags(CPF_Transient))
		{
			continue;
		}
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue; // only editor-authored settings
		}

		// Pose links are walked as children, not settings
		{
			const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				StructProp = CastField<FStructProperty>(ArrayProp->Inner);
			}
			if (StructProp && StructProp->Struct->IsChildOf(FPoseLinkBase::StaticStruct()))
			{
				continue;
			}
		}

		// Properties surfaced as visible pins are already dumped via FormatDataPins
		bool bHasVisiblePin = false;
		for (UEdGraphPin* Pin : AnimNode->Pins)
		{
			if (Pin && !Pin->bHidden && Pin->PinName == Prop->GetFName())
			{
				bHasVisiblePin = true;
				break;
			}
		}
		if (bHasVisiblePin)
		{
			continue;
		}

		const void* Value = Prop->ContainerPtrToValuePtr<void>(NodeData);
		const void* DefValue = Prop->ContainerPtrToValuePtr<void>(DefaultData);
		const bool bDiffers = !Prop->Identical(Value, DefValue, PPF_None);

		bool bForceShow = false;
		if (bIsBlendStack)
		{
			for (const FName& Name : BlendStackAlwaysShow)
			{
				if (Prop->GetFName() == Name)
				{
					bForceShow = true;
					break;
				}
			}
		}
		if (!bDiffers && !bForceShow)
		{
			continue;
		}

		FString ValueStr;
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Obj = ObjProp->GetObjectPropertyValue(Value);
			ValueStr = Obj ? Obj->GetName() : TEXT("None");
			// Asset refs that already ride the node title (e.g. SequencePlayer's clip) — skip the duplicate
			if (Obj && NodeLabel.Contains(ValueStr))
			{
				continue;
			}
		}
		else
		{
			Prop->ExportTextItem_Direct(ValueStr, Value, nullptr, nullptr, PPF_None);
		}

		if (ValueStr.Len() > 96)
		{
			ValueStr = ValueStr.Left(93) + TEXT("...");
		}
		Parts.Add(FString::Printf(TEXT("%s=%s"), *Prop->GetName(), *ValueStr));
	}

	if (Parts.Num() == 0)
	{
		return FString();
	}
	return TEXT(" [Settings: ") + FString::Join(Parts, TEXT(", ")) + TEXT("]");
}

/** (C) On Initial Update / On Become Relevant / On Update member-function bindings. */
static FString FormatNodeFunctionBindings(UAnimGraphNode_Base* AnimNode)
{
	TArray<FString> Parts;
	auto AddBinding = [&Parts](const TCHAR* Label, const FMemberReference& Ref)
	{
		if (!Ref.GetMemberName().IsNone())
		{
			Parts.Add(FString::Printf(TEXT("[%s: %s]"), Label, *Ref.GetMemberName().ToString()));
		}
	};
	AddBinding(TEXT("OnInitialUpdate"), AnimNode->InitialUpdateFunction);
	AddBinding(TEXT("OnBecomeRelevant"), AnimNode->BecomeRelevantFunction);
	AddBinding(TEXT("OnUpdate"), AnimNode->UpdateFunction);

	if (Parts.Num() == 0)
	{
		return FString();
	}
	return TEXT(" ") + FString::Join(Parts, TEXT(" "));
}

/** (B) Pin property bindings ("Bind" dropdown), keyed by property/pin name.
 *  The binding impl class (UAnimGraphNodeBinding_Base) lives in a Private engine
 *  header, so its PropertyBindings map is read via reflection; the value struct
 *  FAnimGraphNodePropertyBinding is public in AnimGraphNode_Base.h. */
static void CollectPropertyBindings(UEdGraphNode* Node, TMap<FName, FString>& OutBindings)
{
	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
	if (!AnimNode)
	{
		return;
	}

	FObjectProperty* BindingProp = FindFProperty<FObjectProperty>(UAnimGraphNode_Base::StaticClass(), TEXT("Binding"));
	if (!BindingProp)
	{
		return;
	}
	UObject* BindingObj = BindingProp->GetObjectPropertyValue_InContainer(AnimNode);
	if (!BindingObj)
	{
		return;
	}

	FMapProperty* MapProp = FindFProperty<FMapProperty>(BindingObj->GetClass(), TEXT("PropertyBindings"));
	if (!MapProp || !CastField<FNameProperty>(MapProp->KeyProp))
	{
		return;
	}
	const FStructProperty* ValueStructProp = CastField<FStructProperty>(MapProp->ValueProp);
	if (!ValueStructProp || ValueStructProp->Struct->GetFName() != TEXT("AnimGraphNodePropertyBinding"))
	{
		return;
	}

	FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(BindingObj));
	for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
	{
		if (!MapHelper.IsValidIndex(Index))
		{
			continue;
		}
		const FName PropName = *reinterpret_cast<const FName*>(MapHelper.GetKeyPtr(Index));
		const FAnimGraphNodePropertyBinding* Binding =
			reinterpret_cast<const FAnimGraphNodePropertyBinding*>(MapHelper.GetValuePtr(Index));

		FString Path = Binding->PathAsText.ToString();
		if (Path.IsEmpty())
		{
			Path = FString::Join(Binding->PropertyPath, TEXT("."));
		}
		OutBindings.Add(PropName, Path);
	}
}

// ============================================================================
// Console command entry point
// ============================================================================

void FAnimBPDumper::ExecuteCommand(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogTemp, Display, TEXT(""));
		UE_LOG(LogTemp, Display, TEXT("Usage: DumpAnimBP /Game/Path/To/AnimBP"));
		UE_LOG(LogTemp, Display, TEXT(""));
		UE_LOG(LogTemp, Display, TEXT("  Dumps AnimBP graph structure to Saved/AnimBPDumps/{Name}_Dump.txt"));
		UE_LOG(LogTemp, Display, TEXT("  Example: DumpAnimBP /Game/Characters/Animation/ABP_MyCharacter"));
		UE_LOG(LogTemp, Display, TEXT(""));
		return;
	}

	const FString& RawPath = Args[0];
	FString AssetPath = FBlueprintDumpUtils::NormalizeAssetPath(RawPath);
	FString Result = DumpAnimBP(AssetPath);

	if (Result.IsEmpty())
	{
		return; // Error already logged inside DumpAnimBP
	}

	// Extract name from normalized path for filename
	FString AssetName = FPaths::GetBaseFilename(AssetPath);

	// Write to file
	FString OutputDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("AnimBPDumps"));
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	FString OutputPath = FPaths::Combine(OutputDir, AssetName + TEXT("_Dump.txt"));

	if (FFileHelper::SaveStringToFile(Result, *OutputPath))
	{
		UE_LOG(LogTemp, Display, TEXT("AnimBP dump written to: %s"), *OutputPath);
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

FString FAnimBPDumper::DumpAnimBP(const FString& AssetPath)
{
	// Load the AnimBlueprint asset
	UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBP)
	{
		// Try appending _C or .AnimBP suffix
		FString AltPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
		AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
	}

	if (!AnimBP)
	{
		UE_LOG(LogTemp, Error, TEXT("DumpAnimBP: Could not load AnimBP at '%s'"), *AssetPath);
		return FString();
	}

	return DumpAnimBPObject(AnimBP);
}

FString FAnimBPDumper::DumpAnimBPObject(UAnimBlueprint* AnimBP)
{
	if (!AnimBP)
	{
		return FString();
	}

	FString Output;

	// Header info
	Output += FString::Printf(TEXT("=== AnimBP: %s ===\n"), *AnimBP->GetName());

	if (AnimBP->TargetSkeleton)
	{
		Output += FString::Printf(TEXT("Skeleton: %s\n"), *AnimBP->TargetSkeleton->GetName());
	}

	if (AnimBP->ParentClass)
	{
		Output += FString::Printf(TEXT("Parent Class: %s\n"), *AnimBP->ParentClass->GetName());
	}

	// Get all graphs (includes sub-graphs, state bound graphs, etc.)
	TArray<UEdGraph*> AllGraphs;
	AnimBP->GetAllGraphs(AllGraphs);

	// List only top-level graphs (skip sub-graphs owned by states/transitions)
	Output += TEXT("Graphs:");
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		UObject* Outer = Graph->GetOuter();
		bool bIsTopLevel = (Outer == AnimBP || Outer == AnimBP->GeneratedClass);
		if (bIsTopLevel)
		{
			Output += FString::Printf(TEXT(" %s,"), *Graph->GetName());
		}
	}
	Output.RemoveFromEnd(TEXT(","));
	Output += TEXT("\n\n");

	// Variables, Class Defaults overrides & Interfaces (before AnimGraph)
	FBlueprintDumpUtils::DumpVariables(AnimBP, Output);
	FBlueprintDumpUtils::DumpClassDefaultOverrides(AnimBP, Output);
	FBlueprintDumpUtils::DumpInterfaces(AnimBP, Output);

	// Find the AnimGraph (main animation graph).
	// First-match-wins was order-dependent: in monolithic ABPs, self-linked layer graphs
	// (and even states' nested bound graphs) are ALSO UAnimationGraph. Prefer the
	// top-level graph literally named "AnimGraph"; fall back to first top-level, then first any.
	UEdGraph* AnimGraph = nullptr;
	UEdGraph* FirstTopLevelAnimationGraph = nullptr;
	UEdGraph* FirstAnimationGraph = nullptr;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph || !Graph->IsA<UAnimationGraph>())
		{
			continue;
		}
		if (!FirstAnimationGraph)
		{
			FirstAnimationGraph = Graph;
		}
		UObject* GraphOuter = Graph->GetOuter();
		if (GraphOuter != AnimBP && GraphOuter != AnimBP->GeneratedClass)
		{
			continue;
		}
		if (!FirstTopLevelAnimationGraph)
		{
			FirstTopLevelAnimationGraph = Graph;
		}
		if (Graph->GetFName() == TEXT("AnimGraph"))
		{
			AnimGraph = Graph;
			break;
		}
	}
	if (!AnimGraph)
	{
		AnimGraph = FirstTopLevelAnimationGraph ? FirstTopLevelAnimationGraph : FirstAnimationGraph;
	}

	if (!AnimGraph)
	{
		Output += TEXT("WARNING: No AnimGraph found!\n");
		return Output;
	}

	Output += TEXT("=== AnimGraph (pose chain from Root) ===\n");

	// Find the Root node
	UAnimGraphNode_Root* RootNode = nullptr;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		RootNode = Cast<UAnimGraphNode_Root>(Node);
		if (RootNode)
		{
			break;
		}
	}

	if (!RootNode)
	{
		Output += TEXT("WARNING: No Root node found in AnimGraph!\n");
		return Output;
	}

	// Walk the pose chain from root
	TSet<UEdGraphNode*> Visited;
	int32 NodeBudget = 2000;
	WalkPoseChain(RootNode, 0, Output, Visited, NodeBudget);

	// Dump cached pose definitions not reached from the root walk
	// (SaveCachedPose nodes on parallel branches consumed via UseCachedPose references)
	DumpUnvisitedCachedPoses(AnimGraph, 0, Output, Visited);

	Output += TEXT("\n");

	// Animation Layers (from implemented interfaces)
	DumpAnimationLayers(AnimBP, Output);

	// Self-linked layer function graphs (monolithic AnimBPs)
	// These are UAnimationGraph instances defined in the AnimBP itself (not via separate ALI AnimBPs)
	// but not covered by ImplementedInterfaces — common in monolithic AnimBPs like AB_Als_Monolithic
	{
		TSet<UEdGraph*> DumpedGraphs;
		DumpedGraphs.Add(AnimGraph);

		// Collect graphs already dumped via interfaces
		for (const FBPInterfaceDescription& InterfaceDesc : AnimBP->ImplementedInterfaces)
		{
			for (UEdGraph* Graph : InterfaceDesc.Graphs)
			{
				DumpedGraphs.Add(Graph);
			}
		}

		bool bHeaderPrinted = false;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph || DumpedGraphs.Contains(Graph)) continue;

			// Only top-level graphs (not sub-graphs of states/transitions)
			UObject* Outer = Graph->GetOuter();
			if (Outer != AnimBP && Outer != AnimBP->GeneratedClass) continue;

			// Only animation graphs (not EventGraph or K2 function graphs)
			if (!Graph->IsA<UAnimationGraph>()) continue;

			if (!bHeaderPrinted)
			{
				Output += TEXT("=== Layer Functions (Self-Linked) ===\n");
				bHeaderPrinted = true;
			}

			Output += FString::Printf(TEXT("--- Layer: %s ---\n"), *Graph->GetName());

			TSet<UEdGraphNode*> LayerVisited;
			DumpStateContent(Graph, 1, Output, LayerVisited);
			DumpUnvisitedCachedPoses(Graph, 1, Output, LayerVisited);
			Output += TEXT("\n");
		}
	}

	// EventGraph & Functions (after AnimGraph)
	FBlueprintDumpUtils::DumpEventGraphs(AnimBP, Output);
	FBlueprintDumpUtils::DumpFunctions(AnimBP, Output);

	return Output;
}

// ============================================================================
// Pose chain walker — recursive backward walk
// ============================================================================

void FAnimBPDumper::WalkPoseChain(UEdGraphNode* Node, int32 Depth, FString& Output, TSet<UEdGraphNode*>& Visited, int32& NodeBudget)
{
	if (!Node)
	{
		return;
	}

	if (Visited.Contains(Node))
	{
		// Shared subtree (e.g. a blend's second input reaching an already-dumped node) —
		// silence here used to read as "unconnected"
		Output += Indent(Depth) + TEXT("(-> see above: ") + GetNodeLabel(Node) + TEXT(")\n");
		return;
	}
	Visited.Add(Node);

	// Node budget — insurance against pathological/generated graphs blowing the stack
	if (NodeBudget <= 0)
	{
		return;
	}
	if (--NodeBudget == 0)
	{
		Output += Indent(Depth) + TEXT("(truncated: node budget reached)\n");
		return;
	}

	// Print this node
	FString Label = GetNodeLabel(Node);
	if (!Node->IsNodeEnabled())
	{
		Label += TEXT(" [DISABLED]");
	}
	FString DataPins = FormatDataPins(Node);
	Output += Indent(Depth) + Label;
	if (!DataPins.IsEmpty())
	{
		Output += TEXT(" (") + DataPins + TEXT(")");
	}
	if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
	{
		Output += FormatAnimNodeSettings(AnimNode, Label);
		Output += FormatNodeFunctionBindings(AnimNode);
	}
	Output += TEXT("\n");

	// Check if this is a state machine node — dump its contents
	UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
	if (SMNode)
	{
		DumpStateMachine(SMNode, Depth + 1, Output, Visited);
		return; // SM handles its own children
	}

	// Nodes hosting their own sub-graphs (e.g. Blend Stack per-sample graphs) — recurse
	// so they stop reading as dead-end leaves. State machines are handled above; linked
	// layers are dumped in their own sections.
	if (!Cast<UAnimGraphNode_LinkedAnimLayer>(Node))
	{
		for (UEdGraph* SubGraph : Node->GetSubGraphs())
		{
			if (!SubGraph)
			{
				continue;
			}
			Output += Indent(Depth + 1) + TEXT("[SubGraph: ") + SubGraph->GetName() + TEXT("]\n");
			TSet<UEdGraphNode*> SubVisited;
			DumpStateContent(SubGraph, Depth + 2, Output, SubVisited);
		}
	}

	// Find all input pose link pins and follow them backward
	struct FPoseInput
	{
		FString PinLabel;
		UEdGraphNode* LinkedNode;
	};
	TArray<FPoseInput> PoseInputs;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction != EGPD_Input)
		{
			continue;
		}

		if (!IsPoseLinkPin(Pin))
		{
			continue;
		}

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				FPoseInput Input;
				Input.PinLabel = Pin->GetDisplayName().ToString();
				Input.LinkedNode = LinkedPin->GetOwningNode();
				PoseInputs.Add(Input);
			}
		}
	}

	// Walk each input
	bool bShowPinLabels = PoseInputs.Num() > 1;
	for (const FPoseInput& Input : PoseInputs)
	{
		if (bShowPinLabels && !Input.PinLabel.IsEmpty())
		{
			Output += Indent(Depth + 1) + TEXT("[") + Input.PinLabel + TEXT("]\n");
			WalkPoseChain(Input.LinkedNode, Depth + 1, Output, Visited, NodeBudget);
		}
		else
		{
			WalkPoseChain(Input.LinkedNode, Depth + 1, Output, Visited, NodeBudget);
		}
	}
}

// ============================================================================
// State machine dumper
// ============================================================================

void FAnimBPDumper::DumpStateMachine(UAnimGraphNode_StateMachine* SMNode, int32 Depth, FString& Output, TSet<UEdGraphNode*>& Visited)
{
	if (!SMNode)
	{
		return;
	}

	UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
	if (!SMGraph)
	{
		Output += Indent(Depth) + TEXT("(no state machine graph)\n");
		return;
	}

	FString SMName = SMNode->GetStateMachineName();
	Output += TEXT("\n") + Indent(Depth) + FString::Printf(TEXT("=== State Machine: %s ===\n"), *SMName);

	// Find entry state
	FString EntryStateName = FindEntryStateName(SMGraph);
	Output += Indent(Depth) + FString::Printf(TEXT("Entry -> %s\n"), *EntryStateName);

	// Collect states and transitions
	TArray<UAnimStateNode*> States;
	TArray<UAnimStateConduitNode*> Conduits;
	TArray<UAnimStateTransitionNode*> Transitions;

	for (UEdGraphNode* GraphNode : SMGraph->Nodes)
	{
		if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(GraphNode))
		{
			States.Add(StateNode);
		}
		else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(GraphNode))
		{
			Conduits.Add(ConduitNode);
		}
		else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(GraphNode))
		{
			Transitions.Add(TransNode);
		}
	}

	// List all states
	Output += Indent(Depth) + TEXT("States:");
	for (UAnimStateNode* State : States)
	{
		Output += TEXT(" ") + State->GetStateName() + TEXT(",");
	}
	Output.RemoveFromEnd(TEXT(","));
	Output += TEXT("\n");

	if (Conduits.Num() > 0)
	{
		Output += Indent(Depth) + TEXT("Conduits:");
		for (UAnimStateConduitNode* Conduit : Conduits)
		{
			Output += TEXT(" ") + Conduit->GetStateName() + TEXT(",");
		}
		Output.RemoveFromEnd(TEXT(","));
		Output += TEXT("\n");
	}

	// Gap 3: Collect state aliases (UE5 feature for multi-source transitions)
	// Uses exclusion-based detection — aliases inherit from UAnimStateNodeBase
	// but aren't states, conduits, transitions, or entry nodes
	TArray<UAnimStateNodeBase*> Aliases;
	for (UEdGraphNode* GraphNode : SMGraph->Nodes)
	{
		UAnimStateNodeBase* StateBase = Cast<UAnimStateNodeBase>(GraphNode);
		if (!StateBase) continue;
		if (Cast<UAnimStateNode>(GraphNode)) continue;
		if (Cast<UAnimStateConduitNode>(GraphNode)) continue;
		if (Cast<UAnimStateTransitionNode>(GraphNode)) continue;
		if (Cast<UAnimStateEntryNode>(GraphNode)) continue;
		// Remaining UAnimStateNodeBase subclasses = aliases (or future node types)
		Aliases.Add(StateBase);
	}

	if (Aliases.Num() > 0)
	{
		Output += Indent(Depth) + TEXT("State Aliases:");
		for (UAnimStateNodeBase* Alias : Aliases)
		{
			Output += TEXT(" ") + Alias->GetStateName() + TEXT(",");
		}
		Output.RemoveFromEnd(TEXT(","));
		Output += TEXT("\n");
	}

	// List transitions with rules (sorted by source state, then priority)
	if (Transitions.Num() > 0)
	{
		Transitions.Sort([](UAnimStateTransitionNode& A, UAnimStateTransitionNode& B)
		{
			FString AFrom = A.GetPreviousState() ? A.GetPreviousState()->GetStateName() : TEXT("");
			FString BFrom = B.GetPreviousState() ? B.GetPreviousState()->GetStateName() : TEXT("");
			if (AFrom != BFrom) return AFrom < BFrom;
			return A.PriorityOrder < B.PriorityOrder;
		});

		Output += Indent(Depth) + TEXT("Transitions:\n");
		for (UAnimStateTransitionNode* Trans : Transitions)
		{
			FString FromName = TEXT("?");
			FString ToName = TEXT("?");

			// GetPreviousState/GetNextState return UAnimStateNodeBase* which has GetStateName()
			if (UAnimStateNodeBase* PrevState = Trans->GetPreviousState())
			{
				FromName = PrevState->GetStateName();
			}
			if (UAnimStateNodeBase* NextState = Trans->GetNextState())
			{
				ToName = NextState->GetStateName();
			}

			// Get transition rule expression
			FString RuleExpr;
			UEdGraph* TransGraph = Trans->GetBoundGraph();
			if (TransGraph)
			{
				RuleExpr = BuildTransitionRuleExpression(TransGraph);
			}

			if (RuleExpr.IsEmpty())
			{
				// Distinguish a genuinely rule-less transition from a reconstruction
				// failure — "(automatic)" used to silently swallow parse failures
				RuleExpr = Trans->bAutomaticRuleBasedOnSequencePlayerInState
					? TEXT("(automatic)")
					: TEXT("(unresolved)");
			}

			// Additional transition info
			FString TransInfo;
			if (Trans->bAutomaticRuleBasedOnSequencePlayerInState)
			{
				TransInfo += TEXT(" [AutoRule]");
			}

			// Blend logic type
			FString BlendType;
			switch (Trans->LogicType)
			{
			case ETransitionLogicType::TLT_StandardBlend:
				BlendType = TEXT("Std");
				break;
			case ETransitionLogicType::TLT_Inertialization:
				BlendType = TEXT("Inert");
				break;
			case ETransitionLogicType::TLT_Custom:
				BlendType = TEXT("Custom");
				break;
			default:
				BlendType = TEXT("?");
				break;
			}

			if (Trans->CrossfadeDuration > 0.0f)
			{
				TransInfo += FString::Printf(TEXT(" [%s %.2fs]"), *BlendType, Trans->CrossfadeDuration);
			}
			else
			{
				TransInfo += FString::Printf(TEXT(" [%s]"), *BlendType);
			}

			// Priority (lower = evaluated first when multiple transitions from same state are true)
			TransInfo += FString::Printf(TEXT(" P%d"), Trans->PriorityOrder);

			if (!Trans->IsNodeEnabled())
			{
				TransInfo += TEXT(" [DISABLED]");
			}

			Output += Indent(Depth + 1) + FString::Printf(TEXT("%s -> %s: %s%s\n"),
				*FromName, *ToName, *RuleExpr, *TransInfo);
		}
	}

	Output += TEXT("\n");

	// Dump each state's content
	for (UAnimStateNode* State : States)
	{
		Output += Indent(Depth) + FString::Printf(TEXT("--- State: %s ---\n"), *State->GetStateName());

		UEdGraph* StateGraph = State->GetBoundGraph();
		if (StateGraph)
		{
			// Create a new visited set for state content (allows re-visiting nodes in different states)
			TSet<UEdGraphNode*> StateVisited;
			DumpStateContent(StateGraph, Depth + 1, Output, StateVisited);
		}
		else
		{
			Output += Indent(Depth + 1) + TEXT("(empty state)\n");
		}
		Output += TEXT("\n");
	}

	// Dump conduit content
	for (UAnimStateConduitNode* Conduit : Conduits)
	{
		Output += Indent(Depth) + FString::Printf(TEXT("--- Conduit: %s ---\n"), *Conduit->GetStateName());

		UEdGraph* ConduitGraph = Conduit->GetBoundGraph();
		if (ConduitGraph)
		{
			FString ConduitRule = BuildTransitionRuleExpression(ConduitGraph);
			if (!ConduitRule.IsEmpty())
			{
				Output += Indent(Depth + 1) + TEXT("Rule: ") + ConduitRule + TEXT("\n");
			}
		}
		Output += TEXT("\n");
	}
}

// ============================================================================
// State content dumper
// ============================================================================

void FAnimBPDumper::DumpStateContent(UEdGraph* StateGraph, int32 Depth, FString& Output, TSet<UEdGraphNode*>& Visited)
{
	if (!StateGraph)
	{
		return;
	}

	// Find the state result node (Output Animation Pose)
	UEdGraphNode* ResultNode = nullptr;
	for (UEdGraphNode* Node : StateGraph->Nodes)
	{
		if (Cast<UAnimGraphNode_StateResult>(Node))
		{
			ResultNode = Node;
			break;
		}
	}

	// Fallback: try Root node (some graphs use this instead)
	if (!ResultNode)
	{
		for (UEdGraphNode* Node : StateGraph->Nodes)
		{
			if (Cast<UAnimGraphNode_Root>(Node))
			{
				ResultNode = Node;
				break;
			}
		}
	}

	if (ResultNode)
	{
		int32 NodeBudget = 2000;
		WalkPoseChain(ResultNode, Depth, Output, Visited, NodeBudget);
	}
	else
	{
		Output += Indent(Depth) + TEXT("(no result node found)\n");
	}
}

// ============================================================================
// Transition rule expression builder
// ============================================================================

FString FAnimBPDumper::BuildTransitionRuleExpression(UEdGraph* TransitionGraph)
{
	if (!TransitionGraph)
	{
		return FString();
	}

	// Find the TransitionResult node
	UAnimGraphNode_TransitionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : TransitionGraph->Nodes)
	{
		ResultNode = Cast<UAnimGraphNode_TransitionResult>(Node);
		if (ResultNode)
		{
			break;
		}
	}

	if (!ResultNode)
	{
		return FString();
	}

	// Find the bool input pin on the result node (skip exec and self pins)
	for (UEdGraphPin* Pin : ResultNode->Pins)
	{
		if (Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("self"))
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				return FBlueprintDumpUtils::BuildExpressionFromPin(Pin->LinkedTo[0]);
			}
			if (!Pin->DefaultValue.IsEmpty())
			{
				return Pin->DefaultValue;
			}
		}
	}

	return FString();
}

// ============================================================================
// Helper: Check if pin is a pose link
// ============================================================================

bool FAnimBPDumper::IsPoseLinkPin(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return false;
	}

	// Check via inheritance — catches FPoseLink, FComponentSpacePoseLink, and any future derivatives
	UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
	if (!Struct)
	{
		return false;
	}

	static UScriptStruct* PoseLinkBase = FPoseLinkBase::StaticStruct();
	return Struct->IsChildOf(PoseLinkBase);
}

// ============================================================================
// Helper: Get human-readable node label
// ============================================================================

FString FAnimBPDumper::GetNodeLabel(UEdGraphNode* Node)
{
	if (!Node)
	{
		return TEXT("(null)");
	}

	// Root node
	if (Cast<UAnimGraphNode_Root>(Node))
	{
		return TEXT("Output Pose (Root)");
	}

	// State machine
	if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
	{
		return FString::Printf(TEXT("State Machine: %s"), *SM->GetStateMachineName());
	}

	// Save cached pose
	if (UAnimGraphNode_SaveCachedPose* SaveCache = Cast<UAnimGraphNode_SaveCachedPose>(Node))
	{
		return FString::Printf(TEXT("Save Cached Pose: %s"), *SaveCache->CacheName);
	}

	// Use cached pose — use GetNodeTitle which includes the cache name (same as ue-llm-toolkit)
	if (UAnimGraphNode_UseCachedPose* UseCache = Cast<UAnimGraphNode_UseCachedPose>(Node))
	{
		FString LinkName = UseCache->GetNodeTitle(ENodeTitleType::ListView).ToString();
		return FString::Printf(TEXT("Use Cached Pose: %s"), *LinkName);
	}

	// State result node (output pose inside a state)
	if (Cast<UAnimGraphNode_StateResult>(Node))
	{
		return TEXT("State Result");
	}

	// Linked input pose (sub-graph input)
	if (UAnimGraphNode_LinkedInputPose* LinkedInput = Cast<UAnimGraphNode_LinkedInputPose>(Node))
	{
		return FString::Printf(TEXT("Linked Input Pose: %s"), *LinkedInput->GetNodeTitle(ENodeTitleType::ListView).ToString());
	}

	// Linked anim layer — Interface is on the inner FAnimNode_LinkedAnimLayer (Node member)
	if (UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(Node))
	{
		FString LayerName = LayerNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
		LayerName.ReplaceInline(TEXT("\n"), TEXT(" "));
		if (UClass* InterfaceClass = LayerNode->Node.Interface.Get())
		{
			return FString::Printf(TEXT("Linked Anim Layer: %s (Interface: %s)"),
				*LayerName, *InterfaceClass->GetName());
		}
		return FString::Printf(TEXT("Linked Anim Layer: %s"), *LayerName);
	}

	// Default: use the node's built-in title (includes asset names, slot names, etc.)
	FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Title.ReplaceInline(TEXT("\n"), TEXT(" "));
	if (!Title.IsEmpty())
	{
		return Title;
	}

	// Absolute fallback
	return Node->GetClass()->GetName();
}

// ============================================================================
// Helper: Format data pins (non-pose inputs with values or connections)
// ============================================================================

FString FAnimBPDumper::FormatDataPins(UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}

	TArray<FString> PinStrings;

	// Pins bound via the "Bind" dropdown carry their real source in the node's binding
	// object, not in DefaultValue — dumping the stale default would assert the opposite
	// of runtime truth. Collect bindings first so bound pins print the binding instead.
	TMap<FName, FString> PropertyBindings;
	CollectPropertyBindings(Node, PropertyBindings);

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction != EGPD_Input)
		{
			continue;
		}

		// Skip hidden pins, pose links, exec pins, self pin
		if (Pin->bHidden)
		{
			continue;
		}
		if (IsPoseLinkPin(Pin))
		{
			continue;
		}
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		if (Pin->PinName == TEXT("self"))
		{
			continue;
		}

		FString PinName = Pin->GetDisplayName().ToString();
		if (PinName.IsEmpty())
		{
			PinName = FBlueprintDumpUtils::FriendlyPinName(Pin);
		}

		// Bound pin — the binding is the runtime truth, not the pin's default
		if (const FString* BoundPath = PropertyBindings.Find(Pin->PinName))
		{
			PinStrings.Add(FString::Printf(TEXT("%s=[bound: %s]"), *PinName, **BoundPath));
			PropertyBindings.Remove(Pin->PinName);
			continue;
		}

		if (Pin->LinkedTo.Num() > 0)
		{
			PinStrings.Add(FString::Printf(TEXT("%s=%s"),
				*PinName, *FBlueprintDumpUtils::ResolvePinSourceLabel(Pin->LinkedTo[0])));
		}
		else if (!Pin->DefaultValue.IsEmpty() && Pin->DefaultValue != Pin->AutogeneratedDefaultValue)
		{
			const FString Literal = FBlueprintDumpUtils::FormatLiteralPinValue(Pin);
			PinStrings.Add(FString::Printf(TEXT("%s=%s"),
				*PinName, Literal.IsEmpty() ? *Pin->DefaultValue : *Literal));
		}
	}

	// Bindings whose pin is hidden (or absent) still drive the node at runtime — print them too
	for (const TPair<FName, FString>& Binding : PropertyBindings)
	{
		PinStrings.Add(FString::Printf(TEXT("%s=[bound: %s]"), *Binding.Key.ToString(), *Binding.Value));
	}

	return FString::Join(PinStrings, TEXT(", "));
}

// ============================================================================
// Helper: Find entry state name from SM graph
// ============================================================================

FString FAnimBPDumper::FindEntryStateName(UEdGraph* StateMachineGraph)
{
	if (!StateMachineGraph)
	{
		return TEXT("?");
	}

	// Find the entry node and follow its output to the first state
	for (UEdGraphNode* Node : StateMachineGraph->Nodes)
	{
		if (UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(Node))
		{
			// The entry node has an output pin connected to the default state
			for (UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
				{
					UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
					UEdGraphNode* FirstState = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (UAnimStateNodeBase* StateBase = Cast<UAnimStateNodeBase>(FirstState))
					{
						return StateBase->GetStateName();
					}
				}
			}
		}
	}

	return TEXT("?");
}

// ============================================================================
// Animation layers dumper
// ============================================================================

void FAnimBPDumper::DumpAnimationLayers(UAnimBlueprint* AnimBP, FString& Output)
{
	if (!AnimBP)
	{
		return;
	}

	// Look for Animation Layer Interfaces (ALI) in implemented interfaces
	bool bHasLayers = false;
	for (const FBPInterfaceDescription& InterfaceDesc : AnimBP->ImplementedInterfaces)
	{
		if (!InterfaceDesc.Interface)
		{
			continue;
		}

		// Check if this interface has any graphs (layer functions)
		if (InterfaceDesc.Graphs.Num() == 0)
		{
			continue;
		}

		if (!bHasLayers)
		{
			Output += TEXT("=== Animation Layers ===\n");
			bHasLayers = true;
		}

		Output += FString::Printf(TEXT("--- Interface: %s ---\n"), *InterfaceDesc.Interface->GetName());

		for (UEdGraph* LayerGraph : InterfaceDesc.Graphs)
		{
			if (!LayerGraph)
			{
				continue;
			}

			Output += FString::Printf(TEXT("  Layer: %s\n"), *LayerGraph->GetName());

			// Find root/result node and walk pose chain
			UEdGraphNode* ResultNode = nullptr;
			for (UEdGraphNode* Node : LayerGraph->Nodes)
			{
				if (Cast<UAnimGraphNode_Root>(Node) || Cast<UAnimGraphNode_StateResult>(Node))
				{
					ResultNode = Node;
					break;
				}
			}

			if (ResultNode)
			{
				TSet<UEdGraphNode*> LayerVisited;
				int32 NodeBudget = 2000;
				WalkPoseChain(ResultNode, 2, Output, LayerVisited, NodeBudget);
				DumpUnvisitedCachedPoses(LayerGraph, 2, Output, LayerVisited);
			}
			else
			{
				Output += TEXT("    (empty layer)\n");
			}
		}

		Output += TEXT("\n");
	}
}

// ============================================================================
// Cached pose chain dumper
// ============================================================================

void FAnimBPDumper::DumpUnvisitedCachedPoses(UEdGraph* Graph, int32 BaseDepth, FString& Output, TSet<UEdGraphNode*>& Visited)
{
	if (!Graph)
	{
		return;
	}

	// Find SaveCachedPose nodes that weren't visited during the main walk
	// These define cached poses consumed via UseCachedPose references in states/sub-graphs
	TArray<UAnimGraphNode_SaveCachedPose*> UnvisitedCaches;
	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		if (UAnimGraphNode_SaveCachedPose* SaveCache = Cast<UAnimGraphNode_SaveCachedPose>(GraphNode))
		{
			if (!Visited.Contains(SaveCache))
			{
				UnvisitedCaches.Add(SaveCache);
			}
		}
	}

	for (UAnimGraphNode_SaveCachedPose* SaveCache : UnvisitedCaches)
	{
		TSet<UEdGraphNode*> CacheVisited;
		int32 NodeBudget = 2000;
		WalkPoseChain(SaveCache, BaseDepth, Output, CacheVisited, NodeBudget);
	}
}

// ============================================================================
// Indent helper
// ============================================================================

FString FAnimBPDumper::Indent(int32 Depth)
{
	if (Depth <= 0)
	{
		return FString();
	}

	// Use tree-style indentation
	FString Result;
	for (int32 i = 0; i < Depth - 1; ++i)
	{
		Result += TEXT("  ");
	}
	Result += TEXT("  \\-- ");
	return Result;
}
