// Copyright (c) 2026 knight-scripts. MIT License.

#include "BlueprintDumpUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Knot.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_SwitchEnum.h"

// ============================================================================
// Pin helpers
// ============================================================================

bool FBlueprintDumpUtils::IsExecPin(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return false;
	}
	return Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
}

TArray<UEdGraphPin*> FBlueprintDumpUtils::GetExecOutputPins(UEdGraphNode* Node)
{
	TArray<UEdGraphPin*> ExecOuts;
	if (!Node)
	{
		return ExecOuts;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == EGPD_Output && IsExecPin(Pin) && !Pin->bHidden)
		{
			ExecOuts.Add(Pin);
		}
	}
	return ExecOuts;
}

FString FBlueprintDumpUtils::PinTypeToString(const FEdGraphPinType& PinType)
{
	const FName& Category = PinType.PinCategory;

	if (Category == UEdGraphSchema_K2::PC_Boolean)
	{
		return TEXT("bool");
	}
	if (Category == UEdGraphSchema_K2::PC_Int)
	{
		return TEXT("int32");
	}
	if (Category == UEdGraphSchema_K2::PC_Int64)
	{
		return TEXT("int64");
	}
	if (Category == UEdGraphSchema_K2::PC_Real)
	{
		// Double vs float via subcategory
		if (PinType.PinSubCategory == TEXT("double"))
		{
			return TEXT("double");
		}
		return TEXT("float");
	}
	if (Category == UEdGraphSchema_K2::PC_Name)
	{
		return TEXT("FName");
	}
	if (Category == UEdGraphSchema_K2::PC_String)
	{
		return TEXT("FString");
	}
	if (Category == UEdGraphSchema_K2::PC_Text)
	{
		return TEXT("FText");
	}
	if (Category == UEdGraphSchema_K2::PC_Byte)
	{
		// If it has an enum subobject, use its name
		if (UEnum* Enum = Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
		{
			return Enum->GetName();
		}
		return TEXT("byte");
	}
	if (Category == UEdGraphSchema_K2::PC_Enum)
	{
		if (UEnum* Enum = Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
		{
			return Enum->GetName();
		}
		return TEXT("enum");
	}
	if (Category == UEdGraphSchema_K2::PC_Struct)
	{
		if (UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get()))
		{
			return Struct->GetName();
		}
		return TEXT("struct");
	}
	if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_SoftObject)
	{
		if (UClass* Class = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
		{
			return Class->GetName() + TEXT("*");
		}
		return TEXT("UObject*");
	}
	if (Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftClass)
	{
		if (UClass* Class = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
		{
			return TEXT("TSubclassOf<") + Class->GetName() + TEXT(">");
		}
		return TEXT("UClass*");
	}
	if (Category == UEdGraphSchema_K2::PC_Interface)
	{
		if (UClass* Class = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
		{
			return TEXT("TScriptInterface<") + Class->GetName() + TEXT(">");
		}
		return TEXT("interface");
	}
	if (Category == UEdGraphSchema_K2::PC_Wildcard)
	{
		return TEXT("wildcard");
	}
	if (Category == UEdGraphSchema_K2::PC_Exec)
	{
		return TEXT("exec");
	}

	return Category.ToString();
}

// ============================================================================
// K2 node label
// ============================================================================

FString FBlueprintDumpUtils::GetK2NodeLabel(UEdGraphNode* Node)
{
	if (!Node)
	{
		return TEXT("(null)");
	}

	// Event node (BlueprintUpdateAnimation, BeginPlay, etc.)
	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		FName EventName = EventNode->EventReference.GetMemberName();
		if (EventName.IsNone())
		{
			// Fallback to node title
			return TEXT("Event: ") + EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}
		return FString::Printf(TEXT("Event: %s"), *EventName.ToString());
	}

	// Custom event
	if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
	{
		return FString::Printf(TEXT("CustomEvent: %s"), *CustomEvent->CustomFunctionName.ToString());
	}

	// Function entry
	if (Cast<UK2Node_FunctionEntry>(Node))
	{
		return TEXT("FunctionEntry");
	}

	// Function result / return
	if (Cast<UK2Node_FunctionResult>(Node))
	{
		return TEXT("Return");
	}

	// Variable set
	if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
	{
		return FString::Printf(TEXT("Set %s"), *VarSet->GetVarName().ToString());
	}

	// Variable get
	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
	{
		return FString::Printf(TEXT("Get %s"), *VarGet->GetVarName().ToString());
	}

	// Function call
	if (UK2Node_CallFunction* FuncCall = Cast<UK2Node_CallFunction>(Node))
	{
		FName MemberName = FuncCall->FunctionReference.GetMemberName();
		return FString::Printf(TEXT("Call %s()"), *MemberName.ToString());
	}

	// Branch (if/then/else)
	if (Cast<UK2Node_IfThenElse>(Node))
	{
		return TEXT("Branch");
	}

	// Dynamic cast
	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		if (CastNode->TargetType)
		{
			return FString::Printf(TEXT("Cast to %s"), *CastNode->TargetType->GetName());
		}
		return TEXT("Cast");
	}

	// Macro instance
	if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		FString MacroName = MacroNode->GetMacroGraph() ? MacroNode->GetMacroGraph()->GetName() : TEXT("Unknown");
		return FString::Printf(TEXT("Macro: %s"), *MacroName);
	}

	// Sequence
	if (Cast<UK2Node_ExecutionSequence>(Node))
	{
		return TEXT("Sequence");
	}

	// Fallback — use node title
	FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Title.ReplaceInline(TEXT("\n"), TEXT(" "));
	if (!Title.IsEmpty())
	{
		return Title;
	}

	return Node->GetClass()->GetName();
}

