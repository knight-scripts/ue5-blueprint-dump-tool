# Blueprint Dump Tool — Improvement Plan

> Single roadmap for the plugin. Consolidates the former `IMPROVEMENT_MILESTONES.md`
> (feature roadmap) and the 2026-07-02 code-review fix plan (correctness + fidelity gaps)
> into one document, so there is only one plan to carry.
>
> **Structure & order of work:** Phase 0 (correctness — do first) → Milestone 1 (data-pin
> wiring, with the review's fidelity gaps folded in) → Milestones 2–13 (features) →
> Phase 2 (hygiene, anytime). Phase 0 must land before M1: M1's recursive expression
> resolution multiplies the walker workload, and the Phase 0 fixes determine whether that
> work produces truth or noise.
>
> **Copies:** this plugin has a canonical standalone repo and may be copied into a host
> project's `Plugins/` folder for in-editor testing. There is no submodule/symlink —
> changes must be mirrored manually between copies. Develop in the standalone repo, copy
> into the host project to test.

## Current State (v1.0 — commit 19e210f)

### What Works Well
- **AnimBP pose chain walks** — Full backward walk from Root through all anim nodes with correct depth/nesting
- **State machines** — States, transitions with reconstructed boolean rule expressions (recursive resolution up to depth 10), blend type/time/priority, auto-rule flags, state aliases
- **Cached pose chains** — Including orphaned branches not reachable from root walk
- **Interface layers + self-linked layers + monolithic AnimBP support**
- **Blueprint exec chain walking** — Event graphs, functions, branches, switches with per-branch visited sets (no false cycles)
- **Expression resolution** — Recursive for operators/pure nodes (depth 4 for data pins, depth 10 for transitions), BP enum display name fallback
- **Plugin path normalization** — Handles /All/, /Plugins/ prefixes, .AssetName suffixes
- **Thread Safe Update functions** — Fully captured when implemented in Blueprint (proven on GASP: 700+ lines of function bodies, branches, data flow). Empty for C++-only projects (ALS) — this is correct behavior, not a gap.

### Validated On
- **ALS Refactored** — 24 dump files (10 AnimBPs, 4 Blueprints, monolithic + segmented). All structures captured. Empty function sections are correct — ALS logic lives in C++ (`AlsAnimationInstance`).
- **GASP** — Full dump with thread-safe functions, chooser references, complex transition rules. ~700 lines of blueprint logic captured including `BlueprintThreadSafeUpdateAnimation`, `Update_Logic`, `Update_EssentialValues`, etc.
- **A production project (private)** — additional real-world AnimBPs and Blueprints.

### Cross-Version Compatibility
Built for UE 5.7.3 but **works on 5.5.1** (tested with ALS Refactored). UE shows a version warning, no actual problems. See Milestone 12.

---

## Architecture Insight: C++ vs Blueprint Logic Split

Understanding why some dumps show rich function bodies and others show empty sections:

| Project | Thread Safe Logic | AnimBP Functions | Plugin Captures |
|---------|------------------|-----------------|-----------------|
| **GASP** | Blueprint (`BlueprintThreadSafeUpdateAnimation`) | Full implementations in BP | Everything — complete function bodies with data flow |
| **ALS Refactored** | C++ (`NativeThreadSafeUpdateAnimation`) | None — all in `AlsAnimationInstance.cpp` | Structure only — correct, C++ not dumpable from BP |
| **KLS** | Mixed — C++ base + BP overrides | Some BP functions | Partial — captures BP parts, C++ invisible |

**Key**: The plugin captures 100% of what exists on the Blueprint/AnimBP side. Projects that put logic in C++ will always have less visible in dumps. This is architectural, not a plugin limitation.

---

## Phase 0 — Correctness fixes (DO FIRST, before Milestone 1)

**Core conclusion:** the tool is solid, but one correctness bug already corrupts dump output
today (false `(cycle)` on shared nodes) and it sits directly under Milestone 1. These fixes
land before M1.

### 0.1 False `(cycle)` on shared nodes in `BuildExpressionFromPin` ⚠ TOP PRIORITY
**File:** `BlueprintDumpUtils.cpp:385-389` (and knot handling at `:392-402`)

**Bug:** One `Visited` set is threaded through the whole expression walk and nodes are
marked permanently. BP data flow is a DAG — one `Get Speed` node routinely feeds multiple
consumers. A rule authored as `Speed > 200 && Speed < 400` from a single Get node dumps as:

```
Speed > 200 && (cycle) < 400
```

Same failure through shared Knots and shared pure-function outputs.

**Key facts:**
- Valid BP data graphs CANNOT contain true cycles (compiler rejects them) → on real
  assets `Visited` produces ONLY false positives. `MaxDepth` already guards pathology.
- The changelog claims "per-branch visited sets — eliminates false cycles" — that fixed
  the EXEC walker only. The EXPRESSION walker has the same disease, uncured.
- M1 Gap 1.1 (recurse into pure-function args) makes recursion much deeper → far more
  shared subexpressions → `(cycle)` spam everywhere. This fix is a prerequisite for M1.

**Fix options (pick one):**
- (a) **Drop `Visited` from the data walk entirely**, rely on `MaxDepth`. Honest, since
  valid data graphs are acyclic. Simplest.
- (b) **Path-based visited**: add on entry, REMOVE before every return. NOTE: the
  function has ~5 early-return paths (max-depth `:378`, knot `:392`, PropertyAccess
  `:404`, VariableGet `:437`, plus the operator/fallback returns) — use a small RAII
  scope guard struct rather than manual removes, or you WILL miss one.

**Verify:** grep existing GASP dumps for `(cycle)` (expected hits in complex transition
rules). After fix: re-dump, confirm gone; author a test BP with one Get feeding both
sides of an AND and confirm both operands print.

### 0.2 `WalkExecChain` Knot bypass skips cycle detection → infinite recursion
**File:** `BlueprintDumpUtils.cpp:692-706`

Knots are followed BEFORE the `Visited` check and never added to the set. An exec
reroute loop (knot→knot cycle — the editor lets you wire this) recurses until stack
overflow = editor crash mid-dump. The pose walker does NOT have this hole (its Visited
check comes first, `AnimBPDumper.cpp:260-263`).

**Fix:** check/add knots in `Visited` before following through (one line each).

### 0.3 No recursion depth cap in either walker (scale risk)
**Files:** `BlueprintDumpUtils.cpp:684` (WalkExecChain), `AnimBPDumper.cpp:258` (WalkPoseChain)

`WalkExecChain` recurses one stack frame per node even on LINEAR chains, and each
multi-exec-output node copies the full `Visited` set per branch (`:803`). Expansion
target = dumping ALS/GASP-scale third-party projects; a macro-heavy or generated graph
with thousands of sequential nodes = stack overflow.

**Fix:** depth/node-count budget (e.g., 2000 nodes per walk), print
`(truncated after N nodes)` when hit. Cheap insurance, never triggers on healthy assets.

### 0.4 Disabled nodes dumped as live logic
**Files:** all walkers — `WalkExecChain`, `WalkPoseChain`, event collection
(`BlueprintDumpUtils.cpp:928-935`), transition collection.

No `IsNodeEnabled()` check anywhere. Dumps are consumed as GROUND TRUTH — presenting
disabled experiment leftovers as active logic is a silent lie.

**Fix:** skip disabled nodes, or better: annotate `[DISABLED]` (skipping hides that
something is there; annotation preserves the full picture).

### 0.5 Main AnimGraph selection is order-dependent
**File:** `AnimBPDumper.cpp:156-164`

Takes the FIRST `UAnimationGraph` from `GetAllGraphs()`. In monolithic ABPs, self-linked
layer graphs are ALSO `UAnimationGraph` with the same outer — if one precedes the main
graph in creation order, the dump presents a layer as "the AnimGraph" and files the real
one under "Layer Functions". Has held on ALS/GASP so far by luck.

**Fix:** prefer name-based selection (`Graph->GetFName() == TEXT("AnimGraph")`), keep
current first-match as fallback.

### 0.6 Silent omission on pose-chain revisits
**File:** `AnimBPDumper.cpp:260-263` (+ pin-label print at `:320-323`)

When a node is already visited, `WalkPoseChain` returns silently — a blend whose second
input reaches an already-dumped subtree prints the `[PinLabel]` header followed by
NOTHING, which reads as "unconnected".

**Fix:** print `(-> see above: <node label>)` on revisit instead of silence.

### 0.7 Unchecked `LinkedTo[0]` dereferences (crash on corrupt assets)
**Files:** `BlueprintDumpUtils.cpp:636` (GetDataInputSummary), `AnimBPDumper.cpp:782`
(FormatDataPins), `AnimBPDumper.cpp:842` (FindEntryStateName)

Stale/null link entries exist in mildly corrupted assets. The walkers null-check
(`LinkedPin && LinkedPin->GetOwningNode()`); these three summarizer spots don't.

**Fix:** same null-check pattern at all three sites.

---

## Milestone 1: Data Pin Source Wiring (HIGHEST PRIORITY — #1)

### Problem
Anim nodes and BP function nodes have data pins (colored wires — float, bool, enum, struct) that drive behavior at runtime. This is the **single biggest gap** in the plugin — proven by side-by-side comparison of a GASP dump vs a hand-written GASP analysis.

**Current state:**
- **Static values**: Captured well (`Alpha=0.750000`, `Play Rate=1.200000`)
- **Simple variable connections**: Captured (`Alpha=ExplorationPlayRate`)
- **Everything else**: Lost — math chains, Property Access paths, function arguments, return values, string literals, boolean operands

### GASP Validation: 5 of 6 Functions NOT Recreatable From Dump Alone

Tested 6 representative GASP functions against hand-written analysis. Results:

| Function | Recreatable? | Blocking Gap |
|----------|-------------|--------------|
| `Update_EssentialValues` | NO | Property Access opaque, Branch condition missing |
| `Get_DynamicPlayRate` | NO | All 4 curve name strings lost, final Lerp formula invisible |
| `Get_MMBlendTime` | NO | All 4 Return values missing, Branch threshold missing |
| `BlueprintThreadSafeUpdateAnimation` | PARTIAL | BooleanAND operands invisible |
| `Update_MotionMatching` | YES | Asset refs + explicit function calls survive well |
| `Update_MovementDirection` | NO | InRange boundaries, Select options, BooleanOR condition all lost |

**Only `Update_MotionMatching` was fully recreatable** — because it uses asset references and explicit named function calls, not math/Property Access chains.

### The 6 Specific Sub-Gaps (ordered by impact)

#### Gap 1.1: Pure Function Arguments (CRITICAL)
```
DUMP:  Return (Return Value=Lerp())
REAL:  Lerp(1.0, Clamp(SafeDivide(Speed2D, Clamp(SpeedCurve, 1.0, 999.0)), Min, Max), AlphaCurve)
```
`SafeDivide()`, `Greater_DoubleDouble()`, `InRange_FloatFloat()`, `Lerp()` — all show the function name but ZERO input arguments. The entire math formula is lost.

**Root cause:** `BuildExpressionFromPin` resolves the OUTPUT pin of a pure node into a function call string, but doesn't recurse into its INPUT pins to show what's being computed.

**Fix:** When encountering a `UK2Node_CallFunction` that's a pure function (no exec pins), recurse into its input pins to build the full expression: `Lerp(A, B, Alpha)` where A/B/Alpha are themselves resolved recursively.

#### Gap 1.2: String Literal Pin Values (CRITICAL)
```
DUMP:  GetCurveValueFromAnimation()    ← appears 4 times identically
REAL:  GetCurveValueFromAnimation("Enable_Warping", AnimTime)
       GetCurveValueFromAnimation("MoveData_Speed", AnimTime)
       GetCurveValueFromAnimation("MaxDynamicPlayRate", AnimTime)
       GetCurveValueFromAnimation("MinDynamicPlayRate", AnimTime)
```
String/Name default values on input pins (curve names, CVar names, socket names, tag names) are not included in function call summaries.

**Root cause:** `GetDataInputSummary` for `UK2Node_CallFunction` currently only shows connected pins and non-default values. String/Name literals that ARE the default value for that specific pin instance (not the AutogeneratedDefaultValue) are being filtered out.

**Fix:** For `UK2Node_CallFunction`, include ALL non-empty string/name input pin values (not just non-default), since these ARE the meaningful parameters. The AutogeneratedDefaultValue filter should not apply to string parameters — an empty string is the default, any non-empty string is user-specified data.

#### Gap 1.3: Return Node Values (CRITICAL)
```
DUMP:  -> Return            (no value)
REAL:  -> Return 0.5        (standard locomotion blend)
```
`Get_MMBlendTime` has 4 returns. All show `Return` with no value. The function's entire purpose is returning specific numbers.

**Root cause:** `WalkExecChain` identifies Return nodes but doesn't read the ReturnValue input pin.

**Fix:** When encountering a Return node, read its `ReturnValue` input pin. If it has a default value, show it: `Return (0.500000)`. If it's connected, resolve with `BuildExpressionFromPin`: `Return (Lerp(1.0, ClampedRate, AlphaCurve))`.

#### Gap 1.4: BooleanAND/BooleanOR Operands (HIGH)
```
DUMP:  Branch (Condition=BooleanAND())
REAL:  Branch (Condition=HasOwningActor && UseThreadSafeUpdateAnimation)
```
Boolean operator nodes show the operator but not WHAT is being combined.

**Root cause:** `BuildExpressionFromPin` handles `BooleanAND` / `BooleanOR` by name but the current implementation may not be resolving their input pins. These are `UK2Node_CommutativeAssociativeBinaryOperator` or similar — need to walk their A/B input pins.

**Fix:** In `BuildExpressionFromPin`, when encountering BooleanAND/BooleanOR, resolve both input operands recursively and join with `&&` / `||`. Already have the operator symbol map — just need to ensure the input pin walk works.

#### Gap 1.5: Property Access Deep Paths (HIGH)
```
DUMP:  Set CharacterTransform = Property Access
REAL:  Set CharacterTransform = CharacterProperties.ActorTransform (thread safe)
```
Property Access nodes read struct member chains but we only show "Property Access" or "Value".

**Root cause:** `GetNodeTitle(FullTitle)` doesn't include the property path on a second line for AnimBP Property Access nodes. The actual path is stored internally in the node's `PropertyAccess` member.

**Fix (verified 5.7):** read `UK2Node_PropertyAccess::GetTextPath()` / `GetPath()` for the path. Its header sits in a plugin Private/ folder, so read the `TextPath` / `Path` UPROPERTYs via reflection — no fragile private include or module dependency needed. Fallback to current "Value" behavior if the cast/reflection fails.

#### Gap 1.6: Branch Condition Sources (MEDIUM)
```
DUMP:  Branch (Condition=MovementState == Moving)     ← this works!
DUMP:  Branch                                          ← but this is empty
```
Some Branch nodes show their condition (when wired to a simple expression). Others show nothing (when the condition pin wiring is complex or uses a cached variable).

**Root cause:** `WalkExecChain` reads the Branch node's Condition pin via `GetDataInputSummary`, but when the condition comes from a multi-hop chain, the summary may be empty.

**Fix:** Always resolve the Condition pin of Branch nodes through `BuildExpressionFromPin` (depth 4). If the result is non-empty, show it. This already partially works for some cases — need to make it consistent.

### Implementation Priority Within M1

1. **Gap 1.3 (Return values)** — Easiest fix, huge payoff. One pin read on Return nodes.
2. **Gap 1.2 (String literals)** — Change filter logic for string/name pins. Moderate effort.
3. **Gap 1.1 (Pure function args)** — Recursive input pin resolution. Medium effort, biggest single improvement.
4. **Gap 1.4 (Boolean operands)** — Verify BuildExpressionFromPin handles these; likely small fix.
5. **Gap 1.6 (Branch conditions)** — Ensure consistent Condition pin resolution. Small fix.
6. **Gap 1.5 (Property Access paths)** — Needs module dependency research. Medium effort.

### Impact
Fixing gaps 1.1-1.4 alone would make **4 of 5 previously non-recreatable GASP functions recreatable from dump output**. This changes the plugin from "structural skeleton" to "functional specification" — the difference between a map of a building and a blueprint with dimensions.

### Evidence: What Already Works (and why)
`Update_MotionMatching` was fully recreatable because it uses:
- Asset references (Chooser names survive as string properties on nodes)
- Explicit `UK2Node_CallFunction` calls (function names + named output pins survive)
- Direct variable SETs (target variable names survive)

These patterns work because they're node-level properties, not pin-level data flow. The fix is extending the same resolution quality to pin-level connections.

### Fidelity gaps from the code review (fold into M1)

These are the same failure category as Gap 1.2 above ("dump shows a default, runtime does
something else") and were surfaced by the full source review; they extend M1's gap list.

> **✅ Verified 5.7 API (decoded from engine source, 2026-07-05).** The symbols below are
> confirmed against `AnimGraphNode_Base.{h,cpp}`, the editor `AnimGraphNode_BlendStack.h`,
> `K2Node_PropertyAccess.h`, and `AnimGraphNodeBinding{,_Base}.h` at UE `ref=5.7`. Where a gap
> writeup below still names a different symbol, **this block wins** — two earlier guesses were
> wrong (1.1b, 1.2b). All against 5.7 specifically (APIs drift across versions).
>
> - **Read any node's settings (1.5b-A):** `UAnimGraphNode_Base::GetFNodeProperty()` (public
>   `UE_API`) returns the `FStructProperty` of the inner `FAnimNode_*`; `GetFNode()` returns
>   `Property->ContainerPtrToValuePtr<FAnimNode_Base>(this)`. Iterate the struct's fields by
>   reflection, print non-transient values → works for **every** anim node. The Blend Stack
>   editor node stores its runtime node as `UPROPERTY FAnimNode_BlendStack Node`.
> - **Function bindings (1.1b):** `UAnimGraphNode_Base::GetBoundFunctionsInfo(TArray<TPair<FName,FName>>&)`,
>   or read the three `UPROPERTY FMemberReference` members `InitialUpdateFunction` /
>   `BecomeRelevantFunction` / `UpdateFunction`. (NOT `FAnimNodeFunctionRef` — wrong guess.)
> - **Pin property bindings (1.2b):** ⚠ the inline node map is **`PropertyBindings_DEPRECATED` in
>   5.7.** Real path: `GetBinding()` → `UAnimGraphNodeBinding` (default impl
>   `UAnimGraphNodeBinding_Base`, storing `TMap<FName, FAnimGraphNodePropertyBinding> PropertyBindings`);
>   each `FAnimGraphNodePropertyBinding` carries `TArray<FString> PropertyPath` (the display target)
>   + a `Type`. Map pin → binding name via `GetPinBindingInfo(...)` / `IsPinBindable()`. The impl
>   header is **Private**, so read `PropertyBindings` via reflection, not by including it.
> - **Property Access deep path (1.5):** `UK2Node_PropertyAccess::GetTextPath()` (display `FText`)
>   or `GetPath()` (`TArray<FString>`). Header lives in a plugin **Private/** folder → read the
>   `TextPath` / `Path` UPROPERTYs by reflection (no fragile private include, no module dep).
> - **Blend Stack sub-graph (1.5b-C):** `UAnimGraphNode_BlendStack_Base::GetSubGraphs()` exposes
>   the per-sample `BoundGraph`. Have `WalkPoseChain` recurse `GetSubGraphs()` (generalizes to any
>   node hosting sub-graphs) instead of stopping at a mute leaf.
>
> Portability: `GetFNodeProperty`/`GetFNode`/`GetBoundFunctionsInfo`/`GetPinBindingInfo` are public
> `UE_API` on `UAnimGraphNode_Base` (call directly); the binding-impl and Property-Access classes
> are Private/Internal-module → prefer reflection over including their headers.

#### 1.1b Anim node FUNCTION bindings not dumped
`On Initial Update` / `On Become Relevant` / `On Update` member-function bindings on anim
graph nodes are invisible to the dump — e.g. a Control Rig node whose reset flag is driven
via its On Become Relevant function is a load-bearing piece of setup the dump is blind to.
GASP uses these bindings heavily.

**Where (verified 5.7):** `UAnimGraphNode_Base::GetBoundFunctionsInfo(...)`, or the
`FMemberReference` members `InitialUpdateFunction` / `BecomeRelevantFunction` / `UpdateFunction`.
Print as e.g. `[OnBecomeRelevant: <FunctionName>]` after the node label in `WalkPoseChain`.

#### 1.2b Pin PROPERTY bindings not dumped (silently wrong data)
UE5 anim node pins can be bound via the "Bind" dropdown (in 5.7 stored on the node's
`GetBinding()` object, NOT the deprecated inline `PropertyBindings` map — see Verified 5.7 API
above) instead of wires. `FormatDataPins` (`AnimBPDumper.cpp:739`) reads only `LinkedTo` and
`DefaultValue` — a bound pin dumps its DEFAULT while runtime uses the binding. Worst
failure mode for a ground-truth tool.

**First step:** verification dump on an asset with a known pin binding to confirm the
blind spot, then print `PinName=[bound: PropertyPath]`.

#### 1.3b State aliases: names only, aliased states missing
`AnimBPDumper.cpp:398-423` lists alias names but not `GetAliasedStates()` /
global-alias flag. For ABP contract checks ("does state X have an edge from Y"), an
unexpanded alias is exactly the edge you can't verify. GASP uses aliases heavily.

**Fix:** cast to `UAnimStateAliasNode`, print
`Alias: <name> -> {StateA, StateB}` or `-> [any state]` when global.

#### 1.4b Smaller completeness items
- Sequence player **sync group** settings (group name, role) not dumped.
- Transition **AutomaticRuleTriggerTime** and **shared-rule names** not indicated
  (`AnimBPDumper.cpp:466-500` prints AutoRule flag + blend + priority only).
- `DumpAnimBP` on a wrong-type asset says "could not load" — load as `UObject` and
  report the actual class ("not an AnimBP, got Blueprint") instead.

#### 1.5b Blend Stack node under-dumped — settings, live bindings, and dynamic nature all lost

**Symptom.** A Blend Stack node — the head-of-graph delivery surface in C++-driven /
Motion Matching setups (GASP, and any project that feeds the stack from code) — renders as
a single mute leaf. A representative dump line:
```
\-- Blend Stack (Animation Time=0.000000, Loop=False)
```
Two static-looking pins, no children, no settings — even though it hosts every requested
animation. Three separate gaps stack up here:

**(A) Node SETTINGS (the anim-node backing struct) are 100% invisible — general gap, Blend Stack is the worst case.**
`FormatDataPins` (`AnimBPDumper.cpp:739`) walks only visible graph *pins* and explicitly
skips `Pin->bHidden` (`:756`). It never reads the node's inner `FAnimNode_*` struct.
`FAnimNode_BlendStack` (engine `AnimNode_BlendStack.h`, plus its `_Standalone` parent) has
~25 `EditAnywhere` properties and **nearly all are `meta=(PinHiddenByDefault)`** → no
visible pin exists unless hand-exposed → all skipped. Lost entirely:
- `MaxActiveBlends` (default 4) — **the defining setting; `0` disables the stack.**
- `bUseInertialBlend` + `InertialBlendNodeTag` — whether blends route through inertialization.
- `BlendTime` (0.2), `BlendProfile`, `BlendOption`, `bStoreBlendedPose`,
  `bResetOnBecomingRelevant`, `MaxBlendInTimeToOverrideAnimation`,
  `PlayerDepthBlendInTimeMultiplier`, `NotifyRecencyTimeOut`, `MaxAnimationDeltaTime`,
  `bShouldFilterNotifies`, sync group/role/method, blendspace + stitch settings, …

  This is NOT Blend-Stack-specific: **every** anim node loses its unexposed struct defaults
  (SequencePlayer loop / start-position / blend-profile, etc.). Blend Stack just makes it
  total because it is *all* hidden pins. SequencePlayer's asset+play-rate survive only
  because they ride the node TITLE (`GetNodeLabel` → `GetNodeTitle`), not because the tool
  reads the struct.
  **Fix (new capability, lifts every node):** for `UAnimGraphNode_Base`, get the inner
  `FAnimNode_*` via `GetFNodeProperty()` / `GetFNode()`, iterate that struct's
  editable properties and print non-transient values (skip `Transient`, skip pose-link
  arrays like `PerSampleGraphPoseLinks`). Objects by name, enums via `ResolveEnumPinValue`.
  Gate verbosity behind a flag / per-node allowlist so common nodes stay terse. Larger than
  M1's pin-wiring scope — its own capability, same "dump ≠ ground truth" motivation.

**(B) Exposed/bound pins dump stale static defaults, not the binding — a direct instance of 1.2b.**
When a Blend Stack's pins are property-bound (the "Bind" dropdown, e.g. driven from C++ each
frame), the dump shows their static defaults instead of the binding. A node whose Animation
Time / Loop / play-rate are runtime-driven prints `Animation Time=0.000000, Loop=False` —
asserting it plays from t=0 non-looping, the *opposite* of runtime truth — and pins whose
`DefaultValue == AutogeneratedDefaultValue` filter out entirely (`:802`), so most bound pins
don't appear at all. Exactly 1.2b (read via `GetBinding()`, not the deprecated `PropertyBindings` map).
**Fix per 1.2b:** print `PinName=[bound: PropertyPath]`.

**(C) Reads as a dead-end; its dynamic, C++-fed nature is unstated.**
Blend Stack owns `PerSampleGraphPoseLinks` (it *can* host a per-sample input sub-graph), but
when driven by library calls (`BlendTo` / `ForceBlendNextUpdate`) from an update function —
as GASP does (`GASP_Dumps/SandboxCharacter_Mover_ABP_Dump.txt:205` shows
`Blend Stack (Blend Time=0.000000)`, also a terminal leaf) — nothing in the pose-chain
output links the leaf to that update function or to the data feeding it. A reader sees a
mute dead-end and cannot tell it is the delivery mux for every requested clip.
**Fix (cheap, rides 1.1b):** when a node has a bound `On Update` function, annotate the
label, e.g. `Blend Stack [OnUpdate: <FunctionName>]`.

**Verification-first:** before coding, add a throwaway dump that, for the Blend Stack node,
walks the inner struct (`GetFNodeProperty()`) + the `GetBinding()` object, to confirm (A)/(B)
exactly and catch any already-exposed pins the tool may be mis-reading.

**Priority.** (B) rides 1.2b and (C) rides 1.1b — do them with those. **(A) is the new item
and the highest fidelity payoff:** it is the only path by which node settings
(`MaxActiveBlends`, `bUseInertialBlend`, blend profiles) ever reach a dump, and it fixes
every anim node at once.

---

## Milestone 2: Batch Dump + File Output (HIGH PRIORITY)

### Problem
Currently one asset at a time via console command. Dumping an entire project requires manual repetition.

### Proposed Commands
```
ScanProject /Game/                      → Discovery scan ONLY — lists all assets, shows what's supported
DumpProject /Game/                      → Full dump (scan + dump all supported assets)
DumpAllAnimBPs /Game/Characters/        → Dumps every AnimBP in folder (recursive)
DumpAllBPs /Game/Characters/            → Dumps every Blueprint in folder (recursive)
```

### Step 1: Asset Discovery Manifest (scan before dump)

Before dumping anything, scan the folder and produce a **manifest** listing EVERY asset found, grouped by type, with clear markers for what we support vs what we skip. This serves two purposes:
1. User sees exactly what will be dumped and what won't
2. We see what asset types exist that we don't support yet — drives future milestones

**Example output:**
```
=== Project Scan: /Game/Characters/ ===
Found 47 assets in 12 folders

SUPPORTED (will dump):
  AnimBlueprint (6):
    /Game/Characters/Animation/ABP_Character
    /Game/Characters/Animation/ABP_Character_Overlay
    /Game/Characters/Animation/ABP_Camera
    /Game/Characters/Animation/ABP_Enemy_Base
    /Game/Characters/Animation/ABP_Enemy_Melee
    /Game/Characters/Animation/ABP_Enemy_Ranged
  Blueprint (4):
    /Game/Characters/BP_PlayerCharacter
    /Game/Characters/BP_EnemyBase
    /Game/Characters/Abilities/BP_DashAbility
    /Game/Characters/Abilities/BP_BlockAbility

NOT YET SUPPORTED (skipped):
  AnimationSequence (18):
    /Game/Characters/Animation/Locomotion/A_Run_Fwd
    /Game/Characters/Animation/Locomotion/A_Run_Bwd
    ... (16 more)
  BlendSpace (5):
    /Game/Characters/Animation/BS_WalkRun_Fwd
    ... (4 more)
  BlendSpace1D (2):
    /Game/Characters/Animation/BS1D_Lean
    /Game/Characters/Animation/BS1D_AimOffset
  ChooserTable (3):
    /Game/Characters/Animation/Choosers/CT_Locomotion
    /Game/Characters/Animation/Choosers/CT_Starts
    /Game/Characters/Animation/Choosers/CT_Stops
  AnimMontage (7):
    /Game/Characters/Animation/Montages/AM_Attack_Light
    ... (6 more)
  ControlRigBlueprint (1):
    /Game/Characters/Animation/CR_FootIK
  PoseSearchDatabase (2):
    /Game/Characters/Animation/MM/PSD_Locomotion
    /Game/Characters/Animation/MM/PSD_Stops
  Skeleton (1):
    /Game/Characters/Meshes/SK_Character
  SkeletalMesh (1):
    /Game/Characters/Meshes/SKM_Character
  PhysicsAsset (1):
    /Game/Characters/Meshes/PA_Character
  BlendProfile (1):
    /Game/Characters/Animation/BP_UpperBody
  Other (1):
    /Game/Characters/Animation/CurveTable_Locomotion (CurveTable)

SUMMARY: 10/47 assets will be dumped (6 AnimBP + 4 BP)
         37 assets skipped (18 AnimSeq, 7 BlendSpace, 3 Chooser, 7 Montage, 2 other)
```

This manifest immediately shows:
- **18 AnimationSequences** — that's M4 territory. Lots of animation data we're missing.
- **7 BlendSpaces** — M5. Axes and samples invisible.
- **3 ChooserTables** — M6. Motion Matching selection logic invisible.
- **7 AnimMontages** — future milestone. Combat/ability logic invisible.
- **1 ControlRigBlueprint** — future. IK setup invisible.
- **2 PoseSearchDatabases** — future. MM database contents invisible.

### Step 2: Dump Supported Assets

After manifest, dump all supported assets using existing `DumpAnimBP`/`DumpBlueprint` logic.

### Output Options
- Individual files (current behavior, one per asset)
- Combined file with table of contents
- Summary mode (structure only, no state details) vs full mode
- Manifest-only mode (`ScanProject` — no dumping, just the discovery report)

### Implementation
Use `FAssetRegistryModule` to enumerate ALL assets in a content path. Group by `UClass`:
- `UAnimBlueprint` → dump with `FAnimBPDumper`
- `UBlueprint` → dump with `FBlueprintDumper`
- Everything else → list in "NOT YET SUPPORTED" with class name and count

As we add new dumpers (M4: AnimSequence, M5: BlendSpace, M6: Chooser), they plug into the same scan loop — assets move from "NOT YET SUPPORTED" to "SUPPORTED" automatically.

### Impact
- Enables project-wide analysis in one command
- Foundation for cross-referencing (M8)
- **Self-documenting roadmap**: the manifest tells us exactly which asset types matter for each project, sorted by count. 18 AnimSequences > 3 ChooserTables — confirms M4 before M6 is the right priority.
- Users see immediately what they're getting and what they're missing — transparent about plugin capabilities

---

## Milestone 3: JSON Output Mode (HIGH PRIORITY)

### Problem
Plain text is human-readable but not queryable. Can't diff, search programmatically, or feed to tools efficiently.

### Proposed Approach
Add `--json` flag to existing commands:
```
DumpAnimBP /Game/Path/ABP --json
```

### JSON Structure
```json
{
  "type": "AnimBP",
  "name": "AB_Als_Standing",
  "skeleton": "SK_Als",
  "parentClass": "AlsLinkedAnimationInstance",
  "graphs": ["AnimGraph", "EventGraph"],
  "animGraph": {
    "poseChain": {
      "node": "OutputPose",
      "children": [
        {
          "node": "RefreshRotateInPlace",
          "type": "ThreadSafeUpdateFunction",
          "children": [...]
        }
      ]
    },
    "stateMachines": [{
      "name": "Standing States",
      "entryState": "Idle",
      "states": [...],
      "transitions": [{
        "from": "Idle",
        "to": "Move",
        "condition": "False",
        "blendType": "Standard",
        "blendTime": 0.20,
        "priority": 1
      }]
    }],
    "cachedPoses": [...]
  },
  "functions": [...],
  "eventGraph": [...]
}
```

### Impact
Enables: programmatic diffing, cross-reference building, LLM-optimized context feeding, custom visualization tools, CI/CD integration (detect AnimBP regressions).

---

## Milestone 4: Animation Sequence Metadata (MEDIUM PRIORITY)

### Problem
Dumps show `Sequence Player 'A_Als_Sprint'` but nothing about the animation itself.

### What to Capture
| Property | Why It Matters |
|----------|---------------|
| Duration (seconds) | Understanding timing, gate thresholds, blend windows |
| Root motion (yes/no + type) | Fundamental architectural decision per animation |
| Additive type (None/Local/Mesh) | Determines how animation stacks |
| Curves present (list names) | Distance curves, enable_warping, foot sync — critical for locomotion |
| Sync markers (list names) | Foot plant timing for sync groups |
| Notify events (list) | AnimNotify triggers that drive state changes |
| Loop flag | Whether animation is designed to repeat |

### Proposed Commands
```
DumpAnim /Game/Animations/A_Als_Sprint          → Single animation details
DumpAnimFolder /Game/Animations/Locomotion/     → All animations in folder
```

### Inline Enhancement
When dumping AnimBP, optionally append metadata inline:
```
Sequence Player 'A_Als_Sprint' (3.2s, Loop, RootMotion, Curves:[Distance,FootPlant_L,FootPlant_R])
```

### Impact
Knowing animation duration, curves, and root motion status is essential for understanding timing relationships between states. Currently requires opening each animation individually in the editor.

---

## Milestone 5: Blend Space Details (MEDIUM PRIORITY)

### Problem
`Blendspace Player 'BS_Als_WalkRun_Forward'` — what are the axes? What sample points? This is critical context for locomotion.

### What to Capture
| Property | Example |
|----------|---------|
| Axis X name + range | Speed (0 → 600) |
| Axis Y name + range | Direction (-180 → 180) |
| Sample count | 9 samples |
| Sample list | (0,0)=Idle, (150,0)=Walk_Fwd, (350,0)=Run_Fwd, ... |
| Interpolation type | Weighted average |
| Target weight interpolation | Per axis (speed 3.0, direction 6.0) |

### Proposed Command
```
DumpBlendSpace /Game/Animation/BS_Als_WalkRun_Forward
```

### Impact
Axes and sample points reveal the blending topology. Speed breakpoints tell you walk→run thresholds. Direction samples show how many directions are blended.

---

## Milestone 6: Chooser Table Dumping (MEDIUM-HIGH PRIORITY)

### Problem
GASP and UE5.4+ Motion Matching projects use Chooser Tables for animation selection. These are `UChooserTable` data assets — NOT AnimBPs or Blueprints. The plugin can't touch them.

### What to Capture
| Property | Why It Matters |
|----------|---------------|
| Columns (parameters) | What inputs drive selection (Speed, Gait, Direction, etc.) |
| Column types | Float range, Enum, Bool, GameplayTag |
| Rows (results) | Which animation/asset each row selects |
| Row conditions | Value ranges/matches that trigger each row |
| Output type | What the chooser returns (AnimSequence, Montage, etc.) |

### Proposed Command
```
DumpChooser /Game/Animation/ChooserTables/CT_Locomotion
DumpAllChoosers /Game/Animation/ChooserTables/
```

### Implementation Notes
`UChooserTable` is in the `Chooser` module (UE5.4+). Need to add module dependency. The table structure is column-based with `FChooserColumnBase` subclasses. Rows are evaluated top-to-bottom with first-match semantics.

### Impact
Critical for understanding ANY Motion Matching project. Without Chooser Table dumps, the animation selection logic is invisible. GASP references choosers heavily (`SetBlendStackAnimFromChooser`).

---

## Milestone 7: Reroute Node Collapse (LOW PRIORITY, EASY)

### Problem
Visual-only routing nodes add noise:
```
\-- Reroute Node
  \-- Reroute Node
    \-- Reroute Node
      \-- Reroute Node
        \-- Layered blend per bone
```

### Proposed Fix
Option to skip Reroute Nodes in pose chain output. Already handled correctly in `BuildExpressionFromPin` (walks through them). Apply same pattern in `WalkPoseChain`.

### Impact
Cleaner output, especially for AB_Als_Layering which has 4-deep reroute chains. Minor improvement.

---

## Milestone 8: Cross-Reference Index (LOW PRIORITY, needs M2+M3)

### Problem
"Which AnimBPs reference animation X?" requires manual grep across dump files.

### Proposed Feature
After batch dump (M2) with JSON output (M3), generate a cross-reference index:

```json
{
  "animations": {
    "A_Als_Sprint": {
      "usedIn": ["AB_Als_Standing:MovementStates:MoveForward", "AB_Als_Monolithic:Standing:Move"],
      "asType": ["SequencePlayer", "SequenceEvaluator"]
    }
  },
  "cachedPoses": {
    "Movement": {
      "savedIn": "AB_Als_Standing:AnimGraph",
      "usedIn": ["AB_Als_Standing:StandingStates:Move", "AB_Als_Standing:StopStates:Entry", ...]
    }
  },
  "blendSpaces": {...},
  "montageSlots": {...}
}
```

### Impact
Answers questions like: "What would break if I delete A_Als_Sprint?" and "Where is cached pose 'Movement' consumed?"

---

## Milestone 12: UE5 Version Compatibility (MEDIUM PRIORITY)

### Current State
Plugin is built for UE 5.7.3 (the engine version it was built against). However, it **already works on UE 5.5.1** — tested successfully with ALS Refactored. UE shows a "plugin was built for a different engine version, expect problems" warning, but no actual problems observed.

### Why It Works Across Versions
The plugin uses editor-only APIs that are stable across UE5 versions:
- `UEdGraph`, `UEdGraphPin`, `UEdGraphNode` — core graph infrastructure, stable since UE4
- `UAnimGraphNode_Root`, `UAnimGraphNode_StateMachine`, `UAnimStateNode` — AnimGraph editor nodes, stable across UE5
- `UK2Node_CallFunction`, `UK2Node_VariableGet` — Kismet/Blueprint graph nodes, very stable
- `FPoseLinkBase::StaticStruct()` — engine-level struct, unlikely to change

### Known Risk Areas
These APIs are more version-sensitive and could break on older UE5 versions:

| API | Risk | Affected Feature | First Available |
|-----|------|-----------------|----------------|
| `UAnimStateAliasNode` | Medium | State alias detection (Gap 1.3b) | UE5.0+ (but class may differ) |
| `GetDisplayNameTextByValue` | Low | BP enum display names | UE4.26+ |
| `PropertyAccessNode` APIs | High | Property Access paths (Gap 1.5) | UE5.1+ |
| `UChooserTable` | High | Chooser Table dumping (M6) | UE5.4+ |
| `BlendStack` node types | High | Blend Stack settings/bindings/leaf (Gap 1.5b) | UE5.4+ |
| Thread Safe anim functions | Low | Function graph detection | UE5.0+ |

### Proposed Approach

**Phase 1: Document + Defensive Guards (Low effort)**
- Add `#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= X` guards around version-sensitive code
- Graceful fallback when a feature isn't available (skip state aliases on UE5.3, skip Chooser dumping on UE5.3, etc.)
- Update `.uplugin` to declare supported engine versions explicitly

**Phase 2: Test Matrix (Medium effort)**
- Test on UE 5.4, 5.5, 5.6, 5.7 with representative projects
- Document which features work on which version
- Identify actual breakages (not just warnings) and add version guards

**Phase 3: Multi-Version Release (Low effort if Phase 1-2 done)**
- Separate plugin releases per engine version, or single release with version guards
- README documents compatibility matrix
- Can claim "UE 5.4 - 5.7+ compatible" on Marketplace/GitHub

### Version Compatibility Matrix (Target)

| Feature | 5.3 | 5.4 | 5.5 | 5.6 | 5.7 |
|---------|-----|-----|-----|-----|-----|
| AnimBP pose chains | Y | Y | Y | Y | Y |
| State machines + transitions | Y | Y | Y | Y | Y |
| Blueprint exec chains | Y | Y | Y | Y | Y |
| Cached pose orphan detection | Y | Y | Y | Y | Y |
| State aliases | ? | Y | Y | Y | Y |
| Chooser Tables (M6) | - | Y | Y | Y | Y |
| Blend Stack internals (Gap 1.5b) | - | ? | ? | Y | Y |
| Property Access paths (Gap 1.5) | ? | Y | Y | Y | Y |

`Y` = works, `?` = needs testing, `-` = feature doesn't exist in that version.

### Impact
Makes the plugin a **drop-in tool for any UE5 project**, not just 5.7. Broadens audience significantly if released publicly. The core graph-walking code is version-agnostic — only specific node type handling needs guards.

---

## Milestone 13: Content Browser Right-Click Menu + Reference Chain (MEDIUM PRIORITY)

### Problem
Console commands require knowing the asset path. Most users work visually in the Content Browser — they see an AnimBP, want to dump it, and shouldn't need to copy-paste a path.

### Feature: Right-Click "Dump Asset"

Right-click any supported asset in Content Browser → **"Dump Asset (BlueprintDumpTool)"** menu entry.

```
[Content Browser: right-click on ABP_Character]
  ├── Edit...
  ├── Rename
  ├── Duplicate
  ├── ...
  ├── ─────────────────────
  ├── Dump Asset (BlueprintDumpTool)     ← NEW
  └── ...
```

Clicking it runs the appropriate dumper (AnimBP or Blueprint) and opens the output file or shows a notification with the path.

### Implementation — UE5 UToolMenus API

UE5's modern menu system uses `UToolMenus`. Register a Content Browser context menu extension:

```cpp
// In Module StartupModule():
UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
{
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu");
    FToolMenuSection& Section = Menu->FindOrAddSection("AssetContextMenu.BlueprintDumpTool");
    Section.AddDynamicEntry("BlueprintDumpTool", FNewToolMenuSectionDelegate::CreateLambda(
        [](FToolMenuSection& InSection)
        {
            // Get selected assets from context
            UContentBrowserAssetContextMenuContext* Context =
                InSection.FindContext<UContentBrowserAssetContextMenuContext>();
            if (!Context) return;

            // Check if any selected asset is a supported type
            bool bHasSupported = false;
            for (const FAssetData& Asset : Context->SelectedAssets)
            {
                if (Asset.AssetClassPath == UAnimBlueprint::StaticClass()->GetClassPathName() ||
                    Asset.AssetClassPath == UBlueprint::StaticClass()->GetClassPathName())
                {
                    bHasSupported = true;
                    break;
                }
            }

            if (bHasSupported)
            {
                InSection.AddMenuEntry(
                    "DumpAsset",
                    LOCTEXT("DumpAsset", "Dump Asset (BlueprintDumpTool)"),
                    LOCTEXT("DumpAssetTooltip", "Dump this asset's structure to a text file"),
                    FSlateIcon(),
                    FToolMenuExecuteAction::CreateLambda([Context](const FToolMenuContext&)
                    {
                        for (const FAssetData& Asset : Context->SelectedAssets)
                        {
                            // Call existing dump logic with asset path
                            FString Path = Asset.GetObjectPathString();
                            // ... dispatch to FAnimBPDumper or FBlueprintDumper
                        }
                    })
                );
            }
        }
    ));
}));
```

The actual API may vary slightly — `UContentBrowserAssetContextMenuContext` is the UE5.1+ approach. Older: `FContentBrowserModule::AddAssetViewContextMenuExtender`. Both are well-documented.

### Feature: Reference Chain Suggestion (Phase 2)

After dumping an asset, query its references and offer to dump connected assets too.

**UE5 Asset Registry API:**
```cpp
FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

TArray<FAssetIdentifier> Dependencies;
AssetRegistry.GetDependencies(AssetData.PackageName, Dependencies);
```

This returns ALL assets the dumped asset references — AnimSequences it plays, BlendSpaces it samples, other AnimBPs it links to, Skeletons, etc.

**Workflow:**

```
[User right-clicks ABP_Character → "Dump Asset (BlueprintDumpTool)"]

Dumping ABP_Character... Done! Saved to Saved/AnimBPDumps/ABP_Character_Dump.txt

This asset references 23 other assets:
  SUPPORTED (can dump now):
    AnimBlueprint (2):  ABP_Character_Overlay, ABP_Camera
    Blueprint (1):      BP_PlayerCharacter

  NOT YET SUPPORTED (listed for reference):
    AnimSequence (12):  A_Idle, A_Run_Fwd, A_Run_Bwd, ...
    BlendSpace (4):     BS_WalkRun_Fwd, BS_WalkRun_Bwd, ...
    AnimMontage (3):    AM_Attack_Light, AM_Dodge, AM_TurnInPlace_90
    Skeleton (1):       SK_Character

  [Dump 3 supported references?] [Yes] [No] [Select individually...]
```

If the user clicks **Yes**, it dumps ABP_Character_Overlay, ABP_Camera, and BP_PlayerCharacter too. If they click **Select individually**, a checklist dialog appears.

As we add new dumpers (M4-M6), more referenced assets move to the "SUPPORTED" section automatically — same pattern as the batch scan manifest (M2).

**The reference chain is recursive** — after dumping ABP_Character_Overlay, it might reference AB_Character_Overlay_Rifle, which references more AnimSequences. Each hop offers the same prompt. In practice, 2-3 hops covers an entire character's animation system.

### Implementation Phases

**Phase 1: Right-click menu (Low effort)**
- Register `UToolMenus` extension in `StartupModule`
- Filter for supported asset classes (AnimBlueprint, Blueprint)
- Call existing dump logic
- Show editor notification with output path

**Phase 2: Reference chain dialog (Medium effort)**
- Query `IAssetRegistry::GetDependencies` after dump
- Filter + group by class
- Show dialog with supported/unsupported split
- Batch dump selected references
- Optional: recursive chain with depth limit (default 2 hops)

**Phase 3: Multi-select support (Low effort)**
- Select multiple assets in Content Browser → right-click → dump all
- Combined manifest showing total asset count + references

### Impact
- **Zero-friction workflow** — see asset, right-click, get dump. No path memorization.
- **Reference chain = guided project exploration** — dump one AnimBP, discover its entire connected graph
- **Natural discovery of unsupported types** — users see "12 AnimSequences referenced but not yet dumpable" and understand what's coming
- **Complements batch dump (M2)** — batch is for "dump everything", right-click is for "dump this one thing and what it touches"

---

## Phase 2 — Hygiene batch (anytime, low risk)

- **LogTemp everywhere** → dedicated `LogBlueprintDump` category, enables verbosity control.
- **Triplicated source-label logic**: `FormatDataPins` (`AnimBPDumper.cpp:739`) ≈
  `GetDataInputSummary` (`BlueprintDumpUtils.cpp:607`), plus a third copy in
  `WalkExecChain`'s VariableSet special case (`BlueprintDumpUtils.cpp:721-754`).
  Consolidate BEFORE M1 — all three need the same M1 upgrade; do it once.
- **`DumpBP` writes into `Saved/AnimBPDumps/`** (`BlueprintDumper.cpp:40`) — misnomer;
  either rename dir to `BlueprintDumps` split by type or a shared `Dumps/`.
- **`"Kismet"` in Build.cs** appears unused — try removing.
- **Transitive-include reliance**: `USkeletalMeshComponent` (`BlueprintDumpUtils.cpp:1140`),
  `AActor`/`GetComponents` — add explicit includes BEFORE the M12 version-compat work
  (include sets shift between engine versions).
- **Console args split on whitespace**: asset path with a space only reaches `Args[0]`.
  Join remaining args or document quoting. Rare for `/Game/` paths, cheap to fix.
- Nit: `DumpVariables` labels `CPF_Edit` as "EditAnywhere" — with
  `CPF_DisableEditOnInstance` it's actually EditDefaultsOnly-ish; refine if variable
  dumps get load-bearing.

---

## Priority Ordering

| Priority | Milestone | Effort | Impact | Validation |
|----------|-----------|--------|--------|------------|
| **1** | **M1: Data pin wiring** | **Medium-High** | **HIGHEST — makes dumps recreatable** | **5/6 GASP functions currently fail without this** |
| 2 | M2: Batch dump + scan manifest | Low-Medium | High — enables everything else | Self-documenting roadmap |
| 3 | M13: Right-click menu + ref chain | Low-Medium | High — zero-friction UX | Phase 1 easy, Phase 2 medium |
| 4 | M3: JSON output | Medium | High — foundation for tooling | Enables M8 |
| 5 | M4: Animation metadata | Low-Medium | Medium-High — essential context | Duration/curves/root motion |
| 6 | M6: Chooser Tables | Medium | High for UE5.4+ projects | GASP uses heavily |
| 7 | M12: UE5 version compat | Low-Medium | High — broadens audience | Already works on 5.5.1, needs guards |
| 8 | M5: Blend Space details | Low | Medium — important for locomotion | Axis config critical |
| 9 | M7: Reroute collapse | Very Low | Low — cosmetic cleanup | AB_Als_Layering noise |
| 10 | M8: Cross-reference | Low (if M2+M3 done) | Medium | Needs M2+M3 first |

**M1 is #1 because:** Every other milestone adds more assets or better formatting to the dump. Only M1 fixes the fundamental data quality — making function dumps go from "structural skeleton" to "functional specification." Without M1, a batch dump (M2) of JSON (M3) still produces non-recreatable function descriptions.

**M13 at #3 because:** Right-click dump is the most natural UX for single-asset exploration — users work in Content Browser, not console. Phase 1 (just the menu entry) is very low effort. The reference chain (Phase 2) is the killer feature: dump one AnimBP, discover its entire animation ecosystem. Pairs perfectly with M2's scan manifest — batch for whole-project, right-click for targeted exploration.

**M12 at #7 because:** The plugin already works cross-version — just needs defensive guards and testing. High payoff for low effort, but doesn't block any other milestone. Best done alongside or after M1-M3 when the core features are mature enough to release.

---

## Suggested Session Order (near-term Phase 0 / M1 work)

1. **0.1** (false cycle) + regression check: grep old GASP dumps for `(cycle)`, re-dump, diff.
2. **0.2 + 0.3** (walker safety) — small, same functions.
3. **0.4** (disabled nodes) + **0.6** (revisit marker) — output fidelity, immediately
   visible in re-dumps.
4. **0.5 + 0.7** — robustness one-liners.
5. Re-dump a representative ABP + one GASP ABP, eyeball diff against a known-good baseline
   (doubles as an overdue re-dump — code has moved since the last baseline).
6. Then start **M1** with the review's fidelity gaps (1.1b–1.5b) added to its gap list —
   consolidate the three source-label summarizers first (Phase 2) since all three need the
   same M1 upgrade.

## Acceptance Checklist (Phase 0 / M1)

- [ ] Test BP: one `Get` feeding both operands of an AND → both print, no `(cycle)`
- [ ] Exec knot loop asset → dump completes with cycle marker, no crash
- [ ] Disabled Branch in a test BP → dumped as `[DISABLED]` (or absent, per chosen policy)
- [ ] Monolithic ABP → main AnimGraph identified by name, layers under Layer Functions
- [ ] Blend node with both inputs from one source → second input shows `(-> see above)`
- [ ] ABP with an On Become Relevant binding → binding visible in dump
- [ ] Blend Stack node → settings (`MaxActiveBlends`, `bUseInertialBlend`, blend profile) + any bound pins visible
- [ ] Re-dump a representative ABP, diff reviewed vs a known-good baseline

---

## Completed Improvements (Changelog)

### Commit 1acb266 — Fix 4 Data Gaps
- Recursive expression resolution in `GetDataInputSummary` (depth 4 for data pins)
- Moved `BuildExpressionFromPin` + helpers to shared `BlueprintDumpUtils`
- BP enum display names (`GetDisplayNameTextByValue` fallback)
- State alias detection in state machines
- Plugin asset path normalization (`/All/`, `/Plugins/` prefix stripping)

### Commit d2e8aaf — NewEnumerator + Monolithic Layers
- Fix NewEnumerator display names for BP-defined enums
- Fix Set-node enum value resolution
- Monolithic AnimBP self-linked layer function graph dumping

### Commit 19e210f — Cached Poses + Cycles + Property Access
- Property Access node handling (FullTitle → pin name fallback)
- Orphaned cached pose chain dumping (`DumpUnvisitedCachedPoses`)
- Per-branch visited sets in exec chain walker (eliminates false cycles)

---

## Validation Methodology

### GASP Side-by-Side Comparison

Compared 6 GASP functions as they appear in:
- **Plugin dump**: `GASP_Dumps/SandboxCharacter_CMC_ABP_Dump.txt`
- **Manual analysis**: a hand-written GASP editor analysis (6,047 lines, from editor screenshots)

**What the dump captures well (structural skeleton):**
- Control flow topology — Sequence order, Branch/True/False, Switch cases, nested branches
- Variable names on SET operations — target always named, simple sources preserved
- Asset references — Chooser names, blendspace names, enum type names
- Function call names — which functions are called and with what named parameters
- State machine transitions — every transition with condition, blend type/time, priority

**What the dump loses (data flow):**
- Property Access paths → shows "Property Access" or "Value", not actual struct member chain
- Pure function arguments → `Lerp()`, `SafeDivide()`, `InRange_FloatFloat()` with no inputs visible
- String literal parameters → curve names, CVar names, tag names inside function calls
- Return values → `Return` with no value on pure-purpose functions like `Get_MMBlendTime`
- Boolean operands → `BooleanAND()` / `BooleanOR()` without showing what's being combined
- Developer comments → zero captured (manual analysis has dozens)
- Design rationale and cross-references → each function presented in isolation

**Recreatability score: 1/6 functions fully recreatable from dump alone.**
Target after M1 completion: 5/6 functions recreatable (Property Access paths may remain partial).

### ALS Architecture Validation

Cross-referenced ALS dump files against ALS-Refactored C++ source (146 files, 18,137 lines):
- Confirmed empty function sections are correct — ALL logic in `AlsAnimationInstance.cpp` (2016 lines)
- C++ computes 40+ state struct members (`FAlsLocomotionAnimationState`, `FAlsGroundedState`, `FAlsStandingState`, `FAlsTurnInPlaceState`, `FAlsFeetState`, `FAlsLayeringState`, etc.)
- AnimBP reads these as `BlueprintReadOnly` properties via Property Access / variable nodes
- Thread Safe functions (`RefreshGroundedMovement`, `RefreshTurnInPlace`, etc.) are `BlueprintCallable` C++ — called FROM AnimBP nodes in the pose chain but logic executes in C++
- 15 custom Control Rig units + 2 custom anim nodes (CurvesBlend, GameplayTagsBlend) — runtime-only, not dumpable
- Plugin correctly captures 100% of the BP-side structure. The "missing" data is C++.
