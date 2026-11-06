# Visual scripting (Blueprints)

Blueprints are node graphs (`.ugraph`, JSON, file version 2; version 1 loads) attached to entities by
a `ScriptComponent{graph, variables}`. The code lives in `engine/include/Engine/Script/` and
`engine/src/Script/`; the editor is `ScriptGraphEditor`.

## Types and values (`ScriptValue.h`)

- `PinType` = {kind Exec/Bool/Int/Float/Vec3/String/Entity/Enum/Struct, container None/Array/Map,
  key kind, interned names, `inner` = interned element/value type}. Containers nest: arrays of arrays
  or maps, maps with container values (`ElementType` / `ArrayOf` are recursive). Map keys: Bool, Int,
  Float, String, Vec3 (lexicographic), Entity, Enum.
- Type strings: `int`, `intArray`, `enum:Color`, `struct:Item[]`, `map:<key>:<value>`,
  `intArray[]`, `array:map:string:int`, `map:int:map:string:float`; `DisplayName` gives `int[][]`,
  `Map<string, int[]>`.
- `ScriptValue` is a variant incl. `ScriptArrayPtr`, `ScriptStructPtr`, `ScriptMapPtr` (shared,
  **copy-on-write**); enum values are ints. Implicit conversions: Int ↔ Float, Bool → number, anything →
  String, Enum → name. `ValuesEqual` is deep. JSON: arrays as lists, structs as objects, maps as pair
  lists, entities null (or `{"$uuid": n}` in save games).
- `ScriptRegistry` (process-wide, main thread): enums (`.uenum`), structs (`.ustruct`, two passes),
  interfaces (`.uinterface`) and libraries (`.ugraph` with `"library": true`, name = file stem);
  `LoadDirectory` (Content folder), `Validate`, `LibraryDiagnostics`, `Revision`.

## Graph model (`ScriptGraph.h`)

- Nodes `{id, type, position, param, defaults, function}` (empty `function` = event graph), links by
  pin name (one link per exec output and per data input), comments, variables (`exposed` = instance
  editable), functions, macros, custom events with parameters, dispatchers, timelines, interfaces,
  breakpoints with options, `parent` + `defaults` (inheritance).
- Editing API: Connect/Disconnect/RemoveDanglingLinks, RenameVariable/SetVariableType,
  Add/Rename/RemoveFunction, SetFunctionPure, FunctionSignatureChanged, OverrideFunction, Collapse.
- `ValidateScriptGraph`: unknown types/variables/keys, type errors, multiple links, pure cycles, scope
  rules (exactly one entry per function, no events in functions, no latent nodes in pure functions,
  no repeating nodes — Timeline, Tween Float — in functions), purity of calls, inheritance rules,
  dispatcher signatures, empty-event warnings. `ValidateExpandedScriptGraph` checks the compiled form
  and maps errors back to their origin.

## Nodes (`ScriptNodes.h`)

More than 300 node types in categories: events (BeginPlay, Tick, EndPlay, collisions, hit, keys, mouse,
input actions, custom, level loaded/unloaded, anim notify, UI, migrate save game, construction),
flow (Branch, Sequence, ForLoop, While, ForEach, DoOnce, FlipFlop, Gate, MultiGate, Delay,
Retriggerable Delay, Switch on Int/String/Enum, Select, Call Event), variables, math (float, int,
vector, comparisons, logic, interpolation, easing), strings (format, split, join, parse, …), arrays,
maps, structs, enums, entity/transform, tags, camera, input, timers, physics, joints, ragdolls, audio,
particles, AI/navigation, animation, rendering (materials, decals), terrain, UI, save game, options,
level streaming, prefab/primitive spawning, async model loading, interfaces, dispatchers, timelines,
tweens and class nodes. `NodeDesc` describes pins, parameter kind (`ParamKind`), purity, latency,
inference (`ParamInference`: reroutes take the connected type, array/map nodes their element type) and
`repeats` (outputs fire repeatedly). Unconnected entity pins mean *Self*.

`ScriptContext` is the interface the nodes use to reach the engine (scene, physics, audio,
particles, navigation, animation, streaming, assets, IsA, Construct, …).