// ============================================================================
// Expression resolution helpers (shared with AnimBPDumper)
// ============================================================================

FString FBlueprintDumpUtils::ResolveOperatorSymbol(const FString& FuncName)
{
	static TMap<FString, FString> OpMap;
	if (OpMap.Num() == 0)
	{
		OpMap.Add(TEXT("Greater"), TEXT(">"));
		OpMap.Add(TEXT("Less"), TEXT("<"));
		OpMap.Add(TEXT("Equal"), TEXT("=="));
		OpMap.Add(TEXT("EqualEqual"), TEXT("=="));
		OpMap.Add(TEXT("GreaterEqual"), TEXT(">="));
		OpMap.Add(TEXT("LessEqual"), TEXT("<="));
		OpMap.Add(TEXT("NotEqual"), TEXT("!="));
		OpMap.Add(TEXT("BooleanAND"), TEXT("&&"));
		OpMap.Add(TEXT("BooleanOR"), TEXT("||"));
		OpMap.Add(TEXT("Add"), TEXT("+"));
		OpMap.Add(TEXT("Subtract"), TEXT("-"));
		OpMap.Add(TEXT("Multiply"), TEXT("*"));
		OpMap.Add(TEXT("Divide"), TEXT("/"));
	}

	// Direct lookup
	if (const FString* Symbol = OpMap.Find(FuncName))
	{
		return *Symbol;
	}

	// Strip type suffix (e.g., "Greater_FloatFloat" -> "Greater")
	int32 UnderscoreIdx;
	if (FuncName.FindChar(TEXT('_'), UnderscoreIdx))
	{
		FString Base = FuncName.Left(UnderscoreIdx);
		if (const FString* Symbol = OpMap.Find(Base))
		{
			return *Symbol;
		}
	}

	return FString();
}

