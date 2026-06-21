# Code Review Is a Dead End

[An article published in Computer Sweden](https://computersweden.se/article/4121340/utvecklare-litar-fortfarande-inte-pa-ai-genererad-kod.html) argues that developers still do not trust AI-generated code—and that this distrust is rational because AI code is harder to review, more verbose, and shifts the bottleneck from writing to inspection. The conclusion is familiar, comfortable, and wrong in an important way.

The article correctly observes a symptom, but it misidentifies the disease. The problem is not that AI generates code we cannot trust. The problem is that we are clinging to code review as the primary mechanism of trust in a world where code production has become effectively free.

Code review was never about quality in the abstract. It was about risk management under scarcity. When humans typed every line, reading diffs was a reasonable proxy for understanding intent, correctness, and system impact. That premise no longer holds. AI systems can generate thousands of lines of plausible code in minutes. Asking humans to line-by-line review that output is not quality assurance; it is ritualized futility.

This is where much of the current discourse subtly [*talks down the machine*](https://pkt.systems/tdtm.pdf). AI is framed as a fast but careless junior developer whose output must be patiently supervised by wiser seniors. That framing is psychologically comforting—it preserves status—but operationally disastrous. **If your process assumes that humans must read most of what machines produce, you have already lost the scaling game**.

The article quotes concerns about verbosity, hidden defects, and "needle-in-a-haystack" bugs, echoed by vendors such as [Sonar](https://www.sonarsource.com/the-state-of-code/developer-survey-report/). These concerns are real. But they are not solved by better reviews. **They are solved by making review largely irrelevant**.

The alternative is not blind trust. It is a shift from outer-loop trust to inner-loop verification.

Instead of asking humans to reason about code by reading it, we should force code (human or AI-generated) to justify itself through executable evidence. That means aggressive use of unit tests, integration tests, property-based tests, invariants, and simulation. It means systems that are self-verifying by construction. If behavior matters, behavior must be specified and exercised, not inferred from a diff.

This approach changes the economics completely. Time spent writing and improving integration tests scales far better than time spent reviewing code. Tests amortize. Reviews do not. A good integration test suite becomes a permanent asset that accelerates iteration and constrains future changes. A code review is a one-off human synchronization cost that must be paid again and again.

Crucially, AI agents are far better suited to this world than to traditional review. Agents can reason about intent, derive edge cases, generate adversarial inputs, and continuously improve test coverage. They can analyze failures, suggest stronger invariants, and refactor tests as the system evolves. In other words, they can help move trust from social process to technical proof.

Several interviewees in the article note that AI code sometimes ignores architectural conventions or broader system context. That is not an argument for more review; it is an argument for better constraints. Architecture that exists only in human heads and style guides is not architecture—it is folklore. Architecture that is enforced by interfaces, contracts, tests, and runtime checks is executable and legible to both humans and machines.

One quote in the article comes close to the truth. A CTO at [DBT Labs](https://www.getdbt.com/) notes that trust depends on whether code is produced "through a process with high integrity." Exactly. But high-integrity processes in an AI-augmented world do not look like expanded code review checklists. They look like tight inner loops where intent, constraints, and verification live close together and execute continuously.

Code review will not disappear entirely. It will remain useful for pedagogy, architectural discussion, and high-level design critique. But as a primary quality gate, it is a dead end. **AI did not make code less trustworthy; it made reading code the wrong abstraction**.

If developers redirected even a fraction of their review time into writing stronger integration tests and designing self-verifying systems, two things would happen immediately. Quality would improve, because behavior would be specified and exercised rather than assumed. And iteration speed would increase, because the review bottleneck would evaporate.

## Conclusion

The uncomfortable conclusion is this: the trust crisis around AI-generated code is largely self-inflicted. We are trying to preserve a human-centric control mechanism in a machine-scaled environment. That mismatch is the real risk. The way forward is not to review more, but to verify better—and to let machines help us do exactly that.

## Examples: what the inner loop looks like in practice

To make this concrete, here are three real codebases built with an AI-augmented inner loop where **verification—not review—carries the trust load**.

### Example 1: *lockd* — verification as a first-class artifact

*lockd* is a distributed coordination service (not yet released). Its development assumes that humans do not scale as primary readers of code, but they *do* scale as designers of constraints and verification.

```
{
  "loc": 144649,
  "test_loc": 53663,
  "code_loc": 90986,
  "percent_test_loc": 37.1,
  "percent_code_loc": 62.9,
  "languages": {
    "go": {
      "loc": 144649,
      "test_loc": 53663,
      "code_loc": 90986,
      "percent_test_loc": 37.1,
      "percent_code_loc": 62.9,
      "test_count": 692,
      "example_count": 4,
      "benchmark_count": 35,
      "fuzz_count": 3
    }
  }
}
```

**Codebase composition:**

* ~145k total lines of code
* ~91k LOC of production code
* ~54k LOC of tests
* **≈37% of the entire codebase exists solely to verify the other 63%**

In practical terms:

* ~700 unit + integration tests
* Dozens of benchmarks
* Fuzz tests for concurrency and edge cases
* Tests written *alongside* code generation, not after review

This is not "extra testing for safety." It is the **core development loop**. The coding agent is expected to:

1. Propose implementations
2. Propose or update tests that falsify those implementations
3. Iterate until the tests—and the system invariants—hold

Human effort is spent on *what must be true*, not on line-by-line inspection of how it is achieved.

### Example 2: *lingon* — speed beyond reviewability

*pkt.systems/lingon* (not released yet, see [github](https://github.com/sa6mwa/lingon)) is a terminal multiplexer and relay system with:

* a Linux host component
* a server component (the WS relay)
* a TUI client
* a web UI (xterm.js)
* a native Android app

It was produced in **~10 days** across multiple languages. Traditional code review simply does not fit this cadence.

```
{
  "loc": 50510,
  "test_loc": 15134,
  "code_loc": 35376,
  "percent_test_loc": 29.96,
  "percent_code_loc": 70.04,
  "languages": {
    "go": {
      "loc": 38957,
      "test_loc": 13703,
      "code_loc": 25254,
      "percent_test_loc": 35.17,
      "percent_code_loc": 64.83,
      "test_count": 199,
      "example_count": 0,
      "benchmark_count": 0,
      "fuzz_count": 0
    },
    "javascript": {
      "loc": 2913,
      "test_loc": 0,
      "code_loc": 2913,
      "percent_test_loc": 0,
      "percent_code_loc": 100,
      "test_count": 0
    },
    "kotlin": {
      "loc": 8065,
      "test_loc": 1431,
      "code_loc": 6634,
      "percent_test_loc": 17.74,
      "percent_code_loc": 82.26,
      "test_count": 22
    },
    "shell": {
      "loc": 575,
      "test_loc": 0,
      "code_loc": 575,
      "percent_test_loc": 0,
      "percent_code_loc": 100
    }
  }
}
```

**Codebase composition:**

* ~50k total LOC
* ~15k LOC of tests
* **≈30% test code overall**
* **≈35% test code in Go**, the core system language

Notably:

* The system is *still under-tested* at the end-to-end level
* Bugs that remain are **not review failures**—they are **missing-test failures**
* The obvious next step is *more tests*, not more review

Trying to "review" this codebase at generation speed would either halt progress or degrade into rubber-stamping. Verification scales; review does not.

### Example 3: Centaurx

For a released system, consider [Centaurx](https://github.com/sa6mwa/centaurx). `pkt.systems/centaurx` is a Codex CLI development environment that lets developers move fluidly between an SSH TUI, a web UI, and a native Android client.

The system spans multiple runtimes and interfaces, yet its quality strategy follows the same inner-loop pattern as the unreleased examples above.

```
{
  "loc": 45516,
  "test_loc": 12393,
  "code_loc": 33123,
  "percent_test_loc": 27.23,
  "percent_code_loc": 72.77,
  "languages": {
    "go": {
      "loc": 38374,
      "test_loc": 11607,
      "code_loc": 26767,
      "percent_test_loc": 30.25,
      "percent_code_loc": 69.75,
      "test_count": 225,
      "example_count": 0,
      "benchmark_count": 0,
      "fuzz_count": 0
    },
    "javascript": {
      "loc": 1584,
      "test_loc": 42,
      "code_loc": 1542,
      "percent_test_loc": 2.65,
      "percent_code_loc": 97.35,
      "test_count": 2
    },
    "kotlin": {
      "loc": 4746,
      "test_loc": 744,
      "code_loc": 4002,
      "percent_test_loc": 15.68,
      "percent_code_loc": 84.32,
      "test_count": 8
    },
    "shell": {
      "loc": 812,
      "test_loc": 0,
      "code_loc": 812,
      "percent_test_loc": 0,
      "percent_code_loc": 100
    }
  }
}
```

**Codebase composition:**

* ~45k total lines of code
* ~33k LOC of production code
* ~12k LOC of test code
* **≈27% of the entire codebase is dedicated purely to verification**

In the core Go codebase—the part most sensitive to correctness and concurrency—**just over 30% of all lines are tests**, spread across more than 200 test cases. The remaining languages (JavaScript, Kotlin, shell) follow the same principle, though with uneven coverage reflecting where risk and complexity actually sit.

What matters here is not the exact percentage, but the development posture it reflects:
tests are not a compliance artifact or something added "after review." They are the mechanism by which rapid iteration remains safe when code is produced faster than humans can reasonably review it.

Centaurx is not an outlier. It is simply a system built under the assumption that **trust must be earned by executable evidence**, not by manual inspection of ever-growing diffs.

### The agent contract (why this works)

These projects rely on explicit **agent instructions** (via an `AGENTS.md`) that define the rules of engagement:

* The agent is not trusted to be correct
* It *is* trusted to explore, refactor, and generate
* Every non-trivial change must come with verification
* Tests are part of the definition of "done," not a follow-up task

This turns AI from a risky junior developer into a **high-throughput implementation engine constrained by executable proof**.

### The takeaway

When ~30–40% of your codebase exists purely to verify the rest, something fundamental has shifted:

* Trust no longer flows through social process (review)
* Trust flows through **repeatable, automated evidence**
* Speed increases *because* quality is mechanized, not despite it

This is the inner loop that replaces code review as the primary quality gate. It is not softer. It is stricter—just finally scalable.
