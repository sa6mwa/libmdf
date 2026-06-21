# AGENTS.md

You are collaborating with a highly opinionated Go architect. Optimize for Go-idiomatic design, separation of concerns, correctness, and developer experience (DX). Speed and volume are secondary.

## Context

`mdf` is a streaming Markdown → ANSI renderer with a growing AST and live output. The core invariants:

1. **Streaming is primary:** parse incrementally from an `io.Reader` and render as data arrives.
2. **No full buffering:** never `io.ReadAll`/buffer the entire payload to render.
3. **AST grows live:** the document is usable while it is still being built.
4. **Render to an ANSI token stream** (text + style), then wrap/reflow as the **final step per emitted segment** (no retroactive reflow).
5. **Width only enters at the last step** (no wrap logic in the AST renderer).
6. **Zero/near-zero alloc** in hot paths, especially repeated renders.
7. **Parity target:** the streaming renderer must be byte‑for‑byte identical to the goldmark-based renderer for `testdata/mdtest/TEST.md`.
8. **Sample data:** use `testdata/mdtest/TEST.md` for tests/demos.

Suggested stack (temporary for parity):

* github.com/yuin/goldmark + extension.GFM (parity reference only; streaming is the long-term path)
* github.com/muesli/reflow/wordwrap + github.com/muesli/reflow/ansi

## Workflow (mandatory)

1. Restate the goal and constraints in your own words.
2. Propose 1–3 designs with explicit tradeoffs (complexity, maintainability, DX, performance, risk).
3. Get alignment before writing significant code.
4. **Prove correctness via tests (see Quality & Verification gates).**
5. If asked to commit, use Conventional Commits.

## Refactors (strong preference)

* Avoid feature flags for refactoring tasks.
* Prefer clean refactors with a clear cutover:

  * No lingering legacy implementations, structs, or parallel code paths.
  * Remove dead code and migrate call sites in the same change set or sequence.
* Only keep parallel implementations or legacy structures if explicitly requested.

## Architecture & packaging

* Separation starts at the package boundary:

  * Public API packages for exported surfaces.
  * `internal/...` for non-exported implementation details.
* If there are two or more variants or adapters of an implementation: **use an interface**.
* Constructors:

  * Provide a `New...` constructor for implementations.
  * **Constructors must return the interface type, never a concrete type.**
* Cyclic imports:

  * If cycles occur, extract core functionality into a `core` package or subpackage so both
    main/module code and subpackages can import it without cycles.

## Public API shape (strong preference)

* Inputs:

  * If a user-facing function or interface method takes more than 4 parameters total
    (including `context.Context`), move non-`ctx` inputs into a request struct
    (e.g. `FooRequest`).
* Outputs:

  * A user-facing function or interface method must return **no more than two values**:
    `(T, error)` or `(Response, error)`.
  * If multiple outputs are required, return a response/result struct as the first value.

## Documentation & generators

* Every package must have a `doc.go` with standard Go package documentation.
* Code generation:

  * If generators are not tightly bound to a single package, place `generate.go` at the
    module root.
  * If tightly bound to a package, place `generate.go` in that package and any generator
    `main` packages underneath as appropriate.

## Quality & verification gates (primary)

* **Assume minimal or no human code review.**
* **Verification is the primary quality gate.**
* Every new feature, behavior, fix, or refactor **must** be proven with comprehensive
  unit tests and/or integration tests.
* Prefer tests that assert **observable behavior** over implementation details.
* Treat tests as executable specification; code exists to satisfy them.
* Optimize for self-verifying systems where correctness is falsifiable by tests,
  not inferred by reading code.
* Code review is secondary and optional: useful for architecture, pedagogy,
  and high-level critique—not as the main correctness mechanism.

### Mandatory checks before “done”

* `go test ./...`
* `go vet ./...`
* `golint ./...`
* `golangci-lint run ./...`

## Commits

* All git commit messages **must** follow the Conventional Commits specification
  (`type(scope): summary`).
* If you are asked to commit, choose the narrowest correct type and keep messages factual
  and outcome-oriented.

## Repo hygiene

* If `.golangci.yml` does not exist in the repo root, create and seed it with:

```yaml
version: "2"
linters:
  disable:
    - errcheck
  exclusions:
    rules:
      # staticcheck style nits we don't want to chase
      - linters: [staticcheck]
        text: "QF1003"
      - linters: [staticcheck]
        text: "S1017"
      - linters: [staticcheck]
        text: "QF1001"
      - linters: [staticcheck]
        text: "S1009"
```
