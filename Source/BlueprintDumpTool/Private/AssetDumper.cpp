// Copyright (c) 2026 knight-scripts. MIT License.

#include "AssetDumper.h"
#include "AnimBPDumper.h"
#include "BlueprintDumper.h"
#include "BlueprintDumpUtils.h"

#include "Engine/Blueprint.h"
#include "Animation/AnimBlueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"

#include "Curves/RichCurve.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/Skeleton.h"
#include "AlphaBlend.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"

#include "UObject/UnrealType.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

// ============================================================================
// Local helpers
// ============================================================================

namespace
{
	constexpr int32 MaxBTDepth = 32;

	FString Pad(int32 Depth)
	{
		FString Result;
		for (int32 i = 0; i < Depth; ++i)
		{
			Result += TEXT("  ");
		}
		return Result;
	}

	FString Truncate(FString Value, int32 Limit = 220)
	{
		return Value.Len() > Limit ? Value.Left(Limit) + TEXT("... (truncated)") : Value;
	}

	/** Print every field of a struct instance. Used where the VALUES are the payload
	 *  (data-table rows) rather than deltas from a default. */
	void AppendStructValues(const UScriptStruct* Struct, const void* Data, const FString& Prefix, FString& Output)
	{
		if (!Struct || !Data)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Prop = *It;
			FString ValueStr;
			Prop->ExportTextItem_Direct(ValueStr, Prop->ContainerPtrToValuePtr<void>(Data),
				nullptr, nullptr, PPF_None);
			Output += FString::Printf(TEXT("%s%s = %s\n"), *Prefix,
				*Struct->GetAuthoredNameForField(Prop),
				*Truncate(FBlueprintDumpUtils::StripMemberGuids(ValueStr)));
		}
	}

	/** Properties a node/object carries that differ from its class defaults. */
	void AppendAuthoredProperties(UObject* Object, const FString& Prefix, FString& Output)
	{
		if (!Object)
		{
			return;
		}
		FBlueprintDumpUtils::DumpObjectPropertyDiffs(Object, nullptr, Prefix, Output);
	}

	/** "PropName", "PropName[3]", "PropName = x", "PropName: 3 element(s)" — but not
	 *  "PropNameSomethingElse". */
	bool IsPropertyLine(const FString& Trimmed, const FString& Name)
	{
		if (!Trimmed.StartsWith(Name))
		{
			return false;
		}
		if (Trimmed.Len() == Name.Len())
		{
			return true;
		}
		const TCHAR Next = Trimmed[Name.Len()];
		return Next == TEXT('[') || Next == TEXT(' ') || Next == TEXT(':') || Next == TEXT('=');
	}

	/** Diff an object's properties, omitting named top-level entries AND everything nested
	 *  under them. Used wherever a structured section already renders that data: repeating
	 *  it as raw property text buries whatever the section did not cover, which is the
	 *  entire reason to print the diff at all. */
	void AppendFilteredProperties(UObject* Object, const FString& Prefix,
		const TArray<FString>& SkipNames, FString& Output)
	{
		if (!Object)
		{
			return;
		}

		FString All;
		FBlueprintDumpUtils::DumpObjectPropertyDiffs(Object, nullptr, Prefix, All);

		TArray<FString> Lines;
		All.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

		const int32 BaseIndent = Prefix.Len();
		bool bSkippingBlock = false;

		for (const FString& Line : Lines)
		{
			if (Line.IsEmpty())
			{
				continue;
			}

			int32 Indent = 0;
			while (Indent < Line.Len() && Line[Indent] == TEXT(' '))
			{
				++Indent;
			}

			// A line at (or above) the base indent starts a new top-level entry, which
			// also ends any block we were skipping
			if (Indent <= BaseIndent)
			{
				const FString Trimmed = Line.TrimStart();
				bSkippingBlock = false;
				for (const FString& Skip : SkipNames)
				{
					if (IsPropertyLine(Trimmed, Skip))
					{
						bSkippingBlock = true;
						break;
					}
				}
			}

			if (!bSkippingBlock)
			{
				Output += Line + TEXT("\n");
			}
		}
	}

	/** Behavior-tree nodes carry structural back-pointers and child arrays that the tree
	 *  walk already renders; printing them per node buries the authored settings. */
	void AppendBTNodeProperties(UObject* Node, const FString& Prefix, FString& Output)
	{
		static const TArray<FString> Skip =
		{
			TEXT("TreeAsset"), TEXT("ParentNode"), TEXT("Children"),
			TEXT("Services"), TEXT("Decorators"), TEXT("DecoratorOps"),
		};
		AppendFilteredProperties(Node, Prefix, Skip, Output);
	}

	FString InterpModeToString(TEnumAsByte<ERichCurveInterpMode> Mode)
	{
		switch (Mode.GetValue())
		{
		case RCIM_Linear:   return TEXT("Linear");
		case RCIM_Constant: return TEXT("Constant");
		case RCIM_Cubic:    return TEXT("Cubic");
		case RCIM_None:     return TEXT("None");
		default:            return TEXT("?");
		}
	}

	void AppendRichCurve(const FRichCurve& Curve, const FString& Label, FString& Output)
	{
		const TArray<FRichCurveKey>& Keys = Curve.GetConstRefOfKeys();
		Output += FString::Printf(TEXT("%s: %d key(s)\n"), *Label, Keys.Num());
		for (const FRichCurveKey& Key : Keys)
		{
			Output += FString::Printf(TEXT("    t=%.4f  v=%.4f  interp=%s  arrive=%.4f  leave=%.4f\n"),
				Key.Time, Key.Value, *InterpModeToString(Key.InterpMode),
				Key.ArriveTangent, Key.LeaveTangent);
		}
	}

	// --- Animation assets (T4) ---
	//
	// Combat and traversal TIMING lives on montages, not in classes: an attack's trace
	// window, its allowed-input window, its rotation window are all notify states with a
	// start and a duration. "How long is the deflect window" is unanswerable without this.
	// The same data answers our own H10 commitment-window and jump-window questions.

	/** Keys are printed in full up to this many; beyond it the summary line still carries
	 *  count and ranges, so a baked per-frame curve stays honest without flooding. */
	constexpr int32 MaxCurveKeysPrinted = 64;

	void AppendAnimNotifies(const UAnimSequenceBase* Anim, FString& Output)
	{
		const TArray<FAnimNotifyEvent>& Events = Anim->Notifies;
		if (Events.Num() == 0)
		{
			return;
		}

		// Authored order is not guaranteed to be time order
		TArray<int32> Order;
		Order.Reserve(Events.Num());
		for (int32 i = 0; i < Events.Num(); ++i)
		{
			Order.Add(i);
		}
		Order.Sort([&Events](int32 A, int32 B)
		{
			return Events[A].GetTriggerTime() < Events[B].GetTriggerTime();
		});

		Output += FString::Printf(TEXT("=== Notifies (%d) ===\n"), Events.Num());
		for (const int32 Index : Order)
		{
			const FAnimNotifyEvent& Event = Events[Index];

			// A notify STATE has a duration (the window); a plain notify is an instant
			const float Start = Event.GetTriggerTime();
			const float Duration = Event.GetDuration();
			FString TimeStr = Duration > 0.0f
				? FString::Printf(TEXT("t=%.4f  dur=%.4f  end=%.4f"), Start, Duration, Start + Duration)
				: FString::Printf(TEXT("t=%.4f"), Start);

			FString TrackLabel = FString::Printf(TEXT("track=%d"), Event.TrackIndex);
#if WITH_EDITORONLY_DATA
			if (Anim->AnimNotifyTracks.IsValidIndex(Event.TrackIndex))
			{
				TrackLabel += FString::Printf(TEXT(" '%s'"),
					*Anim->AnimNotifyTracks[Event.TrackIndex].TrackName.ToString());
			}
#endif

			UAnimNotifyState* StateNotify = Event.NotifyStateClass;
			UAnimNotify* InstantNotify = Event.Notify;
			UObject* NotifyObject = StateNotify
				? static_cast<UObject*>(StateNotify)
				: static_cast<UObject*>(InstantNotify);
			// Skeleton notifies are a NAME with no class object — printing "(none)" for
			// them reads like a missing reference rather than the normal thing it is.
			const FString ClassSuffix = NotifyObject
				? FString::Printf(TEXT(" (%s)"), *NotifyObject->GetClass()->GetName())
				: FString();

			Output += FString::Printf(TEXT("  %s  %s  %s%s\n"),
				*TimeStr, *TrackLabel, *Event.NotifyName.ToString(), *ClassSuffix);

			// The notify's OWN properties — instanced attack/feel properties live here,
			// so T1's recursion is what makes this worth dumping at all.
			AppendAuthoredProperties(NotifyObject, TEXT("      "), Output);
		}
		Output += TEXT("\n");
	}

	void AppendAnimCurves(const UAnimSequenceBase* Anim, FString& Output)
	{
		const TArray<FFloatCurve>& Curves = Anim->GetCurveData().FloatCurves;
		if (Curves.Num() == 0)
		{
			return;
		}

		Output += FString::Printf(TEXT("=== Curves (%d) ===\n"), Curves.Num());
		for (const FFloatCurve& Curve : Curves)
		{
			const TArray<FRichCurveKey>& Keys = Curve.FloatCurve.GetConstRefOfKeys();

			float MinV = 0.0f, MaxV = 0.0f;
			for (int32 i = 0; i < Keys.Num(); ++i)
			{
				MinV = (i == 0) ? Keys[i].Value : FMath::Min(MinV, Keys[i].Value);
				MaxV = (i == 0) ? Keys[i].Value : FMath::Max(MaxV, Keys[i].Value);
			}

			Output += FString::Printf(TEXT("  %s : %d key(s)"),
				*Curve.GetName().ToString(), Keys.Num());
			if (Keys.Num() > 0)
			{
				Output += FString::Printf(TEXT("  value[%.4f..%.4f]  time[%.4f..%.4f]"),
					MinV, MaxV, Keys[0].Time, Keys.Last().Time);
			}
			Output += TEXT("\n");

			const int32 Printed = FMath::Min(Keys.Num(), MaxCurveKeysPrinted);
			for (int32 i = 0; i < Printed; ++i)
			{
				Output += FString::Printf(TEXT("      t=%.4f  v=%.4f\n"), Keys[i].Time, Keys[i].Value);
			}
			if (Keys.Num() > Printed)
			{
				Output += FString::Printf(TEXT("      ... (%d more keys; summary above is complete)\n"),
					Keys.Num() - Printed);
			}
		}
		Output += TEXT("\n");
	}

	void AppendMontageStructure(const UAnimMontage* Montage, FString& Output)
	{
		Output += FString::Printf(TEXT("Blend In: %.4fs   Blend Out: %.4fs   BlendOutTriggerTime: %.4f\n\n"),
			Montage->BlendIn.GetBlendTime(), Montage->BlendOut.GetBlendTime(),
			Montage->BlendOutTriggerTime);

		if (Montage->CompositeSections.Num() > 0)
		{
			Output += FString::Printf(TEXT("=== Sections (%d) ===\n"), Montage->CompositeSections.Num());
			for (const FCompositeSection& Section : Montage->CompositeSections)
			{
				Output += FString::Printf(TEXT("  %-24s t=%.4f  -> next: %s\n"),
					*Section.SectionName.ToString(), Section.GetTime(),
					Section.NextSectionName.IsNone() ? TEXT("(stop)") : *Section.NextSectionName.ToString());
			}
			Output += TEXT("\n");
		}

		if (Montage->SlotAnimTracks.Num() > 0)
		{
			Output += FString::Printf(TEXT("=== Slots (%d) ===\n"), Montage->SlotAnimTracks.Num());
			for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
			{
				Output += FString::Printf(TEXT("  %s\n"), *Slot.SlotName.ToString());
				for (int32 i = 0; i < Slot.AnimTrack.AnimSegments.Num(); ++i)
				{
					const FAnimSegment& Segment = Slot.AnimTrack.AnimSegments[i];
					const UAnimSequenceBase* Ref = Segment.GetAnimReference();
					Output += FString::Printf(
						TEXT("    [%d] %s  start=%.4f  anim=[%.4f..%.4f]  rate=%.4f  loops=%d\n"),
						i, Ref ? *Ref->GetName() : TEXT("(none)"),
						Segment.StartPos, Segment.AnimStartTime, Segment.AnimEndTime,
						Segment.AnimPlayRate, Segment.LoopingCount);
				}
			}
			Output += TEXT("\n");
		}
	}

	// --- Behavior Tree ---

	void AppendBTComposite(UBTCompositeNode* Composite, int32 Depth, FString& Output);

	void AppendBTLeaf(UBTNode* Node, const TCHAR* Kind, int32 Depth, FString& Output)
	{
		if (!Node)
		{
			return;
		}
		Output += FString::Printf(TEXT("%s%s: %s (%s)\n"),
			*Pad(Depth), Kind, *Node->GetNodeName(), *Node->GetClass()->GetName());
		AppendBTNodeProperties(Node, Pad(Depth + 1), Output);
	}

	void AppendBTComposite(UBTCompositeNode* Composite, int32 Depth, FString& Output)
	{
		if (!Composite || Depth > MaxBTDepth)
		{
			return;
		}

		Output += FString::Printf(TEXT("%s%s (%s)\n"),
			*Pad(Depth), *Composite->GetNodeName(), *Composite->GetClass()->GetName());
		AppendBTNodeProperties(Composite, Pad(Depth + 1), Output);

		for (UBTService* Service : Composite->Services)
		{
			AppendBTLeaf(Service, TEXT("Service"), Depth + 1, Output);
		}

		for (int32 i = 0; i < Composite->Children.Num(); ++i)
		{
			const FBTCompositeChild& Child = Composite->Children[i];
			Output += FString::Printf(TEXT("%s[child %d]\n"), *Pad(Depth + 1), i);

			// Decorators gate this child — they ARE the tree's conditions
			for (UBTDecorator* Decorator : Child.Decorators)
			{
				AppendBTLeaf(Decorator, TEXT("Decorator"), Depth + 2, Output);
			}

			if (Child.ChildComposite)
			{
				AppendBTComposite(Child.ChildComposite, Depth + 2, Output);
			}
			else if (UBTTaskNode* Task = Child.ChildTask)
			{
				AppendBTLeaf(Task, TEXT("Task"), Depth + 2, Output);
				for (UBTService* Service : Task->Services)
				{
					AppendBTLeaf(Service, TEXT("Service"), Depth + 3, Output);
				}
			}
		}
	}
}

