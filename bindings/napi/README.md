# bindings/napi (future)

Native Node addon reusing `aud::core` directly (no WASM), per the vision of engine reuse in native
desktop/server contexts. Not started — no milestone currently schedules it. When it lands, it should
need zero changes to `engine/`, which is the whole point of keeping the engine platform-independent
(see `documentation/tasks/M00-foundations-and-conventions.md` §4).
