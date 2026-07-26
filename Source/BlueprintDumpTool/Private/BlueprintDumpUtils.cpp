// Copyright (c) 2026 knight-scripts. MIT License.

#include "BlueprintDumpUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"

#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"

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
#include "K2Node_BreakStruct.h"

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

FString FBlueprintDumpUtils::FriendlyPinName(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return FString();
	}

	// Pins generated from BLUEPRINT struct members carry an internal
	// "_<index>_<32 hex GUID>" group: "BackLedgeHeight_64_FA78930E4...". On a SPLIT
	// member the group sits in the MIDDLE — "GridSizes_16_782DDA9B4..._X" — so scan for
	// it rather than only stripping a tail. Native pin names never match this shape.
	const FString Name = Pin->PinName.ToString();

	TArray<FString> Tokens;
	Name.ParseIntoArray(Tokens, TEXT("_"), /*InCullEmpty=*/false);

	auto IsHex32 = [](const FString& Token)
	{
		if (Token.Len() != 32)
		{
			return false;
		}
		for (const TCHAR C : Token)
		{
			const bool bHexDigit = (C >= TEXT('0') && C <= TEXT('9'))
				|| (C >= TEXT('A') && C <= TEXT('F'))
				|| (C >= TEXT('a') && C <= TEXT('f'));
			if (!bHexDigit)
			{
				return false;
			}
		}
		return true;
	};

	// Start at 1: never strip the leading token, that is the member's actual name
	for (int32 i = 1; i + 1 < Tokens.Num(); ++i)
	{
		if (Tokens[i].IsNumeric() && IsHex32(Tokens[i + 1]))
		{
			Tokens.RemoveAt(i, 2);
			return FString::Join(Tokens, TEXT("_"));
		}
	}

	return Name;
}

namespace
{
	/** Any data input worth resolving? Used to decide whether a depth-truncated node
	 *  should be marked — a bare title is otherwise indistinguishable from a real
	 *  zero-argument call. */
	bool HasDataInput(const UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input
				&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				&& Pin->PinName != TEXT("self"))
			{
				return true;
			}
		}
		return false;
	}

	/** WHICH output was read is information the node title alone does not carry: a struct
	 *  Select feeding a range's Min and Max renders identically for both, and split pins
	 *  make X and Y the same string. Qualify the expression with the pin that produced it. */
	FString QualifyOutput(const FString& Expr, const UEdGraphPin* Pin, const UEdGraphNode* Node)
	{
		if (!Pin || !Node)
		{
			return Expr;
		}

		// Split struct sub-pin: "ReturnValue_X" under parent "ReturnValue" -> ".X"
		// (FriendlyPinName then drops any BP-struct GUID suffix: "Side_9_D749..." -> "Side")
		if (const UEdGraphPin* Parent = Pin->ParentPin)
		{
			const FString Full = FBlueprintDumpUtils::FriendlyPinName(Pin);
			const FString ParentName = FBlueprintDumpUtils::FriendlyPinName(Parent);
			const FString Sub = Full.StartsWith(ParentName + TEXT("_"))
				? Full.RightChop(ParentName.Len() + 1)
				: Full;
			return Sub.IsEmpty() ? Expr : Expr + TEXT(".") + Sub;
		}

		// Several data outputs (function out-params, multi-output nodes): name the one read.
		// Top-level pins only — sub-pins of a split output are not separate outputs.
		int32 DataOutputs = 0;
		for (const UEdGraphPin* Other : Node->Pins)
		{
			if (Other->Direction == EGPD_Output
				&& Other->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				&& !Other->ParentPin)
			{
				++DataOutputs;
			}
		}

		const FString PinNameStr = FBlueprintDumpUtils::FriendlyPinName(Pin);
		return (DataOutputs > 1 && !PinNameStr.IsEmpty()) ? Expr + TEXT(".") + PinNameStr : Expr;
	}
}

