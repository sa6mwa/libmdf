# libmdf Agent Instructions

## Streaming Is A Hot-Path Contract

Streaming in libmdf means producer-to-consumer streaming. It does not mean
buffering content internally and later replaying it in streaming-shaped chunks.

For ANSI output, the core invariant is:

```text
one parser/renderer decision buffer -> one emission -> one sink write
```

The emission trace is not a separate truth source. It must describe the exact
sink write that just happened:

```text
trace event count == sink write count
trace event bytes == sink write bytes
trace event order == sink write order
```

If a trace shows word-sized emissions while the sink receives a whole line, the
trace is lying and the implementation is wrong.

## Buffering Rules

Only buffer what is required to make the next rendering decision.

Allowed decision buffers:

* A current word or styled word while waiting for the boundary that proves it can
  be emitted.
* The minimum inline delimiter/link/code/entity state needed to decide the
  syntax.
* Table buffers required by the configured table mode:
  * full table mode may buffer the table before rendering,
  * row table mode may buffer only what is required to emit header plus first row
    and then subsequent rows.

Disallowed buffering:

* A second renderer/output buffer after a decision is already made.
* Coalescing two or more decided words into one sink write.
* Emitting word-sized trace events while writing larger chunks to the sink.
* Direct ANSI writes that bypass the decision emission path.
* Post-processing a completed line or paragraph into fake streaming chunks.

## Emission Buffer Ownership

Emission composition must use the single per-instance emission buffer interface.
Do not add local stack buffers, local heap allocations, or per-helper scratch
arrays to assemble output bytes that are about to be emitted.

The default emission buffer policy is:

* allocate the per-instance emission buffer through the configured allocator,
* initial capacity is 128 bytes,
* max capacity is 8192 bytes when the configured max is 0,
* grow only as needed,
* keep the high-water allocation for the instance lifetime,
* release owned storage on instance destroy.

Developer supplied emission buffers must be explicit:

* fixed supplied buffers are hard bounded and must not grow,
* owned supplied buffers may grow through the configured allocator up to max,
* non-owned supplied buffers are treated as bounded unless ownership is
  explicitly transferred,
* a single decision emission that exceeds the configured max must fail instead
  of silently bypassing the buffer.

## Emission Surface

ANSI renderer code must route real output through the decision emission surface.
Do not add new direct `mdf_write_*` calls in ANSI hot paths. A direct sink write
is only acceptable in code that is explicitly not part of ANSI streaming output,
or when the write is the emission surface itself.

The preferred shape is:

```text
decide -> emit exact bytes once -> update visible state
```

Do not split this into:

```text
decide -> append to renderer buffer -> later flush -> trace something else
```

## Tests Must Prove The Contract

Every streaming-sensitive change must include a test that compares real sink
writes and emission trace events from the same render. Final rendered output is
not enough. Trace-only tests are not enough.

Required observable assertions:

* The sink write sequence and trace sequence are identical one-to-one.
* Headings such as `# The Outcome-Based Agile Framework` emit the marker and
  each word as separate decided emissions.
* A regression where the sink receives `The Outcome-Based Agile Framework` as
  one write must fail immediately.
* Tables are tested under their configured buffering mode instead of the normal
  word-streaming invariant.

## Parity Strategy

Go `mdf` is the behavioral reference for ANSI streaming. Prefer comparing the C
hot path against Go's single pending decision buffer model rather than
accumulating compatibility shims.

When parity and local inferred behavior disagree, parity wins unless the user
explicitly changes the contract.