FString FBlueprintDumpUtils::ResolveEnumPinValue(const UEdGraphPin* Pin)
{
	if (!Pin || Pin->DefaultValue.IsEmpty())
	{
		return FString();
	}

	// Check if pin has an enum type
	if (UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
	{
		// DefaultValue might be a byte index ("0", "1") — convert to name
		if (Pin->DefaultValue.IsNumeric())
		{
			int64 EnumValue = FCString::Atoi64(*Pin->DefaultValue);
			FString EnumName = Enum->GetNameStringByValue(EnumValue);

			// Gap 2 fix: BP enums return "NewEnumerator0" etc. — try display name
			if (!EnumName.IsEmpty() && !EnumName.StartsWith(TEXT("NewEnumerator")))
			{
				return EnumName;
			}
			FText DisplayName = Enum->GetDisplayNameTextByValue(EnumValue);
			if (!DisplayName.IsEmpty())
			{
				return DisplayName.ToString();
			}
			if (!EnumName.IsEmpty())
			{
				return EnumName; // Return internal name if display name also empty
			}
		}
		// Handle name-string values that are internal BP enum names
		// (e.g., "NewEnumerator4" or "E_Foo::NewEnumerator4")
		if (Pin->DefaultValue.Contains(TEXT("NewEnumerator")))
		{
			int64 EnumValue = Enum->GetValueByNameString(Pin->DefaultValue);
			if (EnumValue != INDEX_NONE)
			{
				FText DisplayName = Enum->GetDisplayNameTextByValue(EnumValue);
				if (!DisplayName.IsEmpty())
				{
					return DisplayName.ToString();
				}
			}
		}
		// Already a name string (e.g., "ELocomotionPhase::Start") — return as-is
	}

	return Pin->DefaultValue;
}

FString FBlueprintDumpUtils::ResolveOperatorFromTitle(const FString& Title)
{
	FString FirstWord = Title;
	int32 SpaceIdx;
	if (FirstWord.FindChar(TEXT(' '), SpaceIdx))
	{
		FirstWord = FirstWord.Left(SpaceIdx);
	}
	return ResolveOperatorSymbol(FirstWord);
}

FString FBlueprintDumpUtils::BuildExpressionFromPin(UEdGraphPin* Pin, int32 MaxDepth, int32 CurrentDepth)
{
	if (!Pin)
	{
		return TEXT("???");
	}

	UEdGraphNode* Node = Pin->GetOwningNode();
	if (!Node)
	{
		return TEXT("???");
	}

	// Depth guard — return node title as fallback at max depth.
	// This is the ONLY recursion bound: valid BP data graphs cannot contain cycles
	// (the compiler rejects them), and one Get node routinely feeds multiple
	// consumers — a visited set here produced only false "(cycle)" markers.
	if (CurrentDepth >= MaxDepth)
	{
		FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Title.ReplaceInline(TEXT("\n"), TEXT(" "));
		return Title.IsEmpty() ? Node->GetName() : Title;
	}

	// Wrap nested operator results so precedence survives reconstruction:
	// AND(bA, OR(bB, bC)) must print "bA && (bB || bC)", not "bA && bB || bC".
	auto Grouped = [CurrentDepth](const FString& Expr) -> FString
	{
		return CurrentDepth > 0 ? TEXT("(") + Expr + TEXT(")") : Expr;
	};

	// Reroute nodes (Knots) — follow through transparently
	if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
	{
		for (UEdGraphPin* KnotPin : Knot->Pins)
		{
			if (KnotPin->Direction == EGPD_Input && KnotPin->LinkedTo.Num() > 0)
			{
				return BuildExpressionFromPin(KnotPin->LinkedTo[0], MaxDepth, CurrentDepth + 1);
			}
		}
		return TEXT("???");
	}

	// Property Access nodes — extract the property path from FullTitle or pin name
	{
		FString ListTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		if (ListTitle == TEXT("Property Access") || ListTitle.StartsWith(TEXT("Property Access")))
		{
			// FullTitle often has path on second line: "Property Access\nbHasVelocity"
			FString FullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx;
			if (FullTitle.FindChar(TEXT('\n'), NewlineIdx))
			{
				FString Path = FullTitle.Mid(NewlineIdx + 1).TrimStartAndEnd();
				if (!Path.IsEmpty())
				{
					return Path;
				}
			}

			// Try the output pin we were given — often named after the property
			if (!Pin->PinName.IsNone())
			{
				FString PinNameStr = Pin->PinName.ToString();
				if (PinNameStr != TEXT("Output") && PinNameStr != TEXT("Result") && PinNameStr != TEXT("ReturnValue"))
				{
					return PinNameStr;
				}
			}

			// Return whatever title we have (may still be "Property Access")
			return ListTitle;
		}
	}

	// Variable Get node — return the variable name
	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
	{
		return VarGet->GetVarName().ToString();
	}

	// Function call node — could be an operator or a function
	if (UK2Node_CallFunction* FuncCall = Cast<UK2Node_CallFunction>(Node))
	{
		FName MemberName = FuncCall->FunctionReference.GetMemberName();
		FString FuncNameStr = MemberName.ToString();

		// Unary NOT — special case
		if (FuncNameStr == TEXT("Not_PreBool"))
		{
			for (UEdGraphPin* InputPin : Node->Pins)
			{
				if (InputPin->Direction == EGPD_Input
					&& InputPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
					&& InputPin->PinName != TEXT("self"))
				{
					if (InputPin->LinkedTo.Num() > 0)
					{
						return Grouped(FString::Printf(TEXT("NOT %s"),
							*BuildExpressionFromPin(InputPin->LinkedTo[0], MaxDepth, CurrentDepth + 1)));
					}
				}
			}
			return TEXT("NOT ???");
		}

		// Binary operator — use operator map with type suffix stripping
		FString OpSymbol = ResolveOperatorSymbol(FuncNameStr);
		if (!OpSymbol.IsEmpty())
		{
			TArray<FString> Operands;
			for (UEdGraphPin* InputPin : Node->Pins)
			{
				if (InputPin->Direction == EGPD_Input
					&& InputPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
					&& InputPin->PinName != TEXT("self")
					&& InputPin->PinName != TEXT("ErrorTolerance"))
				{
					if (InputPin->LinkedTo.Num() > 0)
					{
						Operands.Add(BuildExpressionFromPin(InputPin->LinkedTo[0], MaxDepth, CurrentDepth + 1));
					}
					else
					{
						// Try enum resolution first, then raw default value
						FString Value = ResolveEnumPinValue(InputPin);
						if (!Value.IsEmpty())
						{
							Operands.Add(Value);
						}
						else
						{
							FString Cat = InputPin->PinType.PinCategory.ToString();
							Operands.Add(Cat == TEXT("bool") ? TEXT("false") : TEXT("0"));
						}
					}
				}
			}

			if (Operands.Num() >= 2)
			{
				return Grouped(FString::Printf(TEXT("%s %s %s"), *Operands[0], *OpSymbol, *Operands[1]));
			}
			if (Operands.Num() == 1)
			{
				// Missing operand is a parse gap — say so instead of inventing a value
				return Grouped(FString::Printf(TEXT("%s %s ?"), *Operands[0], *OpSymbol));
			}
			return OpSymbol;
		}

		// Not a recognized operator — treat as a function call
		return FuncNameStr;
	}

	// Generic fallback — try to extract expression from input pins
	// Handles custom comparison nodes, enum equality, etc. that aren't UK2Node_CallFunction
	{
		TArray<FString> Operands;
		for (UEdGraphPin* InputPin : Node->Pins)
		{
			if (InputPin->Direction != EGPD_Input) continue;
			if (InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (InputPin->PinName == TEXT("self")) continue;

			if (InputPin->LinkedTo.Num() > 0)
			{
				Operands.Add(BuildExpressionFromPin(InputPin->LinkedTo[0], MaxDepth, CurrentDepth + 1));
			}
			else
			{
				FString Value = ResolveEnumPinValue(InputPin);
				if (!Value.IsEmpty())
				{
					Operands.Add(Value);
				}
			}
		}

		FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		NodeTitle.ReplaceInline(TEXT("\n"), TEXT(" "));

		// Try to resolve operator from title (e.g., "Equal (Enum)" -> "==")
		FString OpSymbol = ResolveOperatorFromTitle(NodeTitle);
		if (!OpSymbol.IsEmpty() && Operands.Num() >= 2)
		{
			return Grouped(FString::Printf(TEXT("%s %s %s"), *Operands[0], *OpSymbol, *Operands[1]));
		}

		// Not an operator — format with inputs if available
		if (Operands.Num() > 0)
		{
			return FString::Printf(TEXT("%s(%s)"), *NodeTitle, *FString::Join(Operands, TEXT(", ")));
		}

		return NodeTitle.IsEmpty() ? Node->GetName() : NodeTitle;
	}
}

// ============================================================================
// Asset path normalization
// ============================================================================

FString FBlueprintDumpUtils::NormalizeAssetPath(const FString& InputPath)
{
	FString Path = InputPath;

	// Strip /All prefix (Content Browser display artifact)
	if (Path.StartsWith(TEXT("/All/")))
	{
		Path = Path.Mid(4); // "/All/Foo/Bar" -> "/Foo/Bar"
	}

	// Strip /Plugins/{PluginName} prefix → keep internal mount point
	// "/Plugins/ALS/ALS/Character/ABP" → "/ALS/Character/ABP"
	if (Path.StartsWith(TEXT("/Plugins/")))
	{
		// Format: /Plugins/{PluginName}/{MountPoint}/...
		// The first segment after /Plugins/ is the plugin name, second+ is the content path
		FString Rest = Path.Mid(9); // strip "/Plugins/"
		int32 SlashIdx;
		if (Rest.FindChar(TEXT('/'), SlashIdx))
		{
			Path = Rest.Mid(SlashIdx); // "/ALS/Character/ABP"
		}
	}

	// Strip trailing ".AssetName" if present (already there from Content Browser)
	// e.g., "/ALS/Character/AB_Als.AB_Als" → "/ALS/Character/AB_Als"
	int32 DotIdx;
	if (Path.FindLastChar(TEXT('.'), DotIdx))
	{
		FString MaybeDuplicate = Path.Mid(DotIdx + 1);
		FString BaseName = FPaths::GetBaseFilename(Path.Left(DotIdx));
		if (MaybeDuplicate == BaseName)
		{
			Path = Path.Left(DotIdx);
		}
	}

	return Path;
}

// ============================================================================
// Data input summary
// ============================================================================

FString FBlueprintDumpUtils::GetDataInputSummary(UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}

	TArray<FString> Parts;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction != EGPD_Input)
		{
			continue;
		}
		if (IsExecPin(Pin) || Pin->bHidden || Pin->PinName == TEXT("self"))
		{
			continue;
		}

		FString PinName = Pin->GetDisplayName().ToString();
		if (PinName.IsEmpty())
		{
			PinName = Pin->PinName.ToString();
		}

		if (Pin->LinkedTo.Num() > 0)
		{
			// Connected — show source (element null-guarded: stale link entries exist in corrupt assets)
			UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
			UEdGraphNode* SourceNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
			FString SourceLabel;

			if (!SourceNode)
			{
				SourceLabel = TEXT("???");
			}
			else if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(SourceNode))
			{
				SourceLabel = VarGet->GetVarName().ToString();
			}
			else if (UK2Node_CallFunction* FuncCall = Cast<UK2Node_CallFunction>(SourceNode))
			{
				SourceLabel = FuncCall->FunctionReference.GetMemberName().ToString() + TEXT("()");
			}
			else
			{
				// Gap 1: Recursive resolution for pure nodes (operators, struct breaks, selects, etc.)
				SourceLabel = BuildExpressionFromPin(LinkedPin, /*MaxDepth=*/4);
			}

			Parts.Add(FString::Printf(TEXT("%s=%s"), *PinName, *SourceLabel));
		}
		else if (!Pin->DefaultValue.IsEmpty() && Pin->DefaultValue != Pin->AutogeneratedDefaultValue)
		{
			// Gap 2: Use shared enum resolution with display name fallback
			if (Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
			{
				FString EnumName = ResolveEnumPinValue(Pin);
				if (!EnumName.IsEmpty() && EnumName != Pin->DefaultValue)
				{
					Parts.Add(FString::Printf(TEXT("%s=%s"), *PinName, *EnumName));
					continue;
				}
			}
			Parts.Add(FString::Printf(TEXT("%s=%s"), *PinName, *Pin->DefaultValue));
		}
	}

	if (Parts.Num() == 0)
	{
		return FString();
	}

	return FString::Join(Parts, TEXT(", "));
}

