# Blueprint Dump Tool — UE5 Plugin

Console commands that dump Animation Blueprints and Blueprints to human-readable text files. State machines, transitions with boolean rule expressions, pose chains, nested sub-SMs, exec chains, variables, components — everything you need to understand a Blueprint without screenshots.

Built for feeding UE5 project structure to LLMs, documentation, diffing, and code review.

## Installation

1. Copy the `BlueprintDumpTool` folder into your project's `Plugins/` directory
2. Regenerate project files (right-click `.uproject` → Generate Project Files)
3. Build and launch the editor

The plugin loads automatically as an editor-only module.

## Usage

In UE Editor console (Output Log or `~` key):

```
DumpAnimBP   /Game/Path/To/YourAnimBP
DumpBP       /Game/Path/To/YourAsset
DumpBPFolder /Game/Path [-recursive] [-filter=ClassName]
```

No default path — prints usage help if called with no arguments. Works with any asset in any project.

`DumpBP` is not Blueprint-only: it dispatches on the loaded asset's class, so DataAssets,
BehaviorTrees, Blackboards, Curves, DataTables and user-defined structs/enums all dump.
Anything without a specialised dumper gets a generic property dump that names its class.

`DumpBPFolder` batch-dumps a whole content path into a mirrored directory tree and writes a
`_manifest.txt` listing every asset found as `dumped` or `skipped (reason)` — the artifact
that makes a large decode pass auditable instead of assumed.

**Output:** `{ProjectDir}/Saved/AnimBPDumps/{Name}_Dump.txt` (single asset)
and `{ProjectDir}/Saved/BlueprintDumps/{mirrored/path}/` (batch).
Single-asset dumps also print their first 200 lines to the Output Log.

## Example Output

### AnimBP Dump

```
=== AnimBP: ABP_Character ===
Skeleton: SK_Mannequin
Parent Class: UMyAnimInstance
Graphs: AnimGraph

=== AnimGraph (pose chain from Root) ===
Output Pose (Root)
  \-- Control Rig
    \-- Blend Poses by bool (bUseUpperBody) (True Blend Time=0.100000, False Blend Time=0.100000)
      \-- [True Pose]
      \-- Layered blend per bone
        \-- [Base]
        \-- Slot: UpperBodySlot
      \-- [False Pose]
      \-- State Machine: MovementSM

  === State Machine: MovementSM ===
  Entry -> Idle
  States: Idle, Start, Move, Stop
  Transitions:
    Idle -> Start: bHasMovementInput [Inert 0.20s] P1
    Start -> Move: MovementState == EMovementState::Moving [Std 0.20s] P1
    Move -> Stop: !bHasMovementInput && Speed < 10.000000 [Std 0.15s] P1
    Stop -> Idle: StateWeight >= 1.0 [Std 0.10s] P1

  --- State: Idle ---
    State Result
      \-- Sequence Player (Sequence=Idle_Anim)

  --- State: Move ---
    State Result
      \-- State Machine: DirectionalMoveSM

        === State Machine: DirectionalMoveSM ===
        Entry -> Forward
        States: Forward, Right, Backward, Left
        Transitions:
          Forward -> Right: MovementDirection == EDirection::Right [Inert 0.20s] P1
          Right -> Forward: MovementDirection == EDirection::Forward [Inert 0.20s] P1
          ...
```

### Blueprint Dump

```
=== Blueprint: BP_MyCharacter ===
Parent Class: ACharacter

=== Components (C++) ===
  DefaultSceneRoot (USceneComponent)
    +-- CapsuleComponent (UCapsuleComponent)
    +-- CharacterMovement (UCharacterMovementComponent)

=== Components (Blueprint) ===
  Mesh (USkeletalMeshComponent) [AnimClass=ABP_Character_C]
  SpringArm (USpringArmComponent)
    +-- Camera (UCameraComponent)

=== Class Defaults (overrides vs Character) ===
  bUseControllerRotationYaw = False  (default: True)
  [CharacterMovement (UCharacterMovementComponent)]
    MaxWalkSpeed = 500.000000  (default: 600.000000)
    RotationRate = (Pitch=0.000000,Yaw=720.000000,Roll=0.000000)  (default: (Pitch=0.000000,Yaw=360.000000,Roll=0.000000))
  [SpringArm (USpringArmComponent), BP-added]
    TargetArmLength = 350.000000  (default: 300.000000)
  (not listed = inherited default)

=== Variables ===
  MaxHealth: float (Default: 100.0) [EditAnywhere]
  bIsDead: bool
  CurrentWeapon: AWeapon*

=== EventGraph ===
--- Event: BeginPlay ---
  -> Call InitializeAbilities()
  -> Set bIsReady = true

=== Functions ===
--- TakeDamage(float DamageAmount) -> bool ---
  -> Branch (Condition=bIsDead)
    [True]
      -> Return
    [False]
      -> Set CurrentHealth = CurrentHealth - DamageAmount
      -> Branch (Condition=CurrentHealth <= 0)
        [True]
          -> Call Die()
```