// ============================================================================
// Asset loading + type labelling
// ============================================================================

UObject* FAssetDumper::LoadAsset(const FString& AssetPath)
{
	const FString Path = FBlueprintDumpUtils::NormalizeAssetPath(AssetPath);

	// Try the fully-qualified object path FIRST ("/Game/Foo/Bar" -> "/Game/Foo/Bar.Bar").
	// The old typed LoadObject<UBlueprint> rejected a package by cast; an untyped load
	// does not, and would hand back the UPackage — every dump would then report the
	// wrong class for an asset that loaded perfectly well.
	UObject* Asset = nullptr;
	if (!Path.Contains(TEXT(".")))
	{
		const FString ObjectPath = Path + TEXT(".") + FPaths::GetBaseFilename(Path);
		Asset = LoadObject<UObject>(nullptr, *ObjectPath);
	}
	if (!Asset)
	{
		Asset = LoadObject<UObject>(nullptr, *Path);
	}

	if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset))
	{
		Asset = Redirector->DestinationObject;
	}

	// Belt and braces: if a package still came back, reach inside for its asset
	if (UPackage* Package = Cast<UPackage>(Asset))
	{
		UObject* Found = nullptr;
		ForEachObjectWithOuter(Package, [&Found](UObject* Inner)
		{
			if (!Found && Inner->IsAsset())
			{
				Found = Inner;
			}
		}, /*bIncludeNestedObjects=*/false);
		Asset = Found;
	}

	return Asset;
}