// ============================================================================
// Core: exec chain walker
// ============================================================================

void FBlueprintDumpUtils::WalkExecChain(UEdGraphNode* Node, int32 Depth, FString& Output, TSet<UEdGraphNode*>& Visited, int32& NodeBudget)
{
	if (!Node)
	{
		return;
	}

	// Cycle detection — BEFORE knot handling: an exec reroute loop (knot->knot cycle,
	// wireable in the editor) previously bypassed this check and recursed to stack overflow
	if (Visited.Contains(Node))
	{
		Output += Indent(Depth) + TEXT("(cycle -> ") + GetK2NodeLabel(Node) + TEXT(")\n");
		return;
	}
	Visited.Add(Node);

	// Node budget — insurance against macro-heavy/generated graphs blowing the stack
	// (linear chains recurse one frame per node). Never triggers on healthy assets.
	if (NodeBudget <= 0)
	{
		return;
	}
	if (--NodeBudget == 0)
	{
		Output += Indent(Depth) + TEXT("(truncated: node budget reached)\n");
		return;
	}

	// Follow through Knot (reroute) nodes transparently
	if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
	{
		TArray<UEdGraphPin*> ExecOuts = GetExecOutputPins(Knot);
		for (UEdGraphPin* OutPin : ExecOuts)
		{
			for (UEdGraphPin* LinkedPin : OutPin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					WalkExecChain(LinkedPin->GetOwningNode(), Depth, Output, Visited, NodeBudget);
				}
			}
		}
		return;
	}

	// Print this node
	FString Label = GetK2NodeLabel(Node);
	FString DataSummary = GetDataInputSummary(Node);

	// For variable sets, show what value is being assigned
	if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
	{
		// Find the value input pin (not exec, not self, not the variable output)
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && !IsExecPin(Pin) && Pin->PinName != TEXT("self"))
			{
				if (Pin->LinkedTo.Num() > 0)
				{
					UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
					UEdGraphNode* SourceNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					FString SourceLabel;
					if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(SourceNode))
					{
						SourceLabel = VarGet->GetVarName().ToString();
					}
					else if (UK2Node_CallFunction* FuncCall = Cast<UK2Node_CallFunction>(SourceNode))
					{
						SourceLabel = FuncCall->FunctionReference.GetMemberName().ToString() + TEXT("()");
					}
					else if (SourceNode)
					{
						SourceLabel = SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
						SourceLabel.ReplaceInline(TEXT("\n"), TEXT(" "));
					}
					else
					{
						SourceLabel = TEXT("???");
					}
					Label += TEXT(" = ") + SourceLabel;
				}
				else if (!Pin->DefaultValue.IsEmpty())
				{
					Label += TEXT(" = ") + ResolveEnumPinValue(Pin);
				}
				break;
			}
		}
	}
	else if (!DataSummary.IsEmpty())
	{
		Label += TEXT(" (") + DataSummary + TEXT(")");
	}

	// Disabled nodes are compiled out at runtime — annotate instead of presenting as live logic
	if (!Node->IsNodeEnabled())
	{
		Label += TEXT(" [DISABLED]");
	}

	Output += Indent(Depth) + Label + TEXT("\n");

	// Get exec output pins
	TArray<UEdGraphPin*> ExecOuts = GetExecOutputPins(Node);

	if (ExecOuts.Num() == 0)
	{
		// Terminal node — no exec outputs
		return;
	}

	if (ExecOuts.Num() == 1)
	{
		// Single exec output — linear chain, recurse at same depth
		UEdGraphPin* OutPin = ExecOuts[0];
		for (UEdGraphPin* LinkedPin : OutPin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				WalkExecChain(LinkedPin->GetOwningNode(), Depth, Output, Visited, NodeBudget);
			}
		}
		return;
	}

	// Multiple exec outputs — Branch, Sequence, SwitchEnum, etc.
	// Use per-branch copies of Visited to prevent false "cycle" detection
	// when multiple branches target the same node (e.g., Switch branches → same Return)
	for (UEdGraphPin* OutPin : ExecOuts)
	{
		if (OutPin->LinkedTo.Num() == 0)
		{
			continue; // Skip unconnected branches
		}

		FString BranchLabel = OutPin->GetDisplayName().ToString();
		if (BranchLabel.IsEmpty())
		{
			BranchLabel = OutPin->PinName.ToString();
		}

		Output += Indent(Depth + 1) + TEXT("[") + BranchLabel + TEXT("]\n");

		TSet<UEdGraphNode*> BranchVisited = Visited;
		for (UEdGraphPin* LinkedPin : OutPin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				WalkExecChain(LinkedPin->GetOwningNode(), Depth + 2, Output, BranchVisited, NodeBudget);
			}
		}
	}
}