FString FBlueprintDumpUtils::FormatLiteralPinValue(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return FString();
	}

	// Enums first — a raw byte index is not a value a reader can use
	const FString EnumOrDefault = ResolveEnumPinValue(Pin);
	if (!EnumOrDefault.IsEmpty())
	{
		// Quote string-likes so a curve/tag/socket name reads as DATA, not as an
		// identifier: GetCurveValueFromAnimation("MoveData_Speed", t).
		const FName& Category = Pin->PinType.PinCategory;
		if (Category == UEdGraphSchema_K2::PC_String
			|| Category == UEdGraphSchema_K2::PC_Name
			|| Category == UEdGraphSchema_K2::PC_Text)
		{
			return TEXT("\"") + EnumOrDefault + TEXT("\"");
		}
		return EnumOrDefault;
	}

	// Object/class literals live in DefaultObject, never in DefaultValue
	if (Pin->DefaultObject)
	{
		return Pin->DefaultObject->GetName();
	}

	if (!Pin->DefaultTextValue.IsEmpty())
	{
		return TEXT("\"") + Pin->DefaultTextValue.ToString() + TEXT("\"");
	}

	return FString();
}

FString FBlueprintDumpUtils::ResolvePinSourceLabel(UEdGraphPin* LinkedPin, int32 MaxDepth)
{
	if (!LinkedPin || !LinkedPin->GetOwningNode())
	{
		// Stale link entries exist in mildly corrupted assets
		return TEXT("???");
	}
	return BuildExpressionFromPin(LinkedPin, MaxDepth);
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
		if (Title.IsEmpty())
		{
			Title = Node->GetName();
		}
		// Silence is a bug signal: a bare title here reads exactly like a genuine
		// zero-argument call. Mark the truncation whenever inputs actually exist.
		return HasDataInput(Node) ? QualifyOutput(Title + TEXT("(...)"), Pin, Node) : Title;
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
				FString PinNameStr = FriendlyPinName(Pin);
				if (PinNameStr != TEXT("Output") && PinNameStr != TEXT("Result") && PinNameStr != TEXT("ReturnValue"))
				{
					return PinNameStr;
				}
			}

			// Return whatever title we have (may still be "Property Access")
			return ListTitle;
		}
	}

	// Break struct — the MEMBER read is the whole point. "Break S Player Input
	// State(CharacterInputState)" says nothing; "CharacterInputState.WantsToSprint" is
	// what the graph actually means.
	if (Cast<UK2Node_BreakStruct>(Node))
	{
		for (UEdGraphPin* InputPin : Node->Pins)
		{
			if (InputPin->Direction == EGPD_Input
				&& InputPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				&& InputPin->LinkedTo.Num() > 0
				&& InputPin->LinkedTo[0])
			{
				const FString Source = BuildExpressionFromPin(InputPin->LinkedTo[0], MaxDepth, CurrentDepth + 1);

				// A SPLIT member reads as one more hop: CharacterProperties.LandVelocity.Z
				FString Member = FriendlyPinName(Pin);
				if (const UEdGraphPin* MemberParent = Pin->ParentPin)
				{
					const FString ParentName = FriendlyPinName(MemberParent);
					if (Member.StartsWith(ParentName + TEXT("_")))
					{
						Member = ParentName + TEXT(".") + Member.RightChop(ParentName.Len() + 1);
					}
				}
				return Source + TEXT(".") + Member;
			}
		}
	}

	// Variable Get node — return the variable name (qualified if the output is split)
	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
	{
		return QualifyOutput(VarGet->GetVarName().ToString(), Pin, Node);
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
						FString Literal = FormatLiteralPinValue(InputPin);
						if (Literal.IsEmpty())
						{
							Literal = (InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
								? TEXT("false") : TEXT("0");
						}
						Operands.Add(Literal);
					}
				}
			}

			if (Operands.Num() >= 2)
			{
				// Qualified for the split-output case: (A + B).X on a vector operator
				return QualifyOutput(
					Grouped(FString::Printf(TEXT("%s %s %s"), *Operands[0], *OpSymbol, *Operands[1])),
					Pin, Node);
			}
			if (Operands.Num() == 1)
			{
				// Missing operand is a parse gap — say so instead of inventing a value
				return Grouped(FString::Printf(TEXT("%s %s ?"), *Operands[0], *OpSymbol));
			}
			return OpSymbol;
		}

		// Not a recognized operator — a real function call. Recurse into its inputs:
		// returning the bare name here was THE M1 data-loss bug (Gaps 1.1/1.2/1.4).
		// "Lerp()" becomes "Lerp(1.0, Clamp(SafeDivide(Speed2D, Rate), Min, Max), Alpha)".
		{
			TArray<FString> Args;
			for (UEdGraphPin* InputPin : Node->Pins)
			{
				if (InputPin->Direction != EGPD_Input) continue;
				if (InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (InputPin->PinName == TEXT("self")) continue; // target object — noise

				if (InputPin->LinkedTo.Num() > 0 && InputPin->LinkedTo[0])
				{
					Args.Add(BuildExpressionFromPin(InputPin->LinkedTo[0], MaxDepth, CurrentDepth + 1));
				}
				else
				{
					FString Literal = FormatLiteralPinValue(InputPin);
					Args.Add(Literal.IsEmpty() ? TEXT("?") : Literal);
				}
			}

			// Unset TRAILING optional params are noise; a gap in the middle is a real
			// unknown and stays "?" so argument positions keep their meaning.
			while (Args.Num() > 0 && Args.Last() == TEXT("?"))
			{
				Args.Pop();
			}

			// Qualified: a function with out-params renders identically for each of them
			return QualifyOutput(
				FString::Printf(TEXT("%s(%s)"), *FuncNameStr, *FString::Join(Args, TEXT(", "))),
				Pin, Node);
		}
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
				FString Literal = FormatLiteralPinValue(InputPin);
				if (!Literal.IsEmpty())
				{
					Operands.Add(Literal);
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

		// Not an operator — format with inputs if available. Qualified, because this is
		// where multi-output nodes (struct Selects, split pins) land.
		if (Operands.Num() > 0)
		{
			return QualifyOutput(
				FString::Printf(TEXT("%s(%s)"), *NodeTitle, *FString::Join(Operands, TEXT(", "))),
				Pin, Node);
		}

		return QualifyOutput(NodeTitle.IsEmpty() ? Node->GetName() : NodeTitle, Pin, Node);
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
			PinName = FriendlyPinName(Pin);
		}

		if (Pin->LinkedTo.Num() > 0)
		{
			Parts.Add(FString::Printf(TEXT("%s=%s"), *PinName, *ResolvePinSourceLabel(Pin->LinkedTo[0])));
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
					Label += TEXT(" = ") + ResolvePinSourceLabel(Pin->LinkedTo[0]);
				}
				else
				{
					const FString Literal = FormatLiteralPinValue(Pin);
					if (!Literal.IsEmpty())
					{
						Label += TEXT(" = ") + Literal;
					}
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
					Params.Add(FriendlyPinName(Pin));
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
				InputParams.Add(FString::Printf(TEXT("%s %s"), *TypeStr, *FriendlyPinName(Pin)));
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
// DumpClassDefaultOverrides
// ============================================================================
//
// "BP property overrides vs C++ defaults": diff the Blueprint CDO — and its
// default subobjects (e.g. a CharacterMovement component, where movement knobs
// actually live) — against the parent class archetype, so Class Defaults panel
// state is visible in the dump. A property NOT listed is running its inherited
// (usually C++) default; the section header always prints so absence of a knob
// line is meaningful evidence, not a tool-version ambiguity.
//
// The diff walker below works on VALUE pointers rather than UObject containers so
// it can descend through structs and arrays into instanced sub-objects. That
// descent is the point: a plugin that authors its behaviour as EditInlineNew
// objects in the details panel keeps every tuned number down there, and a walker
// that stops at the pointer reports the asset as empty.

namespace
{
	// Transient/deprecated members are noise. Instanced references are deliberately
	// NOT skipped: their POINTERS always differ (the reason they were once filtered
	// out wholesale), but their CONTENT is exactly where details-panel authoring puts
	// the data — an EditInlineNew plugin keeps 100% of its tuning there, so filtering
	// the pointer must not mean discarding the object.
	constexpr uint64 SkipDiffPropertyFlags =
		CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient |
		CPF_Deprecated;

	// Instanced graphs can nest (attack -> traits -> impacts); bound the descent.
	constexpr int32 MaxInstancedDepth = 4;

	/** Shared state for one diff run: cycle guard + current instanced nesting depth. */
	struct FDiffContext
	{
		TSet<const UObject*> Visited;
		int32 Depth = 0;
	};

	int32 AppendPropertyDiffs(UStruct* Scope, const void* Container, const void* Baseline,
		UObject* ExportOwner, const FString& LinePrefix, FDiffContext& Ctx, FString& Output);

	/** Object properties whose value is owned INLINE by the container — the details-panel
	 *  "EditInlineNew / Instanced" authoring model. These get recursed into, not compared. */
	const FObjectProperty* AsInstancedObjectProperty(const FProperty* Prop)
	{
		const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop);
		if (!ObjProp || !ObjProp->PropertyClass)
		{
			return nullptr;
		}
		if (Prop->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance)
			|| ObjProp->PropertyClass->HasAnyClassFlags(CLASS_EditInlineNew))
		{
			return ObjProp;
		}
		return nullptr;
	}

	FString ExportValue(const FProperty* Prop, const void* ValuePtr, UObject* ExportOwner)
	{
		// HARD object references: print the asset path, not ExportText's verbose form.
		// Soft/weak refs stay on the generic path — they resolve to null when unloaded,
		// which would alias two different soft paths.
		if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			UObject* Value = ObjProp->GetObjectPropertyValue(ValuePtr);
			return Value ? Value->GetPathName() : TEXT("None");
		}

		FString ValueStr;
		Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, ExportOwner, PPF_None);
		if (ValueStr.IsEmpty())
		{
			ValueStr = TEXT("(empty)");
		}
		else if (ValueStr.Len() > 220)
		{
			ValueStr = ValueStr.Left(220) + TEXT("... (truncated)");
		}
		return ValueStr;
	}

	bool ValuesIdentical(const FProperty* Prop, const void* A, const void* B)
	{
		if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			UObject* ObjA = ObjProp->GetObjectPropertyValue(A);
			UObject* ObjB = ObjProp->GetObjectPropertyValue(B);
			if (ObjA == ObjB)
			{
				return true;
			}
			if (!ObjA || !ObjB)
			{
				return false;
			}
			// Sub-objects of the two compared containers can never be pointer-equal;
			// match those by name + class. Everything else is an asset reference —
			// compare paths so two distinct assets never alias.
			const bool bSubA = ObjA->GetOuter() && !ObjA->GetOuter()->IsA<UPackage>();
			const bool bSubB = ObjB->GetOuter() && !ObjB->GetOuter()->IsA<UPackage>();
			if (bSubA && bSubB)
			{
				return ObjA->GetFName() == ObjB->GetFName() && ObjA->GetClass() == ObjB->GetClass();
			}
			return ObjA->GetPathName() == ObjB->GetPathName();
		}
		return Prop->Identical(A, B, PPF_None);
	}

	/** Recurse into one instanced sub-object's contents. */
	int32 AppendInstancedObject(const FString& Label, UObject* Value, UObject* BaselineValue,
		const FString& LinePrefix, FDiffContext& Ctx, FString& Output)
	{
		if (!Value)
		{
			// Clearing a slot that shipped with a value IS an authoring act
			if (BaselineValue)
			{
				Output += FString::Printf(TEXT("%s%s = None  (default: %s)\n"),
					*LinePrefix, *Label, *BaselineValue->GetClass()->GetName());
				return 1;
			}
			return 0;
		}

		// An object outered directly to a package is a standalone ASSET — a reference,
		// not inline content. Some referenced classes are EditInlineNew, so without this
		// guard a plain asset pointer would splice another asset's guts into this dump.
		if (Value->GetOuter() && Value->GetOuter()->IsA<UPackage>())
		{
			if (BaselineValue == Value)
			{
				return 0;
			}
			Output += FString::Printf(TEXT("%s%s = %s  (default: %s)\n"),
				*LinePrefix, *Label, *Value->GetPathName(),
				BaselineValue ? *BaselineValue->GetPathName() : TEXT("None"));
			return 1;
		}

		// Components are enumerated by the dedicated subobject/SCS passes; recursing
		// here would print every component twice.
		if (Value->IsA(UActorComponent::StaticClass()))
		{
			return 0;
		}

		if (Ctx.Depth >= MaxInstancedDepth)
		{
			Output += FString::Printf(TEXT("%s%s = %s  (depth limit - not expanded)\n"),
				*LinePrefix, *Label, *Value->GetClass()->GetName());
			return 1;
		}
		if (Ctx.Visited.Contains(Value))
		{
			Output += FString::Printf(TEXT("%s%s = %s  (already shown above)\n"),
				*LinePrefix, *Label, *Value->GetClass()->GetName());
			return 1;
		}
		Ctx.Visited.Add(Value);

		// Baseline: the archetype counterpart when it is the SAME class, otherwise the
		// value's own class CDO. That second case is the details-panel "author picked a
		// subclass" case — which the previous code skipped outright as "diff undefined",
		// discarding exactly the objects worth reading.
		const UObject* Baseline = (BaselineValue && BaselineValue->GetClass() == Value->GetClass())
			? BaselineValue
			: Value->GetClass()->GetDefaultObject();

		FString Body;
		++Ctx.Depth;
		const int32 Inner = AppendPropertyDiffs(Value->GetClass(), Value, Baseline, Value,
			LinePrefix + TEXT("  "), Ctx, Body);
		--Ctx.Depth;

		Output += FString::Printf(TEXT("%s%s = %s\n"), *LinePrefix, *Label, *Value->GetClass()->GetName());
		Output += Inner > 0 ? Body : (LinePrefix + TEXT("    (no property differences)\n"));
		return Inner + 1;
	}

	/** One array element that carries instanced content (object or struct-wrapping-object). */
	int32 AppendInstancedElement(const FProperty* ElemProp, const FString& Label,
		const void* ValuePtr, const void* BaselinePtr, UObject* ExportOwner,
		const FString& LinePrefix, FDiffContext& Ctx, FString& Output)
	{
		if (const FObjectProperty* ObjProp = AsInstancedObjectProperty(ElemProp))
		{
			UObject* Value = ObjProp->GetObjectPropertyValue(ValuePtr);
			UObject* BaselineValue = BaselinePtr ? ObjProp->GetObjectPropertyValue(BaselinePtr) : nullptr;
			return AppendInstancedObject(Label, Value, BaselineValue, LinePrefix, Ctx, Output);
		}

		if (const FStructProperty* StructProp = CastField<FStructProperty>(ElemProp))
		{
			// No archetype counterpart (array grew): compare against a default-constructed
			// instance, so "what did the author set" stays answerable for new elements.
			FStructOnScope DefaultStruct(StructProp->Struct);
			const void* Baseline = BaselinePtr ? BaselinePtr : DefaultStruct.GetStructMemory();

			FString Body;
			const int32 Inner = AppendPropertyDiffs(StructProp->Struct, ValuePtr, Baseline, ExportOwner,
				LinePrefix + TEXT("  "), Ctx, Body);
			if (Inner > 0)
			{
				Output += FString::Printf(TEXT("%s%s (%s)\n"), *LinePrefix, *Label, *StructProp->Struct->GetName());
				Output += Body;
			}
			return Inner;
		}

		return 0;
	}

	// Diff Container vs Baseline over Scope's properties (incl. supers). Scope must be a
	// struct/class whose layout is valid in BOTH containers.
	int32 AppendPropertyDiffs(UStruct* Scope, const void* Container, const void* Baseline,
		UObject* ExportOwner, const FString& LinePrefix, FDiffContext& Ctx, FString& Output)
	{
		if (!Scope || !Container)
		{
			return 0;
		}

		int32 DiffCount = 0;
		for (TFieldIterator<FProperty> It(Scope); It; ++It)
		{
			const FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(SkipDiffPropertyFlags))
			{
				continue;
			}

			for (int32 ArrayIndex = 0; ArrayIndex < Prop->ArrayDim; ++ArrayIndex)
			{
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container, ArrayIndex);
				const void* BasePtr = Baseline
					? Prop->ContainerPtrToValuePtr<void>(Baseline, ArrayIndex)
					: nullptr;

				// Authored name, not the raw one: BP-struct fields carry a GUID suffix
				// internally (Damage_5_A1B2...) that no reader should have to decode.
				FString Name = Scope->GetAuthoredNameForField(Prop);
				if (Prop->ArrayDim > 1)
				{
					Name += FString::Printf(TEXT("[%d]"), ArrayIndex);
				}

				// (1) Instanced object — recurse into its contents
				if (AsInstancedObjectProperty(Prop))
				{
					DiffCount += AppendInstancedElement(Prop, Name, ValuePtr, BasePtr,
						ExportOwner, LinePrefix, Ctx, Output);
					continue;
				}

				// (2) Array whose elements carry instanced content
				if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
				{
					const FProperty* Inner = ArrayProp->Inner;
					const bool bInnerInstanced = AsInstancedObjectProperty(Inner) != nullptr
						|| Inner->HasAnyPropertyFlags(CPF_ContainsInstancedReference);

					if (bInnerInstanced)
					{
						FScriptArrayHelper Helper(ArrayProp, ValuePtr);
						TUniquePtr<FScriptArrayHelper> BaseHelper;
						if (BasePtr)
						{
							BaseHelper = MakeUnique<FScriptArrayHelper>(ArrayProp, BasePtr);
						}

						const int32 BaseNum = BaseHelper ? BaseHelper->Num() : 0;
						if (Helper.Num() != BaseNum)
						{
							Output += FString::Printf(TEXT("%s%s: %d element(s)  (default: %d)\n"),
								*LinePrefix, *Name, Helper.Num(), BaseNum);
							++DiffCount;
						}

						for (int32 i = 0; i < Helper.Num(); ++i)
						{
							const void* BaseElem = (BaseHelper && i < BaseHelper->Num())
								? BaseHelper->GetRawPtr(i) : nullptr;
							DiffCount += AppendInstancedElement(Inner,
								FString::Printf(TEXT("%s[%d]"), *Name, i),
								Helper.GetRawPtr(i), BaseElem, ExportOwner, LinePrefix, Ctx, Output);
						}
						continue;
					}
				}

				// (3) Struct wrapping instanced content (e.g. an FInstanced*Properties
				//     struct holding one EditInlineNew object) — descend into its fields
				if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
				{
					if (Prop->HasAnyPropertyFlags(CPF_ContainsInstancedReference))
					{
						FString Body;
						const int32 Inner = AppendPropertyDiffs(StructProp->Struct, ValuePtr, BasePtr,
							ExportOwner, LinePrefix + TEXT("  "), Ctx, Body);
						if (Inner > 0)
						{
							Output += FString::Printf(TEXT("%s%s (%s)\n"),
								*LinePrefix, *Name, *StructProp->Struct->GetName());
							Output += Body;
							DiffCount += Inner;
						}
						continue;
					}
				}

				// (4) Plain value — diff it
				if (BasePtr && ValuesIdentical(Prop, ValuePtr, BasePtr))
				{
					continue;
				}

				Output += FString::Printf(TEXT("%s%s = %s  (default: %s)\n"),
					*LinePrefix, *Name,
					*ExportValue(Prop, ValuePtr, ExportOwner),
					BasePtr ? *ExportValue(Prop, BasePtr, ExportOwner) : TEXT("n/a"));
				++DiffCount;
			}
		}
		return DiffCount;
	}

	// Diff one subobject/template against its baseline; emit a bracketed sub-heading
	// only when something actually differs.
	int32 AppendSubobjectDiffs(UObject* Sub, const FString& DisplayName, const TCHAR* Suffix, FString& Output)
	{
		if (!Sub)
		{
			return 0;
		}

		UObject* Baseline = Sub->GetArchetype();
		const TCHAR* BaselineNote = TEXT("");
		if (!Baseline || Baseline == Sub || Baseline->GetClass() != Sub->GetClass())
		{
			// Class-overridden (or archetype-less) subobject: the honest baseline is the
			// sub's OWN class CDO. Skipping these was the "field diff undefined" hole.
			Baseline = Sub->GetClass()->GetDefaultObject();
			BaselineNote = TEXT(", vs class defaults");
		}
		if (!Baseline || Baseline == Sub)
		{
			return 0;
		}

		FDiffContext Ctx;
		Ctx.Visited.Add(Sub);

		FString Section;
		const int32 DiffCount = AppendPropertyDiffs(Sub->GetClass(), Sub, Baseline, Sub,
			TEXT("    "), Ctx, Section);
		if (DiffCount > 0)
		{
			Output += FString::Printf(TEXT("  [%s (%s)%s%s]\n"),
				*DisplayName, *Sub->GetClass()->GetName(), Suffix, BaselineNote);
			Output += Section;
		}
		return DiffCount;
	}
}