FString FAssetDumper::GetAssetTypeLabel(const UObject* Asset)
{
	return Asset ? Asset->GetClass()->GetName() : TEXT("(null)");
}

// ============================================================================
// Type dispatch
// ============================================================================

FString FAssetDumper::DumpLoadedAsset(UObject* Asset)
{
	if (!Asset)
	{
		return FString();
	}

	// Blueprints keep their dedicated dumpers
	if (UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Asset))
	{
		return FAnimBPDumper::DumpAnimBPObject(AnimBP);
	}
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		return FBlueprintDumper::DumpBlueprintObject(Blueprint);
	}

	FString Output;
	const FString ClassName = Asset->GetClass()->GetName();

	// --- User-defined struct: the field list everything else is read through ---
	if (UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(Asset))
	{
		Output += FString::Printf(TEXT("=== Struct: %s (%s) ===\n\n"), *Struct->GetName(), *ClassName);
		Output += TEXT("=== Fields ===\n");
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Prop = *It;
			// GetCPPType puts the inner type in ExtendedTypeText: without it every
			// container field dumped as a bare "TArray" / "TMap".
			FString Extended;
			const FString BaseType = Prop->GetCPPType(&Extended, /*CPPExportFlags=*/0);
			Output += FString::Printf(TEXT("  %s : %s%s\n"),
				*Struct->GetAuthoredNameForField(Prop), *BaseType, *Extended);
		}
		Output += TEXT("\n");
		return Output;
	}

	// --- User-defined enum ---
	if (UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Asset))
	{
		Output += FString::Printf(TEXT("=== Enum: %s (%s) ===\n\n"), *Enum->GetName(), *ClassName);
		Output += TEXT("=== Enumerators ===\n");
		for (int32 i = 0; i < Enum->NumEnums(); ++i)
		{
			const FString RawName = Enum->GetNameStringByIndex(i);
			const FString DisplayName = Enum->GetDisplayNameTextByIndex(i).ToString();

			// BP enums store "NewEnumerator3"-style internal names; the display name is
			// what the author actually typed, so show both when they differ.
			FString Label = DisplayName.IsEmpty() ? RawName : DisplayName;
			if (!DisplayName.IsEmpty() && DisplayName != RawName)
			{
				Label += FString::Printf(TEXT("  (%s)"), *RawName);
			}

			Output += FString::Printf(TEXT("  %lld = %s\n"), Enum->GetValueByIndex(i), *Label);
		}
		Output += TEXT("\n");
		return Output;
	}

	// --- Curves: exact keys beat measuring a graph off a screenshot ---
	if (UCurveFloat* CurveFloat = Cast<UCurveFloat>(Asset))
	{
		Output += FString::Printf(TEXT("=== Curve: %s (%s) ===\n\n"), *CurveFloat->GetName(), *ClassName);
		AppendRichCurve(CurveFloat->FloatCurve, TEXT("  Curve"), Output);
		Output += TEXT("\n");
		return Output;
	}
	if (UCurveVector* CurveVector = Cast<UCurveVector>(Asset))
	{
		Output += FString::Printf(TEXT("=== Curve: %s (%s) ===\n\n"), *CurveVector->GetName(), *ClassName);
		static const TCHAR* Axes[3] = { TEXT("  X"), TEXT("  Y"), TEXT("  Z") };
		for (int32 i = 0; i < 3; ++i)
		{
			AppendRichCurve(CurveVector->FloatCurves[i], Axes[i], Output);
		}
		Output += TEXT("\n");
		return Output;
	}
	if (UCurveLinearColor* CurveColor = Cast<UCurveLinearColor>(Asset))
	{
		Output += FString::Printf(TEXT("=== Curve: %s (%s) ===\n\n"), *CurveColor->GetName(), *ClassName);
		static const TCHAR* Channels[4] = { TEXT("  R"), TEXT("  G"), TEXT("  B"), TEXT("  A") };
		for (int32 i = 0; i < 4; ++i)
		{
			AppendRichCurve(CurveColor->FloatCurves[i], Channels[i], Output);
		}
		Output += TEXT("\n");
		return Output;
	}

	// --- Animation assets: montages, sequences, composites (T4) ---
	if (UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Asset))
	{
		const UAnimMontage* Montage = Cast<UAnimMontage>(Anim);
		Output += FString::Printf(TEXT("=== %s: %s (%s) ===\n"),
			Montage ? TEXT("Montage") : TEXT("Animation"), *Anim->GetName(), *ClassName);
		Output += FString::Printf(TEXT("Skeleton: %s\n"),
			Anim->GetSkeleton() ? *Anim->GetSkeleton()->GetName() : TEXT("(none)"));
		Output += FString::Printf(TEXT("Length: %.4fs   RateScale: %.4f\n"),
			Anim->GetPlayLength(), Anim->RateScale);

		if (Montage)
		{
			AppendMontageStructure(Montage, Output);
		}
		else
		{
			Output += TEXT("\n");
		}

		AppendAnimNotifies(Anim, Output);
		AppendAnimCurves(Anim, Output);

		// Root motion, looping, interpolation and friends ride the generic diff rather
		// than a hand-listed set that would drift against the engine — but everything the
		// structured sections above already rendered is filtered out. Unfiltered, the
		// notify array alone re-printed every notify's instanced properties a second time.
		static const TArray<FString> SectionOwned =
		{
			TEXT("Notifies"), TEXT("CompositeSections"), TEXT("SlotAnimTracks"),
			TEXT("AnimNotifyTracks"), TEXT("BranchingPointMarkers"), TEXT("RawCurveData"),
		};

		FString Properties;
		AppendFilteredProperties(Asset, TEXT("  "), SectionOwned, Properties);

		Output += TEXT("=== Properties (differ from class defaults) ===\n");
		Output += Properties.IsEmpty()
			? TEXT("  (none - every property is at its class default)\n")
			: Properties;
		Output += TEXT("  (not listed = class default, or shown in a section above)\n\n");
		return Output;
	}

	// --- Data table ---
	if (UDataTable* DataTable = Cast<UDataTable>(Asset))
	{
		const UScriptStruct* RowStruct = DataTable->GetRowStruct();
		Output += FString::Printf(TEXT("=== DataTable: %s (%s) ===\n"), *DataTable->GetName(), *ClassName);
		Output += FString::Printf(TEXT("Row Struct: %s\n\n"),
			RowStruct ? *RowStruct->GetName() : TEXT("(none)"));

		const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();
		Output += FString::Printf(TEXT("=== Rows (%d) ===\n"), RowMap.Num());
		for (const TPair<FName, uint8*>& Row : RowMap)
		{
			Output += FString::Printf(TEXT("  %s\n"), *Row.Key.ToString());
			AppendStructValues(RowStruct, Row.Value, TEXT("    "), Output);
		}
		Output += TEXT("\n");
		return Output;
	}

	// --- Behavior tree ---
	if (UBehaviorTree* BT = Cast<UBehaviorTree>(Asset))
	{
		Output += FString::Printf(TEXT("=== BehaviorTree: %s (%s) ===\n"), *BT->GetName(), *ClassName);
		Output += FString::Printf(TEXT("Blackboard: %s\n\n"),
			BT->BlackboardAsset ? *BT->BlackboardAsset->GetName() : TEXT("(none)"));

		Output += TEXT("=== Tree ===\n");
		if (BT->RootNode)
		{
			for (UBTDecorator* Decorator : BT->RootDecorators)
			{
				AppendBTLeaf(Decorator, TEXT("Root Decorator"), 1, Output);
			}
			AppendBTComposite(BT->RootNode, 1, Output);
		}
		else
		{
			Output += TEXT("  (no root node)\n");
		}
		Output += TEXT("\n");
		return Output;
	}

	// --- Blackboard ---
	if (UBlackboardData* Blackboard = Cast<UBlackboardData>(Asset))
	{
		Output += FString::Printf(TEXT("=== Blackboard: %s (%s) ===\n"), *Blackboard->GetName(), *ClassName);
		Output += FString::Printf(TEXT("Parent: %s\n\n"),
			Blackboard->Parent ? *Blackboard->Parent->GetName() : TEXT("(none)"));

		const TArray<FBlackboardEntry>& Keys = Blackboard->GetKeys();
		Output += FString::Printf(TEXT("=== Keys (%d) ===\n"), Keys.Num());
		for (const FBlackboardEntry& Entry : Keys)
		{
			Output += FString::Printf(TEXT("  %s : %s%s\n"),
				*Entry.EntryName.ToString(),
				Entry.KeyType ? *Entry.KeyType->GetClass()->GetName() : TEXT("(untyped)"),
				Entry.bInstanceSynced ? TEXT("  [instance-synced]") : TEXT(""));
			AppendAuthoredProperties(Entry.KeyType, TEXT("      "), Output);
		}
		Output += TEXT("\n");
		return Output;
	}

	// --- Everything else: data assets and any unhandled class ---
	// The fallback is not a failure — with instanced recursion it is exactly what a
	// details-panel-authored asset needs. The header names the class either way, so
	// an unhandled type is visible rather than silently empty.
	const bool bIsDataAsset = Asset->IsA(UDataAsset::StaticClass());
	Output += FString::Printf(TEXT("=== %s: %s (%s) ===\n"),
		bIsDataAsset ? TEXT("DataAsset") : TEXT("Asset"), *Asset->GetName(), *ClassName);
	Output += FString::Printf(TEXT("Parent Class: %s\n\n"),
		Asset->GetClass()->GetSuperClass() ? *Asset->GetClass()->GetSuperClass()->GetName() : TEXT("(none)"));

	if (!bIsDataAsset)
	{
		Output += FString::Printf(
			TEXT("NOTE: no specialised dumper for '%s' - generic property dump below.\n\n"), *ClassName);
	}

	Output += TEXT("=== Properties (differ from class defaults) ===\n");
	const int32 Diffs = FBlueprintDumpUtils::DumpObjectPropertyDiffs(Asset, nullptr, TEXT("  "), Output);
	if (Diffs == 0)
	{
		Output += TEXT("  (none - every property is at its class default)\n");
	}
	Output += TEXT("  (not listed = class default)\n\n");

	return Output;
}