// ============================================================================
// DumpVariables
// ============================================================================

void FBlueprintDumpUtils::DumpVariables(UBlueprint* Blueprint, FString& Output)
{
	if (!Blueprint || Blueprint->NewVariables.Num() == 0)
	{
		return;
	}

	Output += TEXT("=== Variables ===\n");

	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		FString TypeStr = PinTypeToString(Var.VarType);

		// Container type prefix
		if (Var.VarType.IsArray())
		{
			TypeStr = TEXT("TArray<") + TypeStr + TEXT(">");
		}
		else if (Var.VarType.IsSet())
		{
			TypeStr = TEXT("TSet<") + TypeStr + TEXT(">");
		}
		else if (Var.VarType.IsMap())
		{
			TypeStr = TEXT("TMap<") + TypeStr + TEXT(", ...>");
		}

		FString Line = FString::Printf(TEXT("  %s: %s"), *Var.VarName.ToString(), *TypeStr);

		// Default value
		if (!Var.DefaultValue.IsEmpty())
		{
			Line += FString::Printf(TEXT(" (Default: %s)"), *Var.DefaultValue);
		}

		// Property flags of interest
		TArray<FString> Flags;
		if (Var.PropertyFlags & CPF_Edit)
		{
			Flags.Add(TEXT("EditAnywhere"));
		}
		if (Var.PropertyFlags & CPF_BlueprintReadOnly)
		{
			Flags.Add(TEXT("ReadOnly"));
		}
		if (Var.PropertyFlags & CPF_Net)
		{
			Flags.Add(TEXT("Replicated"));
		}
		if (Var.PropertyFlags & CPF_ExposeOnSpawn)
		{
			Flags.Add(TEXT("ExposeOnSpawn"));
		}

		if (Flags.Num() > 0)
		{
			Line += TEXT(" [") + FString::Join(Flags, TEXT(", ")) + TEXT("]");
		}

		Output += Line + TEXT("\n");
	}

	Output += TEXT("\n");
}

