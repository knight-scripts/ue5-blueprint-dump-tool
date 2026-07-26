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
> **Copies:** the canonical home is this standalone repo. The host project embeds it as a
> **git subtree** at `Plugins/BlueprintDumpTool` (converted 2026-07-06 — no more manual
> mirroring). Develop and commit HERE, then sync into the host with
> `git subtree pull --prefix=Plugins/BlueprintDumpTool <path-to-this-repo> main --squash`.
> Never edit the embedded copy directly.

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

> **✅ ALL of Phase 0 (0.1–0.9) LANDED 2026-07-06** — plus the Blend Stack cluster
> (1.5b A/B/C, 1.1b, 1.2b) from M1, which is independent of the expression walker.
> See changelog. **Verification pending**: in-editor re-dump on machine 2 against the
> acceptance checklist. During implementation a 4th unchecked `LinkedTo[0]` site was
> found and fixed (0.7): `WalkExecChain`'s VariableSet special case, plus its
> null-`SourceNode` title deref.

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

Takes the FIRST `UAnimationGraph` from `GetAllGraphs()` with **no filter at all** (verified
in-code 2026-07-06). In monolithic ABPs, self-linked layer graphs are ALSO `UAnimationGraph`
— and because `GetAllGraphs()` includes sub-graphs, the first match can be a layer graph *or
even a state's nested bound sub-graph*, not just "a layer". Has held on ALS/GASP so far by luck.

**Fix:** name-based **and** top-level-scoped selection (`Graph->GetFName() == TEXT("AnimGraph")`
AND `Outer == AnimBP || GeneratedClass`), keep current first-match as fallback.

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

### 0.8 Missing parentheses corrupt nested boolean/arithmetic expressions ⚠ pairs with 0.1 (same function)
**File:** `BlueprintDumpUtils.cpp:502, 546` (operator formatting; also `:506`)

`BuildExpressionFromPin` emits `A op B` with **no grouping**, so a rule authored as
`bA && (bB || bC)` — structurally `BooleanAND(bA, BooleanOR(bB,bC))` — dumps as
`bA && bB || bC`, which reads by precedence as `(bA && bB) || bC`: **a different boolean
condition than the AnimBP evaluates.** Same for arithmetic (`(a+b)*c` → `a+b*c`). Verified
in-code 2026-07-06.

This hits **transition-rule reconstruction** — the tool's primary output and the exact thing
ABP-contract checks read (H5/H6 "does state X have edge Y with condition Z"). It is *worse*
than 0.1's `(cycle)`: 0.1 prints a visible marker, this produces clean-looking output that is
**silently wrong**. Fires on any rule mixing `&&`/`||` (GASP and any non-trivial SM do this
constantly). M1's deeper recursion amplifies it.

**Fix:** parenthesize nested operator sub-expressions — wrap an operator result in `(...)`
when it is an operand of another operator; strip the outermost parens at the top-level call
site. Do this together with 0.1 (both live in `BuildExpressionFromPin`).

### 0.9 `(automatic)` conflates a genuinely-automatic transition with a parse failure
**File:** `AnimBPDumper.cpp:460` (+ `BuildTransitionRuleExpression` `:595`)

When `BuildTransitionRuleExpression` returns empty because it **couldn't reconstruct** the
rule, the caller labels it `(automatic)` — identical to a transition that genuinely has no
rule. Silence-as-bug-signal: a parse failure is indistinguishable from a real auto-transition.

**Fix:** distinguish the cases — `(unresolved)` when reconstruction failed vs `(automatic)`
only when `bAutomaticRuleBasedOnSequencePlayerInState` / the logic type actually says so.

---

## Milestone 1: Data Pin Source Wiring (HIGHEST PRIORITY — #1)

