# Architecture

OpenStrike is organized around explicit ownership boundaries instead of Source-era global subsystems.

## Runtime

`openstrike::Engine` owns the host frame loop. It configures engine services, executes queued console commands, advances deterministic fixed simulation ticks through `FixedStepAccumulator`, then renders once through an `IRenderer`.

Modules plug into lifecycle points and receive `EngineContext` so they can use shared engine services without globals:

- `on_start(RuntimeConfig&, EngineContext&)`
- `on_fixed_update(SimulationStep&, EngineContext&)`
- `on_frame(FrameContext&, EngineContext&)`
- `on_stop(EngineContext&)`

That gives client, server, game, editor, and future tools the same lifecycle without static registration macros.

## Engine Services

`EngineContext` is the porting target for Source-style core services:

- `CommandBuffer` mirrors the useful part of `Cbuf_AddText` / `Cbuf_Execute`: semicolon/newline separated commands, startup `+commands`, and `exec` files.
- `CommandRegistry` owns built-in commands such as `quit`, `echo`, `set`, `cvarlist`, `cmdlist`, `path`, and `exec`.
- `ConsoleVariables` provides a typed, testable replacement for early `ConVar` use.
- `ContentFileSystem` provides path-ID search paths like `GAME`, `MOD`, and `PLATFORM` without importing the legacy filesystem.

The default mount set uses the configured content root, `content_root/csgo`, and `content_root/platform`, which keeps `assets/ui/...` and `resource/ui/...` style paths both viable.

## Modes

`RuntimeConfig::from_command_line()` selects one of three first-class modes:

- `Client`: local game runtime.
- `DedicatedServer`: server-only simulation.
- `Editor`: editor shell using the same simulation and renderer contracts.

The mode is deliberately data, not preprocessor state.

## Simulation

The first gameplay slice is small but real: `GameSimulation` owns a player state and advances it through Source-style acceleration and friction in `game/movement.*`. This is a stable place to migrate player prediction, weapon state, and network snapshots.

## Rendering

`IRenderer` is the rendering seam. Client and editor modes use the custom D3D12 backend on Windows by default and a native Metal backend on macOS. Linux builds compile the same runtime spine but currently fall back to `NullRenderer` until a native backend is added. `NullRenderer` also remains for dedicated server mode, tests, and explicit `--renderer=null` runs. Renderer implementations must not own game simulation.

## Migration Rules

1. Preserve behavior, not file layout.
2. Keep command/cvar/filesystem access in `EngineContext`; avoid recreating Source-era globals.
3. Keep deterministic state in shared game code, then expose it to client/server/editor.
4. Add tests around fixed tick, movement, serialization, and content conversion as features migrate.
5. Platform-specific code lives behind interfaces.