// ============================================================================
// DumpInterfaces
// ============================================================================

void FBlueprintDumpUtils::DumpInterfaces(UBlueprint* Blueprint, FString& Output)
{
	if (!Blueprint || Blueprint->ImplementedInterfaces.Num() == 0)
	{
		return;
	}

	Output += TEXT("=== Interfaces ===\n");

	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		if (InterfaceDesc.Interface)
		{
			Output += FString::Printf(TEXT("  %s\n"), *InterfaceDesc.Interface->GetName());
		}
	}

	Output += TEXT("\n");
}

// ============================================================================
// DumpEventGraphs
// ============================================================================

void FBlueprintDumpUtils::DumpEventGraphs(UBlueprint* Blueprint, FString& Output)
{
	if (!Blueprint || Blueprint->UbergraphPages.Num() == 0)
	{
		return;
	}

	Output += TEXT("=== EventGraph ===\n");

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}

		// Find all event entry nodes in this graph
		TArray<UEdGraphNode*> EventNodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<UK2Node_Event>(Node) || Cast<UK2Node_CustomEvent>(Node))
			{
				EventNodes.Add(Node);
			}
		}

		if (EventNodes.Num() == 0)
		{
			continue;
		}

		// If multiple UbergraphPages, show graph name
		if (Blueprint->UbergraphPages.Num() > 1)
		{
			Output += FString::Printf(TEXT("--- Graph: %s ---\n"), *Graph->GetName());
		}

		for (UEdGraphNode* EventNode : EventNodes)
		{
			// Print event header with parameter info
			FString EventLabel = GetK2NodeLabel(EventNode);
			if (!EventNode->IsNodeEnabled())
			{
				EventLabel += TEXT(" [DISABLED]");
			}

			// Extract parameter pins (non-exec output pins = event parameters)
			TArray<FString> Params;
			for (UEdGraphPin* Pin : EventNode->Pins)
			{
				if (Pin->Direction == EGPD_Output && !IsExecPin(Pin) && !Pin->bHidden)
				{
					Params.Add(Pin->PinName.ToString());
				}
			}

			FString ParamStr;
			if (Params.Num() > 0)
			{
				ParamStr = TEXT(" (") + FString::Join(Params, TEXT(", ")) + TEXT(")");
			}

			Output += FString::Printf(TEXT("--- %s%s ---\n"), *EventLabel, *ParamStr);

			// Walk exec chain from this event
			TSet<UEdGraphNode*> Visited;
			int32 NodeBudget = 2000;
			TArray<UEdGraphPin*> ExecOuts = GetExecOutputPins(EventNode);
			for (UEdGraphPin* OutPin : ExecOuts)
			{
				for (UEdGraphPin* LinkedPin : OutPin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						WalkExecChain(LinkedPin->GetOwningNode(), 1, Output, Visited, NodeBudget);
					}
				}
			}

			Output += TEXT("\n");
		}
	}
}