int32 FBlueprintDumpUtils::DumpObjectPropertyDiffs(UObject* Object, const UObject* Baseline,
	const FString& LinePrefix, FString& Output)
{
	if (!Object)
	{
		return 0;
	}

	const UObject* Base = Baseline ? Baseline : Object->GetClass()->GetDefaultObject();

	FDiffContext Ctx;
	Ctx.Visited.Add(Object);
	return AppendPropertyDiffs(Object->GetClass(), Object, Base, Object, LinePrefix, Ctx, Output);
}

void FBlueprintDumpUtils::DumpClassDefaultOverrides(UBlueprint* Blueprint, FString& Output)
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return;
	}

	UClass* GenClass = Blueprint->GeneratedClass;
	UObject* CDO = GenClass->GetDefaultObject();
	UClass* ParentClass = GenClass->GetSuperClass();
	if (!CDO || !ParentClass)
	{
		return;
	}
	UObject* ParentCDO = ParentClass->GetDefaultObject();
	if (!ParentCDO)
	{
		return;
	}

	Output += FString::Printf(TEXT("=== Class Defaults (overrides vs %s) ===\n"), *ParentClass->GetName());

	// 1) The CDO itself, over INHERITED properties only (parent-class scope:
	//    BP-added variables are already covered by the Variables section).
	FDiffContext Ctx;
	Ctx.Visited.Add(CDO);
	int32 TotalDiffs = AppendPropertyDiffs(ParentClass, CDO, ParentCDO, CDO, TEXT("  "), Ctx, Output);

	// 2) Default subobjects (C++-created components) vs their archetypes on the
	//    parent CDO — where Class-Defaults edits to inherited components live.
	TArray<UObject*> Subobjects;
	CDO->GetDefaultSubobjects(Subobjects);
	for (UObject* Sub : Subobjects)
	{
		if (Sub)
		{
			TotalDiffs += AppendSubobjectDiffs(Sub, Sub->GetName(), TEXT(""), Output);
		}
	}

	// 3) BP-added components: SCS templates vs their archetypes (the component
	//    class CDO, or the parent BP's template).
	if (Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentTemplate)
			{
				TotalDiffs += AppendSubobjectDiffs(Node->ComponentTemplate, Node->GetVariableName().ToString(), TEXT(", BP-added"), Output);
			}
		}
	}

	if (TotalDiffs == 0)
	{
		Output += TEXT("  (no overrides)\n");
	}
	Output += TEXT("  (not listed = inherited default)\n\n");
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