// ============================================================================
// DumpBPFolder — batch dump + manifest
// ============================================================================
//
// A per-asset manifest is the acceptance artifact for a batch run: it makes
// coverage auditable instead of assumed. Every asset the registry reports gets a
// row, including the ones that were skipped and why.

void FAssetDumper::ExecuteFolderCommand(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogTemp, Display, TEXT(""));
		UE_LOG(LogTemp, Display, TEXT("Usage: DumpBPFolder /Game/Path [-recursive] [-external] [-filter=ClassName]"));
		UE_LOG(LogTemp, Display, TEXT(""));
		UE_LOG(LogTemp, Display, TEXT("  Dumps every asset under a content path into a mirrored tree in"));
		UE_LOG(LogTemp, Display, TEXT("  Saved/BlueprintDumps/, plus a _manifest.txt listing every asset."));
		UE_LOG(LogTemp, Display, TEXT("  One-File-Per-Actor packages are skipped unless -external is passed."));
		UE_LOG(LogTemp, Display, TEXT("  Example: DumpBPFolder /TempestCombatFramework -recursive"));
		UE_LOG(LogTemp, Display, TEXT(""));
		return;
	}

	FString FolderPath = FBlueprintDumpUtils::NormalizeAssetPath(Args[0]);
	FolderPath.RemoveFromEnd(TEXT("/"));
	if (!FolderPath.StartsWith(TEXT("/")))
	{
		FolderPath = TEXT("/") + FolderPath;
	}

	bool bRecursive = false;
	bool bIncludeExternalActors = false;
	FString ClassFilter;
	for (int32 i = 1; i < Args.Num(); ++i)
	{
		const FString& Arg = Args[i];
		if (Arg.Equals(TEXT("-recursive"), ESearchCase::IgnoreCase) || Arg.Equals(TEXT("-r"), ESearchCase::IgnoreCase))
		{
			bRecursive = true;
		}
		else if (Arg.Equals(TEXT("-external"), ESearchCase::IgnoreCase))
		{
			bIncludeExternalActors = true;
		}
		else if (Arg.StartsWith(TEXT("-filter="), ESearchCase::IgnoreCase))
		{
			ClassFilter = Arg.RightChop(8);
		}
	}

	FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = Module.Get();

	// Plugin content is not always indexed yet — scan before asking
	AssetRegistry.ScanPathsSynchronous({ FolderPath }, /*bForceRescan=*/false);

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByPath(FName(*FolderPath), Assets, bRecursive);

	if (Assets.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("DumpBPFolder: no assets found under '%s'%s"),
			*FolderPath, bRecursive ? TEXT(" (recursive)") : TEXT(" (add -recursive for sub-folders)"));
		return;
	}

	// Mirror the content path under Saved/BlueprintDumps/
	const FString OutputRoot = FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("BlueprintDumps"));
	const FString RunRoot = FPaths::Combine(OutputRoot, FolderPath.RightChop(1)); // drop leading '/'

	UE_LOG(LogTemp, Display, TEXT("DumpBPFolder: %d asset(s) under '%s' -> %s"),
		Assets.Num(), *FolderPath, *RunRoot);

	TArray<FString> ManifestRows;
	ManifestRows.Reserve(Assets.Num());
	int32 DumpedCount = 0;
	int32 SkippedCount = 0;

	auto AddRow = [&ManifestRows](const FString& Name, const FString& Type, const FString& Status)
	{
		ManifestRows.Add(FString::Printf(TEXT("%-44s  %-28s  %s"), *Name, *Type, *Status));
	};

	for (const FAssetData& AssetData : Assets)
	{
		const FString AssetName = AssetData.AssetName.ToString();
		const FString RegistryClass = AssetData.AssetClassPath.GetAssetName().ToString();
		const FString PackagePath = AssetData.PackagePath.ToString();

		// One-File-Per-Actor packages are LEVEL CONTENT, not assets: a placed cube's
		// ActorLabel and FolderGuid. In ALS-Refactored they are 40% of the run (229 of
		// 570) and bury the AnimBPs worth reading. Skipped by default, VISIBLY -- pass
		// -external to include them, or DumpBP a single one by path.
		if (!bIncludeExternalActors
			&& (PackagePath.Contains(TEXT("/__ExternalActors__/"))
				|| PackagePath.Contains(TEXT("/__ExternalObjects__/"))))
		{
			AddRow(AssetName, RegistryClass, TEXT("skipped (external actor package; -external to include)"));
			++SkippedCount;
			continue;
		}

		// Class filter runs BEFORE loading — a filtered run should not pay to load
		if (!ClassFilter.IsEmpty() && !RegistryClass.Equals(ClassFilter, ESearchCase::IgnoreCase))
		{
			AddRow(AssetName, RegistryClass, TEXT("skipped (class filter)"));
			++SkippedCount;
			continue;
		}

		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			AddRow(AssetName, RegistryClass, TEXT("skipped (failed to load)"));
			++SkippedCount;
			continue;
		}
		if (Asset->IsA(UObjectRedirector::StaticClass()))
		{
			AddRow(AssetName, RegistryClass, TEXT("skipped (redirector)"));
			++SkippedCount;
			continue;
		}

		const FString TypeLabel = GetAssetTypeLabel(Asset);
		const FString Dump = DumpLoadedAsset(Asset);
		if (Dump.IsEmpty())
		{
			AddRow(AssetName, TypeLabel, TEXT("skipped (dumper produced no output)"));
			++SkippedCount;
			continue;
		}

		// Mirror the asset's own package path, not the run root, so -recursive keeps structure
		const FString MirroredDir = FPaths::Combine(OutputRoot, PackagePath.RightChop(1));
		IFileManager::Get().MakeDirectory(*MirroredDir, /*Tree=*/true);
		const FString OutputPath = FPaths::Combine(MirroredDir, AssetName + TEXT("_Dump.txt"));

		if (FFileHelper::SaveStringToFile(Dump, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddRow(AssetName, TypeLabel, FString::Printf(TEXT("dumped -> %s/%s_Dump.txt"), *PackagePath, *AssetName));
			++DumpedCount;
		}
		else
		{
			AddRow(AssetName, TypeLabel, TEXT("skipped (could not write file)"));
			++SkippedCount;
		}
	}

	// Manifest
	FString Manifest;
	Manifest += FString::Printf(TEXT("=== DumpBPFolder manifest: %s%s ===\n"),
		*FolderPath, bRecursive ? TEXT(" (recursive)") : TEXT(""));
	if (!ClassFilter.IsEmpty())
	{
		Manifest += FString::Printf(TEXT("Class filter: %s\n"), *ClassFilter);
	}
	Manifest += FString::Printf(TEXT("External actor packages: %s\n"),
		bIncludeExternalActors ? TEXT("included") : TEXT("skipped (-external to include)"));
	Manifest += FString::Printf(TEXT("Assets found: %d   dumped: %d   skipped: %d\n\n"),
		Assets.Num(), DumpedCount, SkippedCount);
	Manifest += FString::Printf(TEXT("%-44s  %-28s  %s\n"), TEXT("ASSET"), TEXT("TYPE"), TEXT("STATUS"));
	Manifest += FString::Join(ManifestRows, TEXT("\n"));
	Manifest += TEXT("\n");

	IFileManager::Get().MakeDirectory(*RunRoot, /*Tree=*/true);
	const FString ManifestPath = FPaths::Combine(RunRoot, TEXT("_manifest.txt"));
	if (FFileHelper::SaveStringToFile(Manifest, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Display, TEXT("DumpBPFolder: manifest written to %s"), *ManifestPath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("DumpBPFolder: failed to write manifest to %s"), *ManifestPath);
	}

	UE_LOG(LogTemp, Display, TEXT("DumpBPFolder: %d dumped, %d skipped (of %d found)"),
		DumpedCount, SkippedCount, Assets.Num());
}