// ============================================================================
// DumpFunctions
// ============================================================================

void FBlueprintDumpUtils::DumpFunctions(UBlueprint* Blueprint, FString& Output)
{
	if (!Blueprint || Blueprint->FunctionGraphs.Num() == 0)
	{
		return;
	}

	Output += TEXT("=== Functions ===\n");

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		// Find the function entry node
		UK2Node_FunctionEntry* EntryNode = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!EntryNode)
			{
				EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			}
			if (!ResultNode)
			{
				ResultNode = Cast<UK2Node_FunctionResult>(Node);
			}
		}

		if (!EntryNode)
		{
			continue;
		}

		// Build function signature
		// Input params = non-exec output pins on the entry node
		TArray<FString> InputParams;
		for (UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (Pin->Direction == EGPD_Output && !IsExecPin(Pin) && !Pin->bHidden)
			{
				FString TypeStr = PinTypeToString(Pin->PinType);
				InputParams.Add(FString::Printf(TEXT("%s %s"), *TypeStr, *Pin->PinName.ToString()));
			}
		}

		// Return type from result node
		FString ReturnStr;
		if (ResultNode)
		{
			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && !IsExecPin(Pin) && Pin->PinName != TEXT("self") && !Pin->bHidden)
				{
					FString TypeStr = PinTypeToString(Pin->PinType);
					if (ReturnStr.IsEmpty())
					{
						ReturnStr = TypeStr;
					}
					else
					{
						ReturnStr += TEXT(", ") + TypeStr;
					}
				}
			}
		}

		FString Signature = FString::Printf(TEXT("%s(%s)"),
			*Graph->GetName(),
			*FString::Join(InputParams, TEXT(", ")));

		if (!ReturnStr.IsEmpty())
		{
			Signature += TEXT(" -> ") + ReturnStr;
		}

		Output += FString::Printf(TEXT("--- %s ---\n"), *Signature);

		// Walk exec chain from entry
		TSet<UEdGraphNode*> Visited;
		int32 NodeBudget = 2000;
		TArray<UEdGraphPin*> ExecOuts = GetExecOutputPins(EntryNode);
		for (UEdGraphPin* OutPin : ExecOuts)
		{
			for (UEdGraphPin* LinkedPin : OutPin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					WalkExecChain(LinkedPin->GetOwningNode(), 1, Output, Visited, NodeBudget);
				}
			}
		}

		Output += TEXT("\n");
	}
}

// ============================================================================
// DumpComponentTree
// ============================================================================

