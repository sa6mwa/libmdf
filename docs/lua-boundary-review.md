# Lua boundary findings

The archived `stash/mdf` proposal was reviewed against the released 0.16.0 Lua
facade. Its portable lifetime fixes are independent of its withdrawn Vectis APIs.

## Confirmed defects

- Sink and trace argument allocation occurred before `lua_pcall`. A deterministic
  allocation failure skipped renderer/parser cleanup: the current baseline leaked
  12,704 native bytes in 14 blocks under Valgrind. Protect complete callback frames,
  return native errors, then rethrow the original Lua exception after cleanup.
- Copying materialized output could longjmp before freeing the native string or
  one-shot renderer. Protect only the Lua copy and unconditionally release native
  resources before propagating failure. A result copy can also run Lua finalizers;
  keep the retained renderer's operation guard active until its native string is
  freed, so a finalizer cannot close it mid-copy.
- One-shot registry references could outlive failed option validation or callback
  capture. Borrow callbacks from the active call stack instead; persistent
  callbacks belong directly to their userdata's uservalue table.
- Retaining the caller's mutable options table did not pin replaced custom font
  strings. Snapshot declared configuration containers and retain immutable string
  values without duplicating their byte payloads. Preserve getter-produced token
  text through native writes for the same reason.
- Implicit number-to-string conversion and embedded NULs were unsafe for borrowed
  C strings. Validate their string type and NUL-free contract. Length-aware byte
  fields continue to accept binary strings.

Regression coverage injects Lua allocation failures into source/sink/trace calls,
callback argument creation and materialized result copying, for one-shot handles,
retained handles and document streams. It checks recovery/close, callback-graph
collection, option mutation, original error identity, coroutine callback lifetimes,
legal reader geometry changes, and identical sink/trace decision sequences under
both table modes. The same cases run under Valgrind without suppressions.

## Surface decisions

Keep `render`, `render_stream` and the existing handle/document methods. `render`
already documents materialized results; renaming it adds no capability. Protecting
the callback boundary requires no native ABI change or new public Lua API.

Do not import the Vectis buffer bridge, typed allocator callbacks, ownership
transfers or mutable emission views. Lua cannot safely own arbitrary native
addresses; a future buffer interface needs a real consumer, explicit leases and
measured FFI costs. The native C allocator/emission interfaces remain available
to embedders. No new per-emission output staging or aggregation is introduced.

Font reader callbacks, built-in font views, additional dump/path helpers, explicit
per-call sink variants and source-pager bindings need a concrete workflow before
expanding the facade. Existing font byte strings and streaming reader/writer
callbacks cover the current supported workflows. Missing mappings are not by
themselves defects.

Do not impose the adaptation's arbitrary layout workspace cap or add parallel
buffered APIs. Source-to-sink streaming does not promise constant layout memory:
full tables and HTML/deck layout retain their documented decision state. Any
stronger bound requires a separate behavioral contract and native proof.