> **✅ CORE LANDED 2026-07-26 — Gaps 1.1 / 1.2 / 1.4 / 1.6 were ONE defect, and T5's
> open question was answered by reading the code, not by a re-dump.**
>
> The collapse was a `UK2Node_CallFunction` early-out that emitted `MemberName + "()"`
> and never consulted the node's pins — hand-copied into **three** places
> (`GetDataInputSummary`, `WalkExecChain`'s VariableSet case, `FormatDataPins`), plus
> `BuildExpressionFromPin`'s own `return FuncNameStr;`. That is why `BooleanAND()`
> collapsed while `Select(2000.0, 500.0, HasMovementInputVector)` expanded in the SAME
> dump: `UK2Node_Select` is not a `CallFunction`, so it fell through to the generic
> recursion. Not a missing feature — a short-circuit on one node class.
>
> Fix: all four sites now route through one `ResolvePinSourceLabel`, and
> `BuildExpressionFromPin` recurses into function arguments. String/name/text literals
> are quoted (Gap 1.2 needed no filter change — the filter was simply never reached).
> Data-pin depth 4 → 6 (`DefaultExpressionDepth`), since the formulas M1 exists to
> recover run 4-5 levels deep.
>
> **⚠ Gap 1.3 (Return values) was ALREADY CLOSED before this round** — the sprint
> decode's own `Return (Return Value=BooleanAND())` proves the pin was read; only the
> expression collapsed. The "easiest fix, huge payoff" ranking below is stale.
>
> Still open in M1: **Gap 1.5 (Property Access deep paths)** — the FullTitle
> second-line fallback still carries it; the reflection route (`TextPath`/`Path`) is
> unimplemented.

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

## Milestone 2 — TCF decode enablement (ADDED 2026-07-26)

> **✅ T1 / T2 / T3 LANDED 2026-07-26 (same day as M1's core).** T4 (montage notify
> tracks) is the one remaining item. See the changelog for what each became, and the
> M2 acceptance list below for what machine 2 still has to confirm.

> **Why this exists now.** `TEMPEST_TCF_DECODE_2026_07_25.md` (workspace) decoded the Tempest
> Combat Framework's C++ and found it is a **skeleton**: 50+ empty `_Implementation` bodies,
> with all actual combat policy living in **319 Blueprint uassets** (54 Abilities · 37 States ·
> 41 Notifies · 22 BehaviorProperties · 22 Input · 12 AttackProperties · 7 Buffers · 7 Camera ·
> 6 Feel · 5 RotationProperties · 3 DataAssets · BT/BB · 23 Structs&Enums). That decode's §9
> names a machine-2 dump pass as the follow-up that would convert the policy layer to text —
> the same move the GASP dumps made for locomotion. **This milestone is what the tool needs
> before that pass is worth running.** Same applies to any future purchased-plugin decode
> (Narrative 3's quest/dialogue graphs, NinjaCombat content) — TCF is just the forcing case.
>
> Ordering: **M1 still comes first** (a batch dump of unreadable expressions is 319 unreadable
> files). T1/T2 are what make TCF-shaped assets dumpable at all; T3 makes the pass practical.

### T1 — Instanced sub-object properties are not recursed into ⚠ **CRITICAL / the blocker**

TCF's entire authoring model is `UPROPERTY(Instanced, EditAnywhere)` UObjects and arrays of
them: `FInstancedAttackProperties`, `ImpactProperties[]`, `AttackPropertyTraits[]`,
`FInstancedFeelProperties`, `CameraProperties[]`, `RotationPropertiesToPerform[]`,
`BufferInfo[]` (input action → buffer property), `FInstanced_AI_BehaviorProperty`.

⚠ **Diagnosis corrected on implementation (2026-07-26) — it was WORSE than written here.**
The original text said instanced properties printed "an object reference/path, not the
object's contents". They printed **nothing at all**: `SkipDiffPropertyFlags` hard-filtered
`CPF_InstancedReference | CPF_PersistentInstance | CPF_ContainsInstancedReference`, a noise
filter added by the 2026-07-19 Class-Defaults work for the correct local reason (instanced
POINTERS always differ between a BP CDO and its parent CDO). So an AttackProperty's traits
were not merely opaque — they were absent, with no marker. Silence, again.

Confirmed as written: `AppendSubobjectDiffs` **early-outs when
`Archetype->GetClass() != Sub->GetClass()`** ("field diff undefined") — precisely the
details-panel case where the author picks a *subclass*.

⇒ Dumping `AttackProperty_LightAttack` today would emit its tag and little else; the traits,
impacts and tuned numbers — the whole reason to dump it — stay invisible. **Fix:** walk
`FObjectProperty` (and `FArrayProperty` of object) values whose property has `CPF_InstancedReference`
/ whose class is `EditInlineNew`; recurse with the sub-object's own **class CDO** as the archetype
when the class differs (that is the correct comparison, and it removes the early-out); indent
and depth-cap (suggest 4) with a cycle guard.

### T2 — Non-Blueprint asset types are silently unsupported ⚠ **CRITICAL**

`FBlueprintDumper::DumpBlueprint` does `LoadObject<UBlueprint>(...)` and returns empty on
failure (`BlueprintDumper.cpp`), so every non-BP asset is a no-op — and the failure is quiet.
TCF needs, in rough priority order:

1. **`UDataAsset` / `UPrimaryDataAsset`** — property dump w/ T1 recursion (TCF DataAssets;
   also NinjaGAS's `UNinjaGASDataAsset` loadout pattern, Narrative's Default* libraries).
2. **`UAnimMontage`** — see T4.
3. **`UBehaviorTree` + `UBlackboardData`** — node tree w/ decorators/services/task params
   and BB key list (TCF's `TCF_BT`/`TCF_BB`, and the P-1 AI question generally).
4. **`UUserDefinedStruct` / `UUserDefinedEnum`** — field/enumerator lists (23 assets; needed
   to read everything else).
5. **`UCurveFloat` / `UCurveVector`** — key list. *(Would also have made today's
   `Curve_StrafeSpeedMap` read exact instead of pixel-measured off a screenshot — see the
   sprint decode §2d "±3°" caveat.)*
6. **`UDataTable`** — row-struct + rows.

Minimum viable: dispatch on loaded `UObject` class, fall back to a generic
"class + property diff vs CDO (with T1 recursion)" dumper, and **name the asset type in the
header** so an unsupported type is loud, not silent (doctrine: silence is a bug signal).

### T3 — Batch / recursive folder dump + manifest — **HIGH** (promoted from Parked)

319 assets cannot be dumped by hand. Wanted: `DumpBPFolder /Game/Path [-recursive]` that walks
the asset registry, dispatches per type (T2), writes one file per asset into a mirrored
directory tree, and emits a **manifest** listing `asset → type → dumped | skipped(reason)`.
The manifest is the acceptance artifact: it makes coverage auditable instead of assumed.
Cheap add-on: `-filter=Class` and a summary line (`N dumped, M skipped`).

### T4 — AnimMontage notify tracks — **HIGH** (promoted from Parked "Animation metadata inline")

TCF's combat *timing* lives on montages, not in classes: `ANS_AttackTrace` (carries an
instanced attack property), `ANS_SetAllowedInputs` (carries `AllowedInputs[]` — the input
buffer window), `ANS_TempestStandardRotation` / `ANS_RotateToInputDirectionCPP`, plus
NinjaGAS's `AnimNotifyState_ApplyGameplayEffect` / `_ApplyLooseGameplayTags`.

Wanted per montage: sections + slots, and per notify **class · track · start time · duration ·
its own properties (T1 recursion)**. Without this, "how long is the deflect window" — the
question the decode exists to answer — is unanswerable. Reuse for our own project is immediate
(H10 commitment windows, jump-program window forensics).

### T5 — M1 is reinforced, not replaced (evidence from 2026-07-25) — ✅ ANSWERED 2026-07-26

The sprint decode hit **Gap 1.1 (pure function arguments)** and **Gap 1.4 (BooleanAND/OR
operands)** head-on: `SandboxCharacter_CMC_Dump.txt` renders `CanSprint() -> Return (Return
Value=BooleanAND())` and `CalculateMaxSpeed -> Return (Return Value=SelectFloat())`, so the
sprint gate's whole condition had to be recovered from screenshots instead. Note `BooleanAND`
**is** in `ResolveOperatorSymbol`'s map, and sibling functions in the same dump *do* expand
(`Select(2000.000000, 500.000000, HasMovementInputVector)`) — so this is a **path-specific
collapse, not a missing feature**, and the published dump may also predate current code on
machine 2. ⇒ Before coding M1: **re-dump one known asset and diff**, to establish whether the
gap is in `GetDataInputSummary`'s Return handling, in the `UK2Node_CallFunction` early-out
(`return FuncNameStr;`, `BlueprintDumpUtils.cpp:520`), or already fixed. Known-answer target:
`CanSprint` must render `WantsToSprint && (OrientRotationToMovement ? true : |Δyaw| < 50)`-
equivalent structure.

**✅ Verdict (2026-07-26): the `UK2Node_CallFunction` early-out — in all three copies of it,
plus the walker's own `return FuncNameStr;`.** Return-value handling was already fine. The
re-dump-and-diff was not needed to establish this; reading the two functions settled it, and
the `Select(...)`-expands-while-`BooleanAND()`-collapses asymmetry is explained exactly by
which node classes hit the early-out. The re-dump is still owed — as **verification** of the
fix (the `CanSprint` known-answer above), not as diagnosis.

For TCF this is existential, not cosmetic: **100 % of TCF's logic is Blueprint**, so an
expression-collapsing dump of 319 assets produces 319 files of node names.

### M2 acceptance (known-answer, per the project's decoder-test doctrine)

> **✅ VERIFIED 2026-07-26 on machine 2** (`SummoningDumps.7z` + `GASPAnimBPDumps.7z`).
> Compiled clean; both new module deps fine. Results below.

- [x] T1: instanced traits and their tuned values appear, nested and indented — **PASSED on
  `IMC_Default`** (struct → array-of-struct → array-of-instanced-object, 3 levels):
  `Mappings[4] → Modifiers[0] = InputModifierNegate → bX = False (default: True)`.
  TCF's `FInstancedAttackProperties` / `ImpactProperties[]` is the same shape.
- [x] T1: an instanced property whose value is a *subclass* still dumps — **PASSED**; the
  declared type is `UInputModifier`, the values are `InputModifierSwizzleAxis` /
  `InputModifierNegate` subclasses, and they dump with their class named.
- [x] T2: real content per type, unsupported types loud — **PASSED across ~20 classes**
  (Blueprint, AnimBlueprint, InputAction, InputMappingContext, Texture2D, Material,
  MaterialInstanceConstant, SkeletalMesh, Skeleton, PhysicsAsset, IKRigDefinition,
  MirrorDataTable, ControlRigBlueprint, SkeletalMeshLODSettings, StaticMesh…). Class named
  in every header. ⚠ BT/Blackboard/Curve/DataTable/UserStruct paths NOT yet exercised —
  no such assets in `/Game/Summoning`; they come with the TCF pass.
- [x] T3: manifest row count == asset count — **PASSED**: 208 found = 35 dumped + 173
  skipped, every skip carrying `(class filter)`. Mirrored tree correct.
- [ ] T4: dump a montage carrying `ANS_AttackTrace` → notify class, start, duration, and the instanced attack property all present *(built 2026-07-26, uncompiled)*
- [x] T5: `CanSprint` renders both operands of the AND — **PASSED**:
  `Break S Player Input State(CharacterInputState) && Select((Abs(NormalizedDeltaRotator(
  K2_GetActorRotation(), Conv_VectorToRotator(Select))) < 50.000000), true,
  bOrientRotationToMovement)`
- [x] Re-dump `BP_BaseCharacter` + `ABP_Hero`, diff vs 2026-07-19 — **no tool regressions.**
  All structural deltas are real machine-2 project changes (ABP: the FootPlacement swap;
  BP: inventory/equipment/interaction components, `RefaceSolverRow.ScaleMax` 1.5→1.3,
  `RunTurnGen.FrontTravelBand` new). The only tool-attributable delta is an improvement —
  see "the `(empty)` fix" below.

#### ⭐ Unplanned win: the `(empty)` fix resolved a standing project question

Switching the exporter to `ExportTextItem_Direct` on value pointers made false/zero/None
values print as themselves instead of the tool's `(empty)` placeholder:

```
- StartRotationOWThreshold = (empty)     (default: 0.700000)
+ StartRotationOWThreshold = 0.000000    (default: 0.700000)
- bUseSeparateBrakingFriction = True     (default: (empty))
+ bUseSeparateBrakingFriction = True     (default: False)
```

The host project's `CLAUDE.md` carried `⚠ StartRotationOWThreshold "(empty)" vs default 0.7
— verify intent` as an open item. It is answered: the BP deliberately overrides it to **0.0**.
Remaining `(empty)` values are legitimate — genuinely empty arrays on the default side.

#### ⚠ Two NEW gaps found by reading the verified output — ✅ BOTH FIXED same day (see changelog)

**N1 — output-pin identity is dropped on multi-output nodes (HIGH; the biggest remaining
expression gap).** `BuildExpressionFromPin` is *given* the specific output pin but the
function-call and generic branches render only the node title, so which output was read is
lost. Real examples from the verified dumps:
- `Break S Player Input State(CharacterInputState)` — which member? (`WantsToSprint` is the
  answer, and it is invisible)
- `Select(WalkSpeeds, RunSpeeds, SprintSpeeds, Gait)` appears **twice** in `CalculateMaxSpeed`
  — once for the range Min and once for the Max, indistinguishable
- `Conv_Vector2DToVector(?, Get IA_Move, Get IA_Move, 0.000000)` — X and Y both print as the
  same label

**Fix:** when the source node has more than one data output pin, append the pin name —
`Break S Player Input State(CharacterInputState).WantsToSprint`, or just `.X` / `.Min`.
Cheap: the pin is already in hand at the top of the function.

**N2 — expression depth truncation is SILENT (MEDIUM).** At `MaxDepth` the walker returns
the bare node title, which is indistinguishable from a genuine zero-argument call. In
`CanSprint`, `Conv_VectorToRotator(Select)` is depth 6 being hit — that `Select` has
arguments, they were just not printed, and nothing says so. This is the same defect class
as 0.9's `(automatic)` vs `(unresolved)` fix, in the walker instead of the transition
labeller. **Fix:** emit a marker, e.g. `Select(...)` or `Select…`, and consider depth 8.

Minor polish observed, not worth its own round: world-context pins render as a leading `?`
(`HandleTransformTrajectoryWorldCollisions(?, Self-Reference, …)`); split-struct pins
fabricate zeros (`GreaterGreater_VectorRotator(…, 0, 0, 0, 0.0, 0.0, …)`).

⚠ **Still unexercised after two verification rounds:** the 0.8 parenthesisation fix. GASP's
50 transition rules are all simple calls (`IsMoving()`, `NOT IsMoving()`,
`MovementMode == InAir`), so no rule mixing `&&`/`||` has ever been dumped. Same note as
2026-07-06 — it needs an authored test rule, or TCF content, to validate.

## Milestone 3 — D-2 done properly: structural container recursion

> **Why this is a real item and not a nit.** The 1000-char cap shipped in the 10th round is
> a mitigation. Flat `ExportTextItem_Direct` output is *lossy by construction* for nested
> containers: the value that gets clipped is always the one with the most structure, i.e.
> the one worth reading. TCF's `Damage Applications Per Impact Result` — the map that holds
> every attack's posture/health numbers per impact result — is exactly this shape. A bigger
> cap postpones the problem; it does not change that a TMap of structs is being rendered as
> one line of engine text.

### The move

The same one T1 made for instanced objects: stop exporting, start walking.
`AppendPropertyDiffs` already recurses instanced objects, arrays of them, and structs that
*contain* them. Extend that walker to containers generally.

### M3-1 — recurse only when it pays (the design decision)

Do **not** recurse every struct: `(X=1.0,Y=2.0,Z=3.0)` is more readable on one line than as
three, and unconditional recursion would bloat every dump we just spent six rounds shrinking.

**Rule: export flat first; recurse only if the flat text exceeds a threshold** (start at
**120 chars**, tune on evidence). Short values keep today's output byte-for-byte — which
also keeps the blast radius small and makes the regression diff readable.

### M3-2 — per-container handling

- **`FStructProperty`** — recurse fields with the baseline's matching field. Already exists
  for the instanced case; drop the `CPF_ContainsInstancedReference` condition and gate on
  length instead.
- **`FArrayProperty`** — per element; baseline element `i`, or a default-constructed
  instance when the baseline array is shorter (the `FStructOnScope` pattern
  `AppendInstancedElement` already uses).
- **`FMapProperty`** — ⚠ **match by KEY, not index.** This is the one that matters: TCF's
  damage map is keyed by GameplayTag, and index-matching would diff `Impact.Result.Block`
  against `Impact.Result.Hit` and report nonsense. `FScriptMapHelper::FindMapIndexWithKey`.
  Print `["<key>"]` then recurse the value.
- **`FSetProperty`** — elements, no baseline pairing (report added/removed).

### M3-2b — nested enums come along for free (evidence, 12th round)

56 of the 122 surviving `NewEnumerator` occurrences are enums *inside* flat-exported
Chooser structs. Routing every leaf through `ExportValue` — which already resolves enums
per D-1 — fixes them as a side effect. Flat export loses enum resolution as well as data,
so M3 closes both.

### M3-3 — reuse the existing guards

`FDiffContext` already carries the depth cap and cycle guard; container recursion counts
against the same budget. No new guard concepts.

### Target output

```
Damage Applications Per Impact Result: 2 entries
  ["Impact.Result.Block"]
    DamageToApply[0]
      TargetAttributeToEffect = Attribute.Posture
      TargetAttributeAmountToApply = 20.000000
  ["Impact.Result.Hit"]
    DamageToApply[0]
      TargetAttributeToEffect = Attribute.Health
      TargetAttributeAmountToApply = 35.000000
```

### ⚠ Blast radius — the reason this is its own milestone

`AppendPropertyDiffs` is now load-bearing for **every** section that took nine rounds to
verify: Class Defaults, T1 instanced content, T2's generic fallback and DataAsset path, BT
node settings, and T4 notify payloads. This is the single riskiest remaining change.

**Acceptance is a regression diff, not a feature check:**
- [ ] Re-dump GASP + TCF Samurai + our own `BP_BaseCharacter` / `ABP_Hero`, and diff against
      the 10th-round dumps. Every changed line must be a value that *was* truncated or
      exceeded the threshold. Anything else changing is a regression.
- [ ] `(truncated)` count drops sharply; the remaining ones are genuinely long leaf strings.
- [ ] TCF's damage map shows **both** `Impact.Result.Block` and `Impact.Result.Hit` with
      their attribute names and numbers, nested.
- [ ] Total dump size does not balloon — short values must still print flat.

### API to verify at 5.7 before writing (per project doctrine)

`FScriptMapHelper` (`Num`, `IsValidIndex`, `GetKeyPtr`, `GetValuePtr`,
`FindMapIndexWithKey`), `FScriptSetHelper` (`Num`, `IsValidIndex`, `GetElementPtr`),
`FMapProperty::KeyProp/ValueProp`, `FSetProperty::ElementProp`.

---

## Parked ideas (only if a concrete need appears)

Speculative "grow it into a product" milestones, cut to keep the plan focused on making dumps
trustworthy (Phase 0 + M1). One line each so the idea survives — git history has the full
write-ups if any is ever revived. (JSON output was dropped outright: plain text stays the format.)

- ~~**Batch dump + scan manifest**~~ — **PROMOTED 2026-07-26 → M2/T3** (TCF needs 319 assets).
- ~~**Animation metadata inline**~~ — notify half **PROMOTED 2026-07-26 → M2/T4**; the rest
  (clip duration, curves, root motion next to Sequence Players) stays parked here.
- **Blend Space details** — axes, sample points. *(Project uses discrete directional clips, not blendspaces.)*
- **Chooser Table dumping** — Motion-Matching selection logic. *(GASP reference only; project doesn't use choosers. Note: their `ColumnsStructs` value now dumps GUID-free as of the 6th round, so the raw text is at least readable.)*
- **Reroute node collapse** — skip visual-only routing nodes in pose-chain output.
- **Cross-reference index** — "which AnimBPs use animation X". *(Depended on JSON, which was dropped.)*
- **UE5 multi-version compatibility** — `#if` guards + test matrix for 5.4–5.8. *(Marketplace concern.)*
- **Content Browser right-click "Dump Asset"** — UX nicety; console commands work fine.

## Phase 2 — Hygiene batch (anytime, low risk)

- **LogTemp everywhere** → dedicated `LogBlueprintDump` category, enables verbosity control.
- ~~**Triplicated source-label logic**~~ — **DONE 2026-07-26** as part of M1's core: all
  three collapsed onto `ResolvePinSourceLabel`. Not hygiene in the end — the duplication
  was where the M1 data-loss bug lived, in triplicate.
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
- **Fabricated `"0"` operand** (`BlueprintDumpUtils.cpp:504-507`): a binary operator with only
  one resolved operand emits `"X op 0"`, inventing a value. Rare; emit `?` instead of `0`.
- **Enum inequality not resolved to `!=`** (`ResolveOperatorFromTitle`): `"Not Equal (Enum)"`
  → first word `"Not"` misses the operator map → shows function-style `Not Equal (Enum)(A, B)`
  instead of `A != B`. Readability only.
- **Dead param**: `DumpStateMachine`'s `Visited` argument is unused (`AnimBPDumper.cpp:336`).

---

## Suggested Session Order (near-term Phase 0 / M1 work)

1. **0.1 + 0.8** (false cycle + missing parens) — **do together, same function
   (`BuildExpressionFromPin`); these two are what make transition-rule output trustworthy.**
   Regression check: grep old GASP dumps for `(cycle)`, author a test rule mixing `&&`/`||`,
   re-dump, diff.
2. **0.2 + 0.3** (walker safety) — small, same functions.
3. **0.4** (disabled nodes) + **0.6** (revisit marker) + **0.9** (`(automatic)` vs
   `(unresolved)`) — output-fidelity fixes, immediately visible in re-dumps.
4. **0.5 + 0.7** — robustness one-liners.
5. Re-dump a representative ABP + one GASP ABP, eyeball diff against a known-good baseline
   (doubles as an overdue re-dump — code has moved since the last baseline).
6. Then start **M1** with the review's fidelity gaps (1.1b–1.5b) added to its gap list —
   consolidate the three source-label summarizers first (Phase 2) since all three need the
   same M1 upgrade. **Start M1 with the T5 re-dump-and-diff** — it may narrow (or close)
   Gaps 1.1/1.3/1.4 before a line is written.
7. ~~**M2 (TCF enablement)**~~ — **T1/T2/T3 LANDED 2026-07-26** alongside M1's core, in
   that order, so the 319-asset pass is not gated on unreadable expressions. **T4 (montage
   notify tracks) is the remaining item** and is what makes "how long is the deflect
   window" answerable.

### Next session (2026-07-26 exit state — M1 + M2 CLOSED, nothing uncompiled)

Everything in M1 core and M2 (T1/T2/T3, N1/N2, and the 6th-round F-1…F-5) is verified on
machine 2 across three unrelated projects totalling 10,688 assets. No pending compile.

Remaining:

1. **Verify T4** (8th round, the only uncompiled code) — known-answers in its changelog
   entry. Re-run `DumpBPFolder /TempestCombatFramework -recursive`; montages now dump.
2. **M3 — structural container recursion** (see its section above). The honest fix for
   D-2. Highest blast radius of anything left, so it gets a regression-diff acceptance
   rather than a feature check.
3. Gap 1.5 (Property Access reflection) → `FText` localisation GUIDs → `Not Equal (Enum)`
   → 1.3b/1.4b → rest of Phase 2. All polish; none blocks a decode.

**With T4 verified, M1 and M2 are complete and the tool is done as a decode instrument.**
Scale is proven (7287-asset project, 3509-char worst line, zero guard pathologies), TCF's
318 assets are in text, and combat timing is readable. The next real work is reading those
dumps, not improving the reader.

**The TCF pass itself is unblocked** — but note TCF is not mounted in the host project
(source-only copy on machine 1). It needs the plugin installed to the engine, or a
throwaway project with TCF + this plugin enabled, before `DumpBPFolder` can reach it.

## Acceptance Checklist (Phase 0 / M1)

- [ ] Test BP: one `Get` feeding both operands of an AND → both print, no `(cycle)`
- [ ] Exec knot loop asset → dump completes with cycle marker, no crash
- [ ] Disabled Branch in a test BP → dumped as `[DISABLED]` (or absent, per chosen policy)
- [ ] Monolithic ABP → main AnimGraph identified by name, layers under Layer Functions
- [ ] Blend node with both inputs from one source → second input shows `(-> see above)`
- [ ] ABP with an On Become Relevant binding → binding visible in dump
- [ ] Blend Stack node → settings (`MaxActiveBlends`, `bUseInertialBlend`, blend profile) + any bound pins visible
- [ ] Re-dump a representative ABP, diff reviewed vs a known-good baseline
- [ ] Class Defaults section: `DumpBP` on a character BP lists a knob deliberately changed
  in Class Defaults (incl. one on the movement component) with both values; an untouched
  knob is absent; the section header + `(not listed = inherited default)` footer always print
- [x] **M1 known-answer** — **PASSED, exactly.** `Get_DynamicPlayRate` now dumps
  `Lerp(1.000000, FClamp(SafeDivide(Speed2D, FClamp(SpeedCurve, 1.000000, 999.000000)),
  MinDynamicPlayRate, MaxDynamicPlayRate), AlphaCurve)` — character-for-character the
  formula this document recorded as the unreachable "REAL" answer under Gap 1.1.
- [x] **String literals** — **PASSED.** The four `GetCurveValueFromAnimation` calls that
  Gap 1.2 cited as "appears 4 times identically" now read
  `GetCurveValueFromAnimation(AnimSequence, "Enable_Warping", AnimTime)`,
  `"MoveData_Speed"`, `"MaxDynamicPlayRate"`, `"MinDynamicPlayRate"`. Also recovers
  literal arrays: `Select("StrafeOffset_F", "StrafeOffset_B", "StrafeOffset_LL", …,
  MovementDirection)`.
- [x] **No expression blow-up** — **PASSED.** Largest dump 872 lines; longest expressions
  are dense but readable; the cycle / depth-limit / already-shown guards fired **zero**
  times across all 71 files. Depth 6 stays.

**Recreatability score: the 2026-07-02 GASP baseline was 1 of 6 functions.**
`Get_DynamicPlayRate` and `Get_MMBlendTime` are now fully recreatable, `CanSprint` /
`CalculateMaxSpeed` likewise; the residue on the rest is N1 (output-pin identity), not
missing data flow.

---

## Completed Improvements (Changelog)

### 2026-07-26 (12th round) — ✅ 10th + 11th VERIFIED. Tool complete except M3.

Verified on TCF Samurai (364) + GASP (3083) + ALS-Refactored (341) + Summoning (208).

**D-3 exact.** `Assets found: 570   dumped: 341   skipped: 229`, all 229 rows carrying
`skipped (external actor package; -external to include)`, manifest rows still summing to
**570**, **0** external dumps on disk, and the policy stated in the manifest header.

**Enum resolution works.** `Swing Direction = Downward  (default: Right)` — was
`NewEnumerator13 (default: NewEnumerator3)`.

⭐ **The damage map is complete and untruncated** — the thing the 1000-char cap was raised
for:
```
Impact.Result.Block -> Attribute.Posture 10.0
Impact.Result.Hit   -> Attribute.Health  10.0 + Attribute.Posture 5.0
```
That is a full attack damage spec in text, both results, per attack.

**Full health check across 3976 dumps / 4 projects — every defect class at zero:**
UTF-16 0 · `(cycle)` 0 · `(unresolved)` 0 · `(depth limit)` 0 · node-budget 0 · `((none))` 0
· GUID group 0 · F-2b macro re-expansion 0 · F-5 bare `TArray` 0 · F-6 section leak 0 ·
max line **2072 chars**.

#### The residue is 100% accounted for, and it is all M3

122 `NewEnumerator` remain. Every one is explained:
- **66 are deliberate** — the UserDefinedEnum dumper's `2 = Aim  (NewEnumerator2)` lines.
  That is the **decoder key**: it maps the internal names that appear inside other dumps
  back to authored labels. Keep.
- **56 sit inside flat Chooser export text** — `ValueName="E_MovementMode::NewEnumerator4"`
  in `CHT_*` `ColumnsStructs`. The per-property enum resolver cannot reach an enum nested
  inside a struct that is exported as one string.

Truncations: 2979 -> **1405** at the 1000-char cap.

⇒ **Both residues are the same defect, and it is D-2.** Flat container export does not just
clip data, it also loses enum resolution inside whatever it flattens. That is now
*evidence* for M3 rather than an argument for it, and M3's spec should note that structural
recursion fixes the nested-enum case for free by routing every leaf through `ExportValue`.

**Status: M1, M2 and every filed defect (F-1…F-6, N1, N2, D-1, D-3) are closed and verified
across five unrelated projects totalling ~11,900 assets. M3 is the only open item.**

### 2026-07-26 (11th round) — ALS-Refactored as a 5th test project; D-3 external actors

ALS-Refactored (570 assets) run purely as a bug hunt. **Every fixed defect stayed fixed**:
0 UTF-16, 0 GUID groups, 0 `(cycle)` / `(unresolved)` / `(depth limit)` / node-budget
truncations, 0 `((none))`, 0 F-6 section leaks, max line **730 chars**. The 5 `NewEnumerator`
and 305 truncations are the 10th round's fixes, still uncompiled. Curve, AnimBP and montage
paths all render correctly — `AB_Als_PostProcessing` at 22 lines with empty EventGraph and
Functions sections is *correct*, since ALS keeps its logic in C++ (README "Architecture
Insight").

**D-3 — One-File-Per-Actor packages were being dumped as assets.** UE5's World Partition /
OFPA writes one package per placed actor under `__ExternalActors__` / `__ExternalObjects__`.
These are **level content instances, not assets**: a placed cube's `ActorLabel`, `FolderGuid`
and mesh override. In ALS they are **229 of 570 dumps (40%)**, and they bury the 26 AnimBPs
and 13 Blueprints that a decode actually wants.

⚠ This is not an ALS quirk — **our own project has `Content/__ExternalActors__`**, so any
`DumpBPFolder /Game -recursive` on Summoning inherits the same noise.

Now skipped by default with a **visible manifest row**
(`skipped (external actor package; -external to include)`) and a policy line in the manifest
header, per the silence-is-a-bug-signal doctrine — the count still reconciles. Pass
`-external` to include them, or `DumpBP` a single one by path when a placed-actor override
actually matters. Contained to the batch command; zero blast radius on the diff walker.

**Known-answers:** ALS re-run drops to ~341 dumped with 229 skipped rows carrying the
external-actor reason, and rows still sum to 570; a `-external` run reproduces today's 570.

### 2026-07-26 (10th round) — ⭐ TCF combat timing is READABLE. Two last defects fixed.

9th round verified: F-6 section-leak **14,375 -> 0** lines, `((none))` **616 -> 0**,
controls intact (`bEnableRootMotion` 1414, `CommonTargetFrameRate` 168).

⭐⭐ **The question this milestone existed to answer is answered.** TCF's plugin folder ships
no montages, but the **demo samples do** (Mixamo 36, Samurai 57), and one attack montage now
reads as a complete combat spec:

```
t=0.0001  dur=0.8147  ANS_BufferInputs_C
    Input Buffer Type = InstantInputFire  (default: HighestPriorityInput)
    Buffer Info[0] (BufferInfo)
      AllowedInput = .../AttackRelease_Input
      InputBufferSpecialProperty = BP_PlayAnimationBufferProperty_C
        Anim Montage to Play = .../GhostSamurai_APose_Attack04_Root_Montage1   <- the combo link
t=0.8920  dur=0.2497  ANS_TempestStandardRotation_C
    RotationPropertiesToPerform[0..2] = RotateToTargetedActor / RotateToInputDirection / RotateToCombatTarget
t=0.9276  dur=0.3125  ANS_Trace_C                                              <- the attack window, 312 ms
    Trace to Control = ((TagName="Trace.Right Hand Weapon"))
    AssignedAttackProperty -> AttackProperty = BP_NormalAttackProperty_C
      ImpactProperties[0] -> BP_DamagePerImpactProperty_C
        Damage Applications Per Impact Result =
          Impact.Result.Block -> Attribute.Posture 20.0                        <- posture-on-block
      ImpactProperties[1] -> BP_HitFeelPerTargetProperty_C
        Hit Feel Per Target[0].Target Gameplay Tag = Character.AI.Enemy
```

Six levels of instanced nesting, rendered correctly. Window timings, the combo graph, the
rotation policy and the posture numbers are all in text. The whole TCF notify vocabulary is
present across the demos: `ANS_Trace` (44), `ANS_BufferInputs` (112),
`ANS_TempestStandardRotation` (80), `ANS_AddCombatStatus` (30), `AN_AnnounceAttack` (30).

**Two defects that read-through exposed, both fixed:**
- **D-1 Blueprint enum property values dumped their INTERNAL name** —
  `Swing Direction = NewEnumerator13  (default: NewEnumerator3)`. 586 occurrences.
  `ResolveEnumPinValue` had always done this for *pins*; the property side never did. New
  `TryExportEnumValue` handles `FEnumProperty` and enum-typed `FByteProperty`, preferring
  the display name and falling back to the internal name rather than to nothing.
- **D-2 the 220-char value cap was cutting combat data mid-record.** The damage map above
  lost its second impact result to truncation. 2979 values were truncated. Cap raised to
  **1000** with the marker kept. ⚠ This is a mitigation, not the fix: the right answer is
  structural recursion into map/struct values instead of exporting them as one line. Filed.

### 2026-07-26 (9th round) — T4 VERIFIED, plus the duplication it exposed

**T4 works.** Verified on Summoning (92 anim assets), GASP (1578) and TCF (0 — see below).

⭐ **The H9 check now exists in text.** `AS_Run_Turn_L90_Rfoot` dumps its full instrumentation
stack, and every §8 claim in `CLAUDE.md` is now verifiable rather than asserted:
```
RemainingTurnYaw   : 48 key(s)  value[-90.0000..0.0000]   time[0.0000..3.8667]
TurnYawWeight      :  2 key(s)  value[0.0000..1.0000]     time[0.0000..1.5000]
movedata_speed     : 25 key(s)  value[286.0515..375.0000] time[0.0333..3.8667]
contact_l/contact_r: 95 / 98 key(s)
Distance           : 103 key(s) value[0.0000..1137.0325]
phase, enable_warping, steeringtargettime, disable_additiveleans
```
The L90 clip's authored turn is exactly -90 deg; rotation authority dies at 1.5s of a
3.87s clip; the clip's authored speed dips to 286 and recovers to 375. ⭐ It also settles
the `CLAUDE.md` §8 spelling flag: the curve **is** `disable_additiveleans`, trailing S.

**Montage half verified** on GASP's interaction montages — a notify STATE prints its window
*and* its instanced payload, which is precisely the `ANS_AttackTrace` shape:
```
t=0.0001  dur=4.3143  end=4.3144  track=1 '2'  MotionWarping (AnimNotifyState_MotionWarping)
    RootMotionModifier = RootMotionModifier_PrecomputedWarp
      TranslationWarpingCurve = (CurveType=EaseInOut,EndRatio=0.807407)  (default: (EndRatio=1.000000))
      RotationWarpingCurve = (StartRatio=0.600000,EndRatio=0.884615)     (default: (EndRatio=1.000000))
      bEnableSteering = True  (default: False)
      WarpTargetName = SmartObject  (default: None)
```

⚠ **TCF ships ZERO montages** — 318 assets, no `AnimMontage` among them. Its notify
*classes* are there (`ANS_AttackTrace` as a Blueprint), but the montages that carry them
live in the demo/game content, not the plugin. So "how long is the deflect window" needs
**TCF's demo content dumped**, not the plugin folder. T4 is proven by GASP's equivalent.

**Two defects this exposed, both fixed:**
- **F-6 the trailing generic diff duplicated the structured sections.** 179,352 of 448,929
  anim-dump lines (**40%**) sat after `=== Properties`, re-printing `Notifies[...]` (12,618
  lines) and `AnimNotifyTracks` (1,670) — and re-running T1's instanced recursion, so every
  notify's payload appeared twice. Exactly the F-4 disease I had already fixed for BT nodes
  and then reintroduced here. The BT filter is now generalised into
  `AppendFilteredProperties`, which skips named top-level entries **and everything nested
  under them** (indent-aware, so `Notifies[0]`'s children go with it). Both call sites use it.
- Skeleton notifies (a NAME with no class object) printed `((none))`, which reads as a
  broken reference rather than the normal thing it is. 616 occurrences; now the class
  suffix is simply omitted.

### 2026-07-26 (8th round) — T4 animation assets

The last M2 item. Handles **any `UAnimSequenceBase`** — montages, sequences, composites —
not just montages, because our own window forensics (H10 commitment windows, the jump
program) live on sequences too.

Per asset: skeleton, play length, rate scale, then
- **Notifies** — sorted by trigger time (authored order is not time order), each with
  `t=` / `dur=` / `end=` for notify STATES and `t=` alone for instant notifies, the track
  index **and its authored track name**, the notify class, and **the notify's own
  properties through T1's instanced recursion**. That last part is the whole point: TCF's
  `ANS_AttackTrace` carries an instanced attack property and `ANS_SetAllowedInputs` carries
  `AllowedInputs[]`, so the window and its payload land together.
- **Sections** (montages) — name, time, and next-section, which is the montage's control
  flow.
- **Slots + segments** (montages) — slot name, then per segment the referenced animation,
  start position, the `[AnimStart..AnimEnd]` sub-range, play rate and loop count.
- **Curves** — name, key count, value range and time range, then the keys themselves up to
  64 with an explicit "N more keys" marker. This is slightly beyond T4's letter (the parked
  "animation metadata" line covers curves *next to Sequence Players*, which is a different
  feature), but H9 — "missing animation curves fail SILENTLY to zero; verify existence on
  the actual asset" — is a hard rule here, and this answers it directly.
- **Properties** — the generic diff closes the section, so root motion, looping and
  interpolation come along without a hand-listed field set that would drift against the
  engine.

**Known-answers:** a TCF montage with `ANS_AttackTrace` shows the trace window's start and
duration *and* its instanced attack property nested underneath; a GASP locomotion clip lists
`contact_l` / `contact_r` / `movedata_speed` with key counts; our own `AS_Run_Turn` family
shows `RemainingTurnYaw` with a value range (the H9 check, in text, for the first time).

### 2026-07-26 (7th round) — ✅ ALL SIX FIXES VERIFIED. M1 + M2 (less T4) CLOSED.

Re-verified on GASP (3083) + AGLS (7287) + TCF (318):

| Known-answer | Before | After |
|---|---|---|
| F-1 GUID group, anywhere | 11 | **0** (Chooser lines cleaned too) |
| F-2a VariableSet as data source | 70 | **0** real (10 hits are legitimate `Set members in …` / `Set Defense Impact Values(…)` names — my metric's false positives) |
| F-2b macro re-expansion | 1449 | **0** real (7 hits are all `For Each Loop(...)` — the elision marker, not re-expansion) |
| F-3 longest line (AGLS) | 11,052 ch | **3,509 ch** (−68%); lines >2000: 79 → 11 |
| F-4 BT structural noise | every node | **0** |
| F-5 bare `TArray` | 19 structs | **0** — `Damage To Apply : TArray<FS_DamageToApply>` |
| Encoding | 87% UTF-16 | **0 of 10,688** |

3790 elision markers are in active use across the three projects — the memo is what holds
the line length down. The residual AGLS length is genuine expression complexity in ALSv4's
climbing math (`CollapseGraph_0(Make ALS Component and Transform(…))`), not a defect.

**The BT now reads as a decode artifact should:**
```
Selector (BTComposite_Selector)
  [child 0]
    Decorator: BTD_CheckStates (BTD_CheckStates_C)
      AI State Tag = (GameplayTags=(("State.ReturnToPost"),("State.Go To Location"),…))
      FlowAbortMode = Self  (default: None)
    Sequence (BTComposite_Sequence)
      [child 0]
        Task: Move To (BTTask_MoveTo)
          BlackboardKey = (SelectedKeyName="Destination")
```

**Status: M1 core and M2 T1/T2/T3 are CLOSED and verified at 10,688 assets across three
unrelated projects.** T4 (montage notify tracks) is the only milestone item left.

### 2026-07-26 (6th round) — the four-project stress test: 2 fixes verified, 6 defects found

Verified across **11,551 dumps** — GASP (3083), **AGLS/ALSv4 (7287, a new MM project)**,
Narrative plugins (855), **TCF (318)**.

**✅ 5th round verified.** Encoding: **0 UTF-16 files of 11,551** (was 87%). GUID leakage in
GASP: 11 → **3**, and those 3 are exactly the predicted Chooser `ColumnsStructs` lines.

**⭐ M2's whole purpose is met.** TCF: 321 found / 318 dumped / 3 redirectors, 9980 lines of
text from what the decode called "policy locked in 319 uassets". T1 delivers on the real
target — `ImpactProperties[0] (InstancedImpactProperties) → ImpactProperty =
BP_DamagePerImpactProperty_C` — and every T2 path is finally exercised for real
(19 UserDefinedStructs, 4 enums, the BT + Blackboard, curves, InputActions, a DataTable).
The BT dump reads as intended: `Decorator: BTD_CheckStates` with
`AI State Tag = (GameplayTags=(("State.ReturnToPost"),("State.Go To Location"),…))`.

**Six defects the scale exposed — all fixed in this round:**

- **F-1 GUIDs in exported VALUES.** The same `_<index>_<32 hex>` group appears inside
  exported *property values*, where no pin exists: AGLS's
  `Settings = (VisualStaticMesh_49_2616A0B0…="…")`, and the 3 Chooser lines previously
  written off as out of scope. New `StripMemberGuids` runs over exported text, so both are
  now covered — the Chooser exclusion is retired.
- **F-2a VariableSet read as a data source** rendered the *assignment*:
  `Set Local Damage To Apply(<entire assigned expression>)`. It now renders the variable
  name. 70 occurrences.
- **F-2b Macro instances re-expanded their inputs at every output read**, so nested loops
  squared: `For Each Loop(For Each Loop(…).Array Element).Array Element`. Macros now render
  `MacroName.PinName` — their inputs are already printed on the macro's own exec line.
  **1449 occurrences.**
- **F-3 shared DAG sub-expressions re-expanded per consumer** — the big one. One AGLS line
  reached **11,052 characters** holding 62 copies of `FindObjectCenterTraceConfig(...)` and
  31 of `LineTraceSingle(...)`. This is the cost of Phase 0.1 removing the visited set: that
  was right for correctness (the set produced false `(cycle)` claims) but left re-expansion
  unbounded. Now a **per-top-level-expression memo** prints the first occurrence in full and
  elides repeats as `Name(...)`. Note this is *not* a return to 0.1's bug: the marker claims
  "arguments elided", never "cycle", and the memo is scoped to one expression.
- **F-4 BT structural noise.** `TreeAsset`, `ParentNode`, `Children`, `Services`,
  `Decorators` repeated on every node, burying the authored settings the tree walk exists to
  show. Filtered — the tree walk already renders that structure.
- **F-5 container field types truncated.** `Damage To Apply : TArray` should be
  `TArray<S_DamageToApply>`; `GetCPPType` puts the inner type in its `ExtendedTypeText`
  out-param, which the T2 struct dumper was discarding. 19 TCF structs were affected.

**Known-answers for the next round:** no `For Each Loop(` as an *argument*; no `= Set X(`;
AGLS's longest line well under 11k (the `BP_ClimbingMovementComponent` `Return (CMC Ledge=…)`
line is the benchmark); `Damage To Apply : TArray<...>`; no `_<digits>_<32 hex>` anywhere at
all, Chooser included; BT nodes free of `TreeAsset`/`ParentNode`.

**Observed, not fixed (filed):** `FText` values export with localisation GUIDs
(`NSLOCTEXT("[56ACCA…]", "500390…", "Forward")`) — the readable string is the third
argument. Cheap to reduce to the source string if data-table/UI dumps become load-bearing.

**Narrative note:** 855 assets across 5 mounts, but 553 are Texture2D and the rest is mostly
UI widgets — the plugins ship a *framework*, and the quest/dialogue **content** lives in a
game project, not here. Nothing to decode that `NARRATIVE3_DECODE_2026_07_24.md` did not
already cover from source.

### 2026-07-26 (5th round) — ⭐ dumps were being written as UTF-16; plus the last GUID cases

**Output encoding — the significant one.** `FFileHelper::SaveStringToFile` defaults to
`EEncodingOptions::AutoDetect`, which writes **UTF-16** the moment a string contains any
non-ASCII character. Measured on `GASPGame.7z`: **2687 of 3083 dumps (87%) were UTF-16LE.**
For a tool whose entire product is plain text read by grep and by AI, that is a defect in
the deliverable itself, not a cosmetic one. `sed`, `cut` and `cat -v` fail outright on
those files ("illegal byte sequence"); `grep` on macOS happens to decode them, which is
why it went unnoticed for two verification rounds and why the marker censuses in the
4th-round entry remain valid.

All four write sites now pass `EEncodingOptions::ForceUTF8WithoutBOM` (single-asset
`DumpBP`, `DumpAnimBP`, the batch per-asset writes, and the manifest). Contributing cause
on our side: three emitted literals of ours used em-dashes; those are now ASCII, and any
future non-ASCII in *content* is now harmless rather than encoding-flipping.

**The last GUID cases.** The 4th-round fix took leakage from 494 occurrences / 21 files to
**11 / 6**. Reading the residue found two distinct causes:
- **8 of 11 — the GUID group sits in the MIDDLE**, not at the tail, when the member is a
  SPLIT struct pin: `GridSizes_16_782DDA9B..._X`, `LandVelocity_40_AD6ED73D..._Z`.
  `FriendlyPinName` now scans for the `_<index>_<32 hex>` group anywhere past the first
  token instead of only stripping a suffix. The Break-struct branch also became
  parent-aware, so a split member reads `CharacterProperties.LandVelocity.Z` rather than
  `…LandVelocity_Z`.
- **3 of 11 — not pin names at all**: the Chooser table `ColumnsStructs` property, whose
  raw `ExportTextItem_Direct` text embeds a property-binding chain containing GUID'd names.
  That is engine text-export inside a value we print verbatim (and truncate at 220 chars).
  **Out of scope by decision** — Chooser Table dumping is a parked non-goal.

Known-answers for the next round: no `_<digits>_<32 hex>` anywhere outside the three
Chooser lines; `TraversalCheckResult.BackLedgeHeight` and `UpdateMaterials.GridSizes.X`
both clean; and **zero dumps with a `fffe` BOM** (`head -c2` on any dump).

### 2026-07-26 (4th round) — GUID suffix leak (found by the whole-game verification)

`FriendlyPinName` strips the internal `_<index>_<32 hex GUID>` suffix that pins generated
from **Blueprint** struct members carry; native pin names pass through untouched. Applied
at every raw-`PinName` site: the N1 Break-struct rendering and `QualifyOutput` (both the
split-sub-pin derivation and the `.PinName` append), function signatures, event parameter
lists, the Property-Access pin fallback, and both `GetDisplayName` fallbacks.

Found because N1 *worked*: surfacing member names for the first time also surfaced their
GUIDs — `TraversalCheckResult.BackLedgeHeight_64_FA78930E475E1B89F0CCC5BC6F6043A3`, and
`TriggerVisLog(E_FoleyEventSide Params_Side_9_D749B6B548D0A4778009A58DC6BD468F, …)`.
494 occurrences across 21 of 3083 files — small, but concentrated in the traversal system,
exactly the content worth reading. Same disease the T1 walker already cured for *property*
names with `GetAuthoredNameForField`; this is the *pin* half.

### 2026-07-26 (3rd round) — N1 + N2 — ✅ VERIFIED on the whole-game GASP dump

`GASP_WholeGameDump.7z`: `DumpBPFolder /Game -recursive`, **3132 assets, 3083 dumped,
49 skipped (all redirectors, each reasoned)**, manifest rows == assets found, 3083 files
on disk. Largest single run to date and the first at TCF-like scale.

- **N1 Break-struct** ✓ `CanSprint` opens with `CharacterInputState.WantsToSprint` where it
  used to read `Break S Player Input State(CharacterInputState)`.
- **N1 split pins** ✓ `CalculateMaxSpeed` now separates all four range endpoints:
  `MapRangeClamped(StrafeSpeedMap, 0, 1, Select(…).X, Select(…).Y)` and
  `MapRangeClamped(StrafeSpeedMap, 1, 2, Select(…).Y, Select(…).Z)`. Previously all four
  were the same string. Also `NormalizedDeltaRotator(…).Yaw` in `CanSprint` — it was
  invisible that only Yaw is compared.
- **N2 depth 8** ✓ the formerly-bare `Select` is fully expanded:
  `Conv_VectorToRotator(Select(GetCurrentAcceleration(), GetPendingMovementInputVector(),
  IsLocallyControlled()))`. The `(...)` marker appears **23 times in 3083 files**, so depth
  8 is close to sufficient and the residue is now visible rather than silent.
- Guards: 0 `(cycle)`, 0 `(unresolved)`, 0 node-budget truncations; 48 `(already shown)`
  from T1's cycle guard doing its job on instanced graphs.

**⭐ 0.8 parenthesisation FINALLY exercised** — unvalidated since 2026-07-06 because GASP's
CMC ABP has no compound rules. The whole-game dump has them, and they are correct:
`(NOT IsCurrentAssetLooping(…)) && (GetCurrentAssetTimeRemaining(…) <= 0.750000)` and
`Select(Value, Run, ((Value == Sprint) && Trj_IsCircling))`.

**⭐ T2 curve dumper exercised, and it retired a measurement caveat.**
`Curve_StrafeSpeedMap` dumps exact: `(0,0) (45,0) (80,1) (100,1) (135,2) (180,2)` linear.
The host project's `SPRINT_DECODE_2026_07_25.md` §2d had that curve read off a screenshot
with a ±3° caveat. Combined with the `.X/.Y/.Z` fix above, GASP's whole directional speed
law is now readable from text: the curve maps |direction| → 0..2, and `CalculateMaxSpeed`
lerps `Speeds.X`→`.Y` over 0..1 and `.Y`→`.Z` over 1..2.

Also visible in real rules and already filed under Phase 2: `Not Equal (Enum)(A, B)` still
renders function-style instead of `A != B`.

### 2026-07-26 (3rd round, as committed) — N1 + N2

**N1 — output-pin identity.** New `QualifyOutput` helper, applied at every return site in
`BuildExpressionFromPin` that can serve a multi-output node (7 sites):
- **Split struct sub-pins** → `.X` / `.Y` (derived by stripping the parent pin's name), so
  `Conv_Vector2DToVector(?, Get IA_Move, Get IA_Move, 0.0)` becomes `…, Get IA_Move.X,
  Get IA_Move.Y, …` and a struct `Select` feeding a range's Min and Max stops rendering
  identically for both.
- **Nodes with >1 top-level data output** (function out-params) → `.PinName`. Sub-pins of a
  split output are excluded from that count, or every split node would be "multi-output".
- **`UK2Node_BreakStruct` gets a dedicated rendering**: `Source.Member` instead of
  `Break Foo(Source)`. `Break S Player Input State(CharacterInputState)` →
  `CharacterInputState.WantsToSprint` — the member IS the information.

**N2 — depth truncation is no longer silent.** At `MaxDepth` the walker appended nothing,
so a truncated node read exactly like a genuine zero-argument call. It now emits `Title(...)`
whenever the node actually has data inputs (`HasDataInput`), and `DefaultExpressionDepth`
goes **6 → 8** — GASP's `CanSprint` truncated at 7. The marker is the safety net: if 8 is
still short, the dump says so instead of lying.

**Known-answers for the next verification round:**
- `CanSprint` — the previously-bare `Conv_VectorToRotator(Select)` must now be either fully
  expanded (depth 8) or explicitly `Select(...)`; and the AND's first operand should read
  `CharacterInputState.WantsToSprint`-style rather than `Break S Player Input State(…)`.
- `CalculateMaxSpeed` — the two `Select(WalkSpeeds, RunSpeeds, SprintSpeeds, Gait)` args
  must now differ from each other.
- Eyeball line length: depth 8 is the readability risk. Drop to 7 if it bites.

### 2026-07-26 — M1 core + M2 T1/T2/T3 (⚠ pending machine-2 compile + verification)

**M1 core — the expression collapse (Gaps 1.1 / 1.2 / 1.4 / 1.6, and T5's question):**
- Root cause: a `UK2Node_CallFunction` early-out emitting `MemberName + "()"` without
  reading the node's pins, hand-copied into `GetDataInputSummary`
  (`BlueprintDumpUtils.cpp:659`), `WalkExecChain`'s VariableSet case (`:765`) and
  `FormatDataPins` (`AnimBPDumper.cpp:1102`) — plus `BuildExpressionFromPin`'s own
  `return FuncNameStr;`.
- New `FBlueprintDumpUtils::ResolvePinSourceLabel` is now the single "what drives this
  pin" entry point; all three copies deleted in favour of it (this also closes the
  Phase 2 "triplicated source-label logic" item, which was never mere hygiene — the
  duplication WAS the bug's habitat).
- `BuildExpressionFromPin` recurses into function arguments:
  `Lerp(1.0, Clamp(SafeDivide(Speed2D, Rate), Min, Max), Alpha)`. Trailing unset optional
  params are dropped; a gap in the middle stays `?` so argument positions keep meaning.
- New `FormatLiteralPinValue`: enum-resolved, **string/name/text literals quoted**
  (`GetCurveValueFromAnimation("MoveData_Speed", t)`), object literals by name via
  `DefaultObject`, `DefaultTextValue` handled.
- `DefaultExpressionDepth` 4 → 6 for data pins (transition rules stay at 10).

**M2/T1 — instanced sub-object recursion:**
- `AppendPropertyDiffs` reworked to walk **value pointers** instead of UObject containers,
  so it can descend through structs and arrays into instanced content.
- `CPF_InstancedReference | CPF_PersistentInstance | CPF_ContainsInstancedReference`
  removed from the skip mask; instead: instanced objects recurse, arrays of them recurse
  per element (with an element-count line when the count differs), and structs carrying
  `CPF_ContainsInstancedReference` descend into their fields (the `FInstanced*Properties`
  wrapper shape).
- Baseline selection: the archetype counterpart when same-class, else the value's **own
  class CDO** — which removes the `AppendSubobjectDiffs` "field diff undefined" early-out
  instead of working around it. Class-overridden subobjects now dump, marked
  `vs class defaults`.
- Guards: depth cap 4, cycle guard (`already shown above`), components skipped (owned by
  the dedicated subobject/SCS passes), and — **found while self-reviewing** — objects
  outered directly to a package are treated as REFERENCES, not inline content, so an
  EditInlineNew *asset* pointer cannot splice another asset's guts into the dump.
- Property names now print via `GetAuthoredNameForField`, so BP-struct fields lose their
  internal GUID suffix.
- New public `FBlueprintDumpUtils::DumpObjectPropertyDiffs` — the reusable engine behind
  T2's generic dumper.

**M2/T2 — non-Blueprint asset types (new `AssetDumper.{h,cpp}`):**
- `DumpBP` no longer hard-gates on `LoadObject<UBlueprint>`. It loads as `UObject` and
  dispatches: AnimBlueprint → Blueprint → UserDefinedStruct (field list) → UserDefinedEnum
  (enumerators, display + internal name) → Curve Float/Vector/LinearColor (exact key
  lists) → DataTable (row struct + rows) → BehaviorTree (composites/tasks/decorators/
  services with each node's authored properties) → BlackboardData (keys + key types) →
  generic property-diff fallback.
- The asset's real class is always in the header, and an unhandled type says so — the old
  "could not load" for a perfectly loadable non-BP asset is gone.
- ⚠ Loading a bare package path as `UObject` can return the `UPackage`; `LoadAsset` builds
  the fully-qualified object path first and unwraps redirectors/packages.
- Build.cs: `+AIModule` (BehaviorTree/Blackboard), `+AssetRegistry` (T3).

**M2/T3 — batch dump + manifest:**
- New `DumpBPFolder /Game/Path [-recursive] [-filter=ClassName]`. Walks the asset registry
  (with a synchronous scan first, since plugin content is not always indexed), dispatches
  per type, and writes one file per asset into a **mirrored** tree under
  `Saved/BlueprintDumps/`.
- Emits `_manifest.txt` with one row per asset found — `ASSET | TYPE | dumped -> path` or
  `skipped (reason)` — plus a `found / dumped / skipped` summary. Row count == asset count
  by construction, including filtered ones, so coverage is auditable rather than assumed.
- The class filter is applied from registry metadata **before** loading.
- Single-asset `DumpBP` still writes to `Saved/AnimBPDumps/` (unchanged, to avoid breaking
  existing habits); the Phase 2 directory rename is therefore now half-done.

### 2026-07-19 — Class Defaults overrides section (⚠ pending machine-2 compile + re-dump verification)
**Gap (filed same day, host-project driven):** dumps showed components and graphs but no
CDO property state — a BP override of an inherited C++ knob (e.g. a movement-component
threshold changed in Class Defaults) was invisible, forcing behavioral inference from
gameplay logs ("override trap" conversations). New section closes it:

- `DumpClassDefaultOverrides` (`BlueprintDumpUtils`) — `=== Class Defaults (overrides vs
  <Parent>) ===`, three passes, all read-only reflection:
  1. **CDO vs parent CDO** over the PARENT class property scope (inherited properties
     only — BP-added variables stay in the Variables section; offsets valid in both
     containers by construction)
  2. **Default subobjects** (C++-created components) vs `GetArchetype()` — where
     Class-Defaults edits to inherited components actually live; bracketed sub-heading
     per component, emitted only when diffs exist
  3. **SCS component templates** (BP-added components) vs their archetypes, marked
     `BP-added`
- Comparison: `Identical_InContainer` per static-array element (5.7-verified: per-index
  API, default index 0 — NOT whole-array). Object references compare by pointer, except
  subobjects of the compared pair (never pointer-equal) → name + class match.
- Noise filter: skip `Transient | DuplicateTransient | NonPIEDuplicateTransient |
  Deprecated | InstancedReference | PersistentInstance | ContainsInstancedReference`
  (instanced POINTERS always differ CDO-vs-parent-CDO; their CONTENT is pass 2).
- Values via `ExportText_InContainer` (delta=nullptr → full export), object refs as
  path names, 220-char truncation. Both sides printed: `Knob = X  (default: Y)`.
- **Absence is meaningful by design**: header + `(not listed = inherited default)`
  footer ALWAYS print, so "knob not in dump" = "knob at C++ default" in one glance.
- Wired into BOTH dumpers: `DumpBP` (after component tree) and `DumpAnimBP` (after
  variables) — ABP Class-Defaults overrides were the original override-trap class.
- Class-overridden subobjects (archetype class ≠ instance class) are skipped, not
  field-diffed. Known non-goal: nested subobjects below one level.

### 2026-07-06 — Phase 0 complete + Blend Stack fidelity cluster
All of Phase 0, in one commit (signature changes thread through all walkers):
- **0.1** `BuildExpressionFromPin` visited set REMOVED — data graphs are DAGs, `MaxDepth`
  is the only bound; shared Get nodes no longer print `(cycle)`
- **0.8** nested operator results parenthesized (`Grouped` at depth > 0); fabricated `0`
  operand → `?` (Phase 2 nit, same line)
- **0.2** exec knot cycle check moved BEFORE knot pass-through (crash fix)
- **0.3** node budget (2000/walk) in both walkers, `(truncated: node budget reached)`
- **0.4** `[DISABLED]` annotation: pose walker, exec walker, event headers, transitions
- **0.5** main AnimGraph = top-level graph named `AnimGraph`, fallback first top-level,
  then first any
- **0.6** pose-chain revisits print `(-> see above: <label>)`
- **0.7** null-guarded `LinkedTo[0]` at all sites incl. the 4th (VariableSet case in
  `WalkExecChain` + its `SourceNode` title deref)
- **0.9** `(unresolved)` vs `(automatic)` keyed on `bAutomaticRuleBasedOnSequencePlayerInState`

Blend Stack / anim-node fidelity (M1 1.5b + 1.1b + 1.2b, verified-5.7 APIs):
- **(A)** `[Settings: ...]` — inner `FAnimNode_*` struct read via `GetFNodeProperty()`,
  non-default editable properties printed for EVERY anim node (skip transient, pose links,
  visible-pin duplicates, title-riding asset refs); Blend Stack always shows
  `MaxActiveBlends`/`bUseInertialBlend`/`BlendTime`/`BlendProfile`
- **(B)** `Pin=[bound: Path]` — pin property bindings read reflectively from the node's
  `Binding` object (`PropertyBindings` map; value struct is public, impl header stays
  un-included); bound pins print the binding INSTEAD of the stale default
- **(C)** `[OnUpdate: Fn]` / `[OnBecomeRelevant: Fn]` / `[OnInitialUpdate: Fn]` after node
  labels — the three `FMemberReference` members are public, read directly (simpler than
  the planned `GetBoundFunctionsInfo` route)
- **Sub-graphs**: `WalkPoseChain` recurses `UEdGraphNode::GetSubGraphs()` (generic; SM and
  linked-layer nodes excluded) → Blend Stack per-sample graph no longer a mute dead-end

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