## Compilation

1. `ResolveScriptParent` loads the parent chain (depth ≤ 16; cycles, missing and library parents are
   errors).
2. `FlattenScriptInheritance` (`ScriptInherit.cpp`) merges ancestors: their nodes get IDs after the
   child's and an origin (debugger key of the parent file); overridden parent functions become
   `Super<n>:F` (calls stay virtual and reach the child's version); overridden event handlers become
   custom events `Super<n>:<event key>`; Call Parent nodes become calls of the renamed versions.
3. Macro and library expansion (`ScriptExpand.cpp`): library functions are copied in as `Lib.F`,
   macro uses are replaced by copies (tunnel rewiring, conversions kept through reroutes, nested up to
   32 rounds), macro definitions removed. `ScriptOrigin` (file, node, function, outermost macro use)
   maps every compiled node back to its source for highlighting, timings, breakpoints and errors.
4. The VM compiles pins to indices (`Program`, `CompiledNode`, `CompiledFunction`).

## VM (`ScriptSystem.h`)

- One instance per entity with a `ScriptComponent`; instance-variable overrides
  (`ScriptVariableOverride{value, entityUuid}`) are applied on instantiation.
- Chains run with a continuation stack (loops, sequences). **Latent** nodes park the chain; the rest
  of the event continues (like UE). Update order per frame: delays → timers → ticking nodes →
  collisions → input → Tick. `maxStepsPerEvent` aborts endless loops. Runtime errors are reported once
  per node.
- **Pure caching:** each impure execution has a stamp; a pure node is evaluated once per stamp
  (Random stays consistent within a node's inputs).
- **Functions:** impure calls push a `CallFrame` (locals reset per call); pure calls evaluate
  synchronously. **Recursion** is allowed: re-entering an active function saves outputs/stamps/locals
  of its nodes (`CallFrame::saved`); depth limit `kMaxCallDepth` = 256 ("Calls nested too deep").
- **Latent nodes in functions** (Delay, Retriggerable Delay, AI Move To, Load/Unload Stream Level,
  Move/Rotate/Scale To, Async Load Model): the call stack is **parked** (`Parked{stack, calls,
  scopes}`, at the `Waiting` entry or in `parkedTicks`) and restored when the node resumes; the caller
  continues after the return. Latent node state is per call. Interface calls do not wait: the caller
  gets the outputs set so far and the rest runs as its own chain.
- **Interfaces and dispatchers:** `CallFunctionOn` runs the target instance's function nested;
  Interface Call has an `Implemented` output. Dispatcher bindings are per instance (dead ones pruned),
  arguments converted positionally; `DispatcherSignatureMismatch` is checked when validating
  self-binds and on every runtime Bind.
- **Inheritance at runtime:** `Program::classes` (own key + ancestors) drives `IsA`, Cast To,
  `InstancesOf` and `Watch` through parent files; breakpoints work in parent files.
- **Timelines/tweens:** `ScriptTimeline{length, loop, autoPlay, tracks Float/Vector/Event with keys
  Linear/Constant/Smooth}`; ticking nodes via `SetTicking` → executed once per update; tweens in world
  space (scale local), 14 easing curves.
- **Input:** `InputMap` actions/axes from the project; Pressed fires only when no other bound key was
  already held.
- **Save games:** slots `<dir>/<slot>.sav` (JSON `{version, gameVersion, values}`), in-memory cache
  across level changes, entities by UUID. `SetSaveVersion`: older slots fire `Event.MigrateSaveGame`
  synchronously, newer slots are refused (not read, not overwritten). Slot Version / Game Version
  nodes.
- **Levels:** Open Level / Quit through `TakeLevelRequest()`; Load/Unload Stream Level (latent),
  Is Level Loaded, level events; `EndPlayFor` when a level unloads.
- `Provide(file, graph)` runs unsaved editor versions (drops all programs, child classes included).
  `Debug(file)` gives node/link timings and diagnostics, `Messages()` the Print output. Entities
  spawned by scripts are released at `End`.

## Debugger

Breakpoints are stored in the graph and set live (`SetBreakpoints / SetBreakpointList`). A chain halts
*before* the node and the whole VM pauses (new chains queue up). `DebugContinue`, `DebugStep` (into,
over, out — by chain ID and call depth), `PausedAt()` (incl. callers), `Watch(file, entity)`
(variables, locals of the paused function, last value of every data output), `InstancesOf`.
Breakpoint options: condition over variables/locals (`ScriptCondition`: `|| && !`, comparisons,
`+ - * / %`, literals) and hit count; a broken condition halts and reports. A breakpoint on a macro
node halts when the copy is entered. Not available in pure functions, interface calls and
construction scripts.

## Construction scripts

`Event.Construction` runs on a temporary instance (`RunConstruction / RunAllConstruction`; before
BeginPlay; in the editor after every history or graph change, open and stop, throttled to 4 runs/s
while a widget is active). Entities it creates get `ConstructionOwned{owner}` and are destroyed on the
next run or when the owner disappears; they are kept in memory snapshots but skipped in scene files.
Owner reset like UE: per owner `{before, after}` entity-state snapshots; before each run `before` is
restored and the JSON diff `after → current` is re-applied (user edits survive). `ResetConstructed`
runs before saving. Latent nodes, timers and ticks are runtime errors there. Prefabs spawned at
runtime are constructed too.

## Inheritance rules

- `parent` (relative to the graph file) and `defaults` (overridden defaults of inherited variables).
  Const lookups (`FindVariable/Function/Macro/Event/Dispatcher/Timeline`, `HasScope`) fall back to the
  parent chain; non-const lookups are own-only.
- A function with an inherited name is an override (same signature and purity). Event nodes with the
  same key (`ScriptEventKey` = type[:param]) replace the parent's handlers. Own variables, events,
  dispatchers, timelines and macros must not repeat inherited names; defaults only for inherited
  variables. Interfaces are inherited.
- Nodes: `Function.CallParent` / `CallParentPure` (inside the overriding scope),
  `Flow.CallParentEvent` (only when the event is overridden and an ancestor handles it),
  `Class.IsA` (pure) and `Class.CastTo` (Success/Failed, As) with a class graph parameter.

## Editor (`ScriptGraphEditor`)

- "Blueprint" window (tab next to the viewport): tabs per graph, undo/redo (JSON snapshots, widget
  sessions), zoom/pan/box select/snap, exec and data pins (arrays square), Bezier links, drag a link to
  empty space → context-sensitive search with auto-connect, Alt-click disconnects, inline values,
  comments with color presets, reroutes (double-click a link), minimap (M), align/distribute
  (Shift+W/A/S/D), copy/paste/duplicate/delete, frame, execution highlight while playing.
- Side bar: Class (parent class with drop/browse/clear, inherited variables with defaults, inherited
  functions with Override/Call), Graphs (event graph + functions with inputs/outputs/locals/pure),
  Macros, Custom Events, Dispatchers, Interfaces, Timelines (curve editor window), Variables (all types
  incl. nested containers up to 4 levels, instance editable), Details (parameters per `ParamKind`),
  compiler results (click focuses), Debug (instance, variables, locals, call stack).
- Find in Blueprints (Ctrl+F; open graphs + project `.ugraph`, reference mode), Find references,
  Collapse to function/macro, Blueprint types window (enums/structs/interfaces, libraries).
- Debugger keys: F9 breakpoint, F5 continue, F10 over, F11 into, Shift+F11 out.
- Refactoring: renaming a variable/function updates child classes (open graphs as an undo step,
  project files rewritten); *Move to parent class* for variables and functions (only when valid in the
  parent; overrides and name clashes refused). The content browser repairs `parent` and Is A / Cast To
  class paths on move; *Create child blueprint* creates `<Name>_Child.ugraph`.
- UI automation for tests: `Editor::SimulateMouse / SimulateKey / SimulateText / FocusViewport`,
  `ScriptGraphEditor::CanvasRect / TimelineCurveRect`.

## Examples

`assets/scripts/{Rotator, RainOnSpace}.ugraph`, template scripts (`Rotator`, `ImpactSound`,
`RainOnSpace`, `Wander`).