## What It Captures

### AnimBP (`DumpAnimBP`)

| Feature | Detail |
|---------|--------|
| **Pose chain** | Backward walk from Root through all anim nodes |
| **State machines** | Entry state, all states listed, nested SM recursion |
| **Transitions** | Source -> Target with reconstructed boolean rule expressions |
| **Transition metadata** | Blend type (Std/Inert/Custom), duration, priority, AutoRule flag |
| **Data pins** | Connected variables and non-default values shown inline |
| **Cached poses** | Save/Use Cached Pose with cache names, including orphaned branches |
| **Multi-input nodes** | Pin labels shown for blend nodes (`[Base Pose]`, `[Blend Pose 0]`) |
| **Conduits** | Listed separately with their rule expressions |
| **State aliases** | UE5 state alias nodes detected and listed |
| **Nested SMs** | Full recursive dump (outer SM -> nested SM -> state content) |
| **Animation layers** | Interface layers, self-linked layers, monolithic AnimBP support |
| **Variables** | All Blueprint variables with types, defaults, and property flags |
| **Class Defaults** | CDO property overrides vs the parent class (not listed = inherited default) |
| **EventGraph** | Event handlers with exec chain walking |
| **Functions** | Blueprint functions with signatures, parameters, and exec chains |

### Blueprint (`DumpBP`)

| Feature | Detail |
|---------|--------|
| **Component tree** | Both Blueprint and C++ components with hierarchy |
| **Class Defaults** | CDO + component property overrides vs the parent class / C++ defaults |
| **Variables** | All Blueprint variables with types, defaults, and flags |
| **Interfaces** | Implemented interfaces listed |
| **EventGraph** | Event handlers with exec chain walking |
| **Functions** | Function signatures and exec chain bodies |

### Other assets (`DumpBP` / `DumpBPFolder`)

| Type | Detail |
|------|--------|
| **DataAsset / any UObject** | Properties that differ from class defaults, recursing into instanced sub-objects |
| **BehaviorTree** | Composite/task tree with decorators, services, and each node's authored properties |
| **BlackboardData** | Keys with key types and instance-sync flags |
| **Curves** | Exact key lists (time, value, interp, tangents) for Float/Vector/LinearColor |
| **DataTable** | Row struct plus every row's fields |
| **User struct / enum** | Field list with types; enumerators with display + internal names |

### Expression Resolution

Transition rules and exec chains resolve expressions recursively:
- **Operators**: `Greater_FloatFloat` -> `>`, `EqualEqual_ByteByte` -> `==`, etc. (13 operators)
- **Function calls**: recursed into their arguments — `Lerp(1.0, Clamp(SafeDivide(Speed2D, Rate), Min, Max), Alpha)`
- **String literals**: quoted in call arguments — `GetCurveValueFromAnimation("MoveData_Speed", t)`
- **Unary NOT**: `Not_PreBool` -> `NOT variable`
- **Enum values**: Byte indices resolved to display names (handles BP enums with `NewEnumerator` names)
- **Property Access**: Reads property path from node title
- **Reroute nodes**: Followed through transparently (K2Node_Knot)

## What It Doesn't Capture (Yet)

- **Property Access deep paths** — shows the node-title path, not the full struct member chain
- **Montage notify tracks** — notify class/time/duration and their own properties
- **Node positions / layout** — text is structural, not spatial
- **Animation asset internals** — curves and notifies inside montages/sequences
- **Blend profiles** — referenced by name only

See [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md) for the full roadmap.

## Validated On