static void DumpComponentTreeRecursive(USCS_Node* Node, int32 Depth, FString& Output)
{
	if (!Node)
	{
		return;
	}

	FString Prefix;
	for (int32 i = 0; i < Depth; ++i)
	{
		if (i == Depth - 1)
		{
			Prefix += TEXT("  +-- ");
		}
		else
		{
			Prefix += TEXT("  |   ");
		}
	}
	if (Depth == 0)
	{
		Prefix = TEXT("  ");
	}

	FString NodeName = Node->GetVariableName().ToString();
	FString ClassName;
	if (Node->ComponentTemplate)
	{
		ClassName = Node->ComponentTemplate->GetClass()->GetName();
	}
	else if (Node->ComponentClass)
	{
		ClassName = Node->ComponentClass->GetName();
	}
	else
	{
		ClassName = TEXT("Unknown");
	}

	// Check for special properties on skeletal mesh components
	FString Extra;
	if (Node->ComponentTemplate)
	{
		if (USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
		{
			if (SkelMesh->GetAnimClass())
			{
				Extra = FString::Printf(TEXT(" [AnimClass=%s]"), *SkelMesh->GetAnimClass()->GetName());
			}
		}
	}

	Output += FString::Printf(TEXT("%s%s (%s)%s\n"), *Prefix, *NodeName, *ClassName, *Extra);

	// Recurse into children
	for (USCS_Node* Child : Node->ChildNodes)
	{
		DumpComponentTreeRecursive(Child, Depth + 1, Output);
	}
}

void FBlueprintDumpUtils::DumpComponentTree(UBlueprint* Blueprint, FString& Output)
{
	if (!Blueprint)
	{
		return;
	}

	// Try SCS (Simple Construction Script) first — this is the Blueprint component tree
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (SCS)
	{
		const TArray<USCS_Node*>& RootNodes = SCS->GetRootNodes();
		if (RootNodes.Num() > 0)
		{
			Output += TEXT("=== Components (Blueprint) ===\n");
			for (USCS_Node* RootNode : RootNodes)
			{
				DumpComponentTreeRecursive(RootNode, 0, Output);
			}
		}
	}

	// Also dump CDO components (C++ defined) for completeness
	if (Blueprint->GeneratedClass)
	{
		AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
		if (CDO)
		{
			TArray<UActorComponent*> Components;
			CDO->GetComponents(Components);

			// Filter to only C++ components (ones NOT in the SCS)
			TSet<FName> SCSNames;
			if (SCS)
			{
				TArray<USCS_Node*> AllSCSNodes = SCS->GetAllNodes();
				for (USCS_Node* SCSNode : AllSCSNodes)
				{
					SCSNames.Add(SCSNode->GetVariableName());
				}
			}

			TArray<UActorComponent*> CppComponents;
			for (UActorComponent* Comp : Components)
			{
				FName CompName = Comp->GetFName();
				if (!SCSNames.Contains(CompName))
				{
					CppComponents.Add(Comp);
				}
			}

			if (CppComponents.Num() > 0)
			{
				Output += TEXT("=== Components (C++) ===\n");

				// Build parent→children map for scene components
				TMap<USceneComponent*, TArray<USceneComponent*>> ChildMap;
				USceneComponent* RootComp = CDO->GetRootComponent();

				for (UActorComponent* Comp : CppComponents)
				{
					USceneComponent* SceneComp = Cast<USceneComponent>(Comp);
					if (SceneComp && SceneComp->GetAttachParent())
					{
						ChildMap.FindOrAdd(SceneComp->GetAttachParent()).Add(SceneComp);
					}
				}

				// Print tree starting from root
				TFunction<void(USceneComponent*, int32)> PrintTree = [&](USceneComponent* Comp, int32 Depth)
				{
					FString Prefix;
					for (int32 i = 0; i < Depth; ++i)
					{
						Prefix += (i == Depth - 1) ? TEXT("  +-- ") : TEXT("  |   ");
					}
					if (Depth == 0) Prefix = TEXT("  ");

					Output += FString::Printf(TEXT("%s%s (%s)\n"),
						*Prefix, *Comp->GetName(), *Comp->GetClass()->GetName());

					if (TArray<USceneComponent*>* Children = ChildMap.Find(Comp))
					{
						for (USceneComponent* Child : *Children)
						{
							PrintTree(Child, Depth + 1);
						}
					}
				};

				if (RootComp && !SCSNames.Contains(RootComp->GetFName()))
				{
					PrintTree(RootComp, 0);
				}

				// Print non-scene components
				for (UActorComponent* Comp : CppComponents)
				{
					if (!Cast<USceneComponent>(Comp))
					{
						Output += FString::Printf(TEXT("  %s (%s)\n"),
							*Comp->GetName(), *Comp->GetClass()->GetName());
					}
				}
			}
		}
	}

	Output += TEXT("\n");
}

// ============================================================================
// Indent helper
// ============================================================================

FString FBlueprintDumpUtils::Indent(int32 Depth)
{
	if (Depth <= 0)
	{
		return FString();
	}

	FString Result;
	for (int32 i = 0; i < Depth; ++i)
	{
		Result += TEXT("  ");
	}
	Result += TEXT("-> ");
	return Result;
}
