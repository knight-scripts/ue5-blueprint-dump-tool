# Blueprint Dump Tool — Improvement Plan

> Focused fix list for the plugin — **not a product roadmap.** Consolidates the former
> `IMPROVEMENT_MILESTONES.md` and the 2026-07-02 code-review fix plan into one document.
> This is a personal tool for reading AnimBP/Blueprint dumps as ground truth; the goal is
> trustworthy dumps, not features. Output stays **plain text** (the JSON idea was dropped).
>
> **Structure & order of work:** Phase 0 (correctness — do first) → Milestone 1 (data-pin
> wiring / fidelity, incl. the Blend Stack settings + bindings decode) → Phase 2 (hygiene).
> Phase 0 must land before M1: M1's recursive expression resolution multiplies the walker
> workload, and the Phase 0 fixes determine whether that work produces truth or noise.
> Speculative "grow it into a product" ideas are parked as one-liners at the end.
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
Built for UE 5.7.3 but **works on 5.5.1** (tested with ALS Refactored). UE shows a version warning, no actual problems. (Broader multi-version support is parked — see Parked ideas.)

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

## Parked ideas (only if a concrete need appears)

Speculative "grow it into a product" milestones, cut to keep the plan focused on making dumps
trustworthy (Phase 0 + M1). One line each so the idea survives — git history has the full
write-ups if any is ever revived. (JSON output was dropped outright: plain text stays the format.)

- **Batch dump + scan manifest** — dump a whole folder; list supported vs unsupported assets.
- **Animation metadata inline** — clip duration, curves, root motion, notifies next to Sequence Players. *(Closest to actually useful — fits the curve-driven work.)*
- **Blend Space details** — axes, sample points. *(Project uses discrete directional clips, not blendspaces.)*
- **Chooser Table dumping** — Motion-Matching selection logic. *(GASP reference only; project doesn't use choosers.)*
- **Reroute node collapse** — skip visual-only routing nodes in pose-chain output.
- **Cross-reference index** — "which AnimBPs use animation X". *(Depended on JSON, which was dropped.)*
- **UE5 multi-version compatibility** — `#if` guards + test matrix for 5.4–5.8. *(Marketplace concern.)*
- **Content Browser right-click "Dump Asset"** — UX nicety; console commands work fine.

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
  `AActor`/`GetComponents` — add explicit includes (include sets shift between engine
  versions; matters if cross-version support is ever revived).
- **Console args split on whitespace**: asset path with a space only reaches `Args[0]`.
  Join remaining args or document quoting. Rare for `/Game/` paths, cheap to fix.
- Nit: `DumpVariables` labels `CPF_Edit` as "EditAnywhere" — with
  `CPF_DisableEditOnInstance` it's actually EditDefaultsOnly-ish; refine if variable
  dumps get load-bearing.

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