- **GASP (Game Animation Sample)** — ~700 lines of Blueprint function bodies captured, including thread-safe update functions, chooser references, complex transition rules
- **ALS Refactored** — 24 dump files (10 AnimBPs, 4 Blueprints). Empty function sections correctly reflect that all logic lives in C++
- **Custom projects** — AnimBPs and Blueprints with locomotion state machines, montage systems, and combat logic

## Architecture

The tool walks UE's native `UEdGraph` API — the same graph data the editor displays visually.

```
Console Command
  -> LoadObject<UAnimBlueprint> (or UBlueprint)
  -> GetAllGraphs() -> find UAnimationGraph
  -> Find UAnimGraphNode_Root
  -> WalkPoseChain() backward through pose link pins
      -> For each node: label + data pins
      -> If StateMachine: DumpStateMachine()
          -> EditorStateMachineGraph -> iterate states/transitions
          -> For each transition: BuildTransitionRuleExpression()
          -> For each state: GetBoundGraph() -> WalkPoseChain() (recursion)
  -> Write to file + print to log
```

### Key Design Decisions

- **Pose link detection via inheritance** — `FPoseLinkBase::StaticStruct()` + `IsChildOf()` catches all pose link types
- **Operator map with type suffix stripping** — `Greater_FloatFloat` -> `>` via map lookup + underscore stripping
- **Per-state visited sets** — prevents cross-state node skipping while still detecting cycles
- **`GetNodeTitle(ListView)` as universal fallback** — engine titles already include asset names, slot names, etc.
- **`AutogeneratedDefaultValue` comparison** — only shows pins with user-set values, not engine defaults

## Files

```
BlueprintDumpTool/
+-- BlueprintDumpTool.uplugin
+-- README.md
+-- IMPROVEMENT_PLAN.md
+-- Source/BlueprintDumpTool/
    +-- BlueprintDumpTool.Build.cs
    +-- Public/
    |   +-- BlueprintDumpToolModule.h
    |   +-- AnimBPDumper.h
    |   +-- BlueprintDumper.h
    |   +-- AssetDumper.h
    |   +-- BlueprintDumpUtils.h
    +-- Private/
        +-- BlueprintDumpToolModule.cpp
        +-- AnimBPDumper.cpp            (~1270 lines)
        +-- BlueprintDumper.cpp
        +-- AssetDumper.cpp             (~570 lines — non-BP types + batch dumping)
        +-- BlueprintDumpUtils.cpp      (~1810 lines)
```

### Module Dependencies

```
Public:  Core
Private: CoreUObject, Engine, UnrealEd, BlueprintGraph, AnimGraph, Kismet,
         AssetRegistry (batch dumping), AIModule (BehaviorTree/Blackboard)
```

`AnimGraphRuntime` is NOT needed — only editor-side `UAnimGraphNode_*` wrappers are used. `FPoseLinkBase` lives in the `Engine` module.

## Troubleshooting

### "Could not load AnimBP"
The path must be the **content browser path**, not a file system path:
```
DumpAnimBP /Game/Characters/Animation/ABP_MyCharacter       <-- correct
DumpAnimBP C:/Projects/Game/Content/ABP_MyCharacter.uasset  <-- wrong
```
The tool automatically tries appending the asset name as a suffix (e.g., `/Game/Path/ABP.ABP`) as a fallback. Plugin paths (`/All/`, `/Plugins/` prefixes) are normalized automatically.

### Empty output for a state
The state's bound graph has no `UAnimGraphNode_StateResult` node. This happens with newly created states that haven't been set up yet.

### Transition rule shows function name instead of expression
The operator isn't in the map. The tool falls back to printing the raw function name (e.g., `NearlyEqual_FloatFloat`). Add more operators to `ResolveOperatorSymbol()` if needed.

### Compile error: "no member named X"
UE5 occasionally moves properties behind getters across versions. If a property was renamed:
- `EditorStateMachineGraph` -> try `GetEditorStateMachineGraph()` or `GetEditorGraph()`
- `CacheName` -> try `GetCacheName()`
- `CrossfadeDuration` -> check `AnimStateTransitionNode.h` in your engine install

## Compatibility

- **Built for UE 5.7** but tested working on **UE 5.5.1** (ALS Refactored). UE shows a version warning, no actual problems.
- **Editor only** — the module is editor-only by design, zero runtime cost
- Works with any AnimBP or Blueprint in any project — takes asset path as parameter

## License

MIT License. See [LICENSE](LICENSE) for details.
