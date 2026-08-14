# M9 — Capstone, System Design & Behavioral Preparation

> **Why this matters for this role.** By this point you can answer questions. This
> module is about the two rounds that decide the offer: the **design round**, where
> you are handed an open-ended problem and watched while you think, and the
> **behavioral round**, which for a team whose entire job is serving demanding
> internal customers is weighted far more heavily than candidates expect.
>
> Nobody fails a design round for not knowing an API. They fail it for designing
> before asking, for never mentioning testing, and for producing something nobody
> could extend.

---

## Part 1 — The design round

### 1.1 The method

A design round is 45 minutes. Budget it deliberately:

```
 0-8 min   CLARIFY      Ask questions. Do not draw anything yet.
 8-12 min  SCOPE        State requirements + non-requirements + success criteria.
12-30 min  DESIGN       Layers, interfaces, data model. Draw it. Think aloud.
30-38 min  TRADEOFFS    Alternatives considered and why rejected. Failure modes.
38-45 min  TEST & OPS   How it is tested, debugged, versioned, and rolled out.
```

The single most common failure is spending minute 1 through 40 in the DESIGN box.
The interviewer is not primarily evaluating your architecture; they are evaluating
whether you are someone who *asks before building*, which pre-silicon work punishes
you for skipping.

### 1.2 The clarifying questions that always apply

Memorise this list. It works on every design prompt in this domain:

1. **Who are the users?** Architects, verification engineers, driver developers — and
   how many? Ten people or five hundred? That single answer changes how much API
   stability matters.
2. **What is the lifetime?** A throwaway experiment or a decade-long framework?
3. **What changes most often?** The ISA? The architecture? The test content? Whatever
   changes most must be the cheapest thing to change — usually meaning it becomes *data*,
   not code.
4. **What is the execution target?** Silicon, emulator, RTL simulation, functional model?
   That sets the performance budget by six orders of magnitude.
5. **Does it need determinism?** (In this domain: yes. Ask anyway, then say why it matters.)
6. **What is the scale?** Ten tests or ten million? Kilobyte traces or terabyte traces?
7. **What does failure look like?** Who sees it, how do they reproduce it, how do they
   tell whether the bug is in the hardware, the model, or the tool?
8. **What is explicitly out of scope?** Getting the interviewer to say "don't worry
   about X" is free time.

### 1.3 The principles to state out loud

Interviewers score on whether you *articulate* principles, not just apply them:

- **Separate what from how.** Architecture-neutral description; architecture-specific
  back ends.
- **Make the frequently-changing thing data.** Table-driven encoding means an ISA
  revision is a table edit reviewed by an architect, not a code change reviewed by you.
- **One place to change.** For any anticipated change, be able to name the single file.
- **Open for extension, closed for modification.** New opcode/engine/architecture =
  registering a plugin, not editing a switch.
- **Determinism is a feature with an owner.** Not an emergent property.
- **Debuggability is a feature.** Dry-run modes, introspection, good errors, minimisation.
- **Design the failure path first.** In verification tooling, the failure path *is* the
  main path — the whole point is finding failures.

---

## Part 2 — The capstone system design, fully worked

**Prompt: "Design a framework for generating GPU workloads to verify a new architecture."**

This is the archetypal question for this exact job. Here is a complete answer.

### Step 1 — Clarify (said aloud)

> "Before I design anything, let me make sure I understand the problem. Who writes
> the tests — verification engineers, or architects who want a quick experiment? What
> executes them: RTL simulation, emulation, or silicon? Is this one architecture or
> must it span several concurrently, since pre-silicon usually means the previous
> architecture is still being debugged while the next is being defined? Roughly how
> many tests, and how long may one take? And do we have a golden reference model to
> check against, or must the tests be self-checking?"

Assume the answers: verification engineers *and* architects; targets RTL sim, emulation
and eventually silicon; must support two architectures concurrently plus one in
definition; hundreds of thousands of tests nightly; a functional model exists but is
itself under development and may be wrong.

That last assumption is the interesting one and you should call it out: **you cannot
trust the reference model**, so the design must support cross-checking rather than
assuming an oracle.

### Step 2 — Requirements

**Functional**
- Describe a workload programmatically (directed) and generate them randomly under
  constraints (constrained-random).
- Target multiple architectures from one description.
- Emit whatever the target consumes: a command/method stream, a binary, a trace.
- Check results: self-checking, or comparison against a reference.
- Run against multiple execution backends (sim / emulation / silicon).

**Non-functional**
- **Bit-exact reproducibility from a seed.** Non-negotiable.
- Extensible: new opcode/engine/architecture without editing core code.
- Small, dense output — simulation time is the scarcest resource in the building.
- Debuggable: minimisation, introspection, dry-run, comprehensible errors.
- Stable API — hundreds of existing tests must not break when internals change.

**Out of scope (state it):** the reference model itself, the simulator, and the job farm.
We integrate with them, we do not build them.

### Step 3 — Architecture

```
        ┌──────────────────────────────────────────────────────────┐
        │  FRONT END  (Python + C++ API)                           │
        │  Directed test DSL │ Random generator │ Constraints      │
        └────────────────────────┬─────────────────────────────────┘
                                 │  builds
                                 ▼
        ┌──────────────────────────────────────────────────────────┐
        │  IR — architecture-neutral workload description           │
        │  Program │ BasicBlock │ Instruction │ MemoryObject        │
        │  Surface │ LaunchDesc │ Expectation                       │
        └────────────────────────┬─────────────────────────────────┘
                                 │  passes (validate, legalize, allocate, schedule)
                                 ▼
        ┌──────────────────────────────────────────────────────────┐
        │  BACK ENDS  (one per architecture, plugin-registered)     │
        │  ArchA encoder │ ArchB encoder │ ArchC(next) encoder      │
        │  driven by ISA TABLES (data, not code)                    │
        └────────────────────────┬─────────────────────────────────┘
                                 │  emits method stream / binary / image
                                 ▼
        ┌──────────────────────────────────────────────────────────┐
        │  RUNTIME  (plugin-registered execution backends)          │
        │  RTL-sim │ Emulator │ Silicon │ Functional model          │
        └────────────────────────┬─────────────────────────────────┘
                                 │  results + traces
                                 ▼
        ┌──────────────────────────────────────────────────────────┐
        │  CHECKING & ANALYSIS                                      │
        │  Self-check │ Golden compare │ Cross-backend diff         │
        │  Coverage   │ Minimiser      │ Trace analysis             │
        └──────────────────────────────────────────────────────────┘
```

Say why the IR is the centre: **it is the contract**. Front ends produce it, back ends
consume it, passes transform it, and analysis reads it. Any of the four can change
independently. Without an IR you get an N×M matrix of front ends against targets; with
one you get N+M.

### Step 4 — Key abstractions

```cpp
// ---- IR: architecture-neutral ------------------------------------------
enum class Op { Load, Store, Fma, Add, Branch, Barrier, Atomic, /*...*/ };

struct Operand {
    enum class Kind { Reg, Imm, Mem, Pred } kind;
    uint32_t index = 0;        // register number / memory object id
    int64_t  imm   = 0;
    // ... addressing mode, type, modifiers
};

class Instruction {
public:
    Op op() const noexcept { return op_; }
    std::span<const Operand> operands() const noexcept { return operands_; }
    // Deliberately no encode() here: encoding is architecture-specific and
    // lives in the back end. The IR must not know about any ISA.
private:
    Op op_;
    llvm_like::SmallVector<Operand, 4> operands_;
};

class Program {                      // the workload IR root
public:
    BasicBlock& createBlock();
    MemoryObject& createBuffer(size_t bytes, Alignment a);
    void addExpectation(Expectation e);   // what the checker will verify
private:
    std::vector<std::unique_ptr<BasicBlock>> blocks_;
    std::vector<MemoryObject> memory_;
    std::vector<Expectation> expectations_;
};

// ---- Back end interface -------------------------------------------------
class Encoder {                      // one implementation per architecture
public:
    virtual ~Encoder() = default;
    virtual ArchId arch() const = 0;
    virtual bool   supports(const Instruction&) const = 0;   // legality query
    virtual void   encode(const Instruction&, ByteStream&) const = 0;
};

// ---- Registry: adding an architecture touches no existing file ----------
class EncoderRegistry {
public:
    static EncoderRegistry& instance();          // Meyers singleton, init-order safe
    void  add(ArchId, std::unique_ptr<Encoder>);
    const Encoder& get(ArchId) const;            // throws with a clear message
};

#define REGISTER_ENCODER(ArchEnum, Type)                                   \
    namespace {                                                            \
    const bool registered_##Type = [] {                                    \
        EncoderRegistry::instance().add(ArchEnum, std::make_unique<Type>());\
        return true;                                                       \
    }();                                                                   \
    }
```

And the part that shows domain understanding — **the ISA as data**:

```cpp
struct EncodingRule {
    Op          op;
    uint32_t    opcode_bits;
    uint32_t    opcode_mask;
    FieldSpec   dst, src0, src1, pred, imm;   // bit position + width + signedness
    uint32_t    min_arch;                     // first architecture supporting it
};

// Loaded from a table (generated from the architecture spec), so an ISA change
// is a data change reviewed by an architect — not a code change.
extern const EncodingRule kArchB_Rules[];
```

Say the consequence explicitly: *"the same table drives the encoder and the
disassembler, so they cannot drift apart, and I get round-trip testing for free."*
That single sentence is worth a lot in this interview.

### Step 5 — Determinism, in detail

This is where you separate yourself. Be specific:

**Everything random flows from one seeded generator**, and sub-generators are derived
deterministically from it (`child_seed = hash(master_seed, component_id)`), so adding a
new randomised component does not perturb the sequence consumed by existing ones. That
detail matters: naive designs share one stream, so adding a feature changes every
existing test's behaviour and your whole regression baseline moves.

**Banned as inputs to any decision:** wall-clock time, pointer/address values,
uninitialised memory, `std::unordered_map` iteration order, thread completion order,
hash values that vary per process, filesystem directory order, and any environment
variable not captured in the manifest.

**Captured with every result** (the reproduction manifest): master seed, tool version
and git SHA, ISA table version, configuration, backend and its version, and the exact
command line. One command must rebuild the failing test.

**Enforced by test:** a CI job that runs a suite twice with the same seed and diffs the
generated output byte-for-byte. Determinism that is not continuously tested is
determinism you have already lost.

### Step 6 — Constrained-random generation

```python
# Front-end DSL: readable by a verification engineer, deterministic by construction
suite = Suite(seed=0xC0FFEE)

suite.add(RandomTest(
    name="atomics_contention",
    weights={Op.ATOMIC: 40, Op.LOAD: 30, Op.STORE: 20, Op.FMA: 10},
    constraints=[
        InRange("block_size", 32, 1024, multiple_of=32),
        Implies(lambda t: t.uses_shared, InRange("shared_bytes", 0, 48 * 1024)),
        Distinct("dst_reg", "src_reg"),          # aim at a specific hazard
        AtLeastOnce(Op.BARRIER),                 # guarantee coverage of a feature
    ],
    count=5000,
))
```

The generator must **validate its own output** against architectural legality before
emitting — otherwise a large fraction of "hardware bugs" turn out to be illegal
stimulus, which destroys the verification team's trust in your tool faster than any
crash.

### Step 7 — Checking

Three independent mechanisms, because you cannot trust any single oracle:

1. **Self-checking tests** — the workload computes a result and compares against a value
   derived independently (computed on the host, or an algebraic identity that must hold).
2. **Golden comparison** — run against the functional model and diff. Useful, but the
   model may be wrong, so a mismatch is a *disagreement to triage*, not automatically
   a hardware bug.
3. **Cross-backend differential** — same stimulus on model vs RTL vs emulation vs
   silicon. Disagreement localises the bug: model-vs-RTL disagreement with silicon
   unavailable is the everyday case pre-silicon.

### Step 8 — Failure workflow (the part candidates forget)

```
failure → reproduce (seed + manifest, one command)
        → minimise  (shrink via the constraint system; re-run at a cheaper backend)
        → localise  (cross-backend diff: model vs RTL)
        → attribute (hardware bug? model bug? illegal stimulus from my generator?)
        → file with minimised repro + manifest + trace excerpt
```

**Minimisation** deserves its own sentence: because the workload is an IR, shrinking is
a *program transformation* — delete instructions, shrink loop counts, reduce thread
counts — repeatedly, keeping any reduction that still reproduces the failure. This is
delta debugging, and it works because the IR is structured. That is a second, independent
justification for the IR, and mentioning it shows the design was thought through rather
than pattern-matched.

### Step 9 — Tradeoffs to volunteer

| Decision | Alternative | Why this choice |
|---|---|---|
| Central IR | Direct front-end → target emission | N+M instead of N×M; enables minimisation, analysis, retargeting. Costs: an extra layer, and IR churn when hardware introduces something the IR cannot express |
| Table-driven encoding | Hand-written per-instruction code | ISA change = data change; encoder/disassembler share truth. Costs: table format itself becomes a thing to maintain and version |
| Python front end + C++ core | All C++ | Fast iteration for test authors; performance where it matters. Costs: binding boundary, lifetime hazards, heavier build |
| Plugin registry | Central switch/factory | New architecture touches no existing file. Costs: static-init order subtleties, harder to see what exists |
| Constrained-random | Purely directed | Finds bugs nobody thought to write a test for. Costs: needs coverage measurement and minimisation to be usable at all |

### Step 10 — How I'd test the framework itself

Because the framework is software too, and a verification tool that is itself buggy is
worse than no tool:

- **Round-trip:** encode → decode → compare IR, property-based over random legal IR.
- **Golden files** for generated output, so unintended changes are visible in review.
- **Determinism test** in CI (same seed twice, byte-identical).
- **Fuzz the decoder** so it never crashes or hangs on corrupt input.
- **Self-test suite of known-buggy models** — inject a known bug into a model and assert
  the framework catches it. This tests the *checker*, which otherwise is never tested.
- **Coverage of the framework's own feature matrix**, not just of the DUT.

---

## Part 3 — Three more worked design prompts

### 3.1 "Design a disassembler for a new GPU ISA."

**Clarify:** fixed or variable length? Is there a machine-readable spec? Is it for humans
reading output, or for tools consuming a structure? Must it round-trip with an assembler?
How does it handle data mixed with code, and does it need control-flow reconstruction?

**Design:**
- **Table-driven decoder** generated from the ISA spec: entries of `(mask, match, format)`,
  organised as a decision tree on the discriminating bits rather than a linear scan.
- **Layered output**: `bytes → DecodedInstruction (structured) → printer`. Keep the
  structure separate from the text; other tools want the structure, and the printer is
  where formatting churn lives.
- **CFG reconstruction** as a separate pass: identify leaders (branch targets and
  instructions following branches), form basic blocks, add edges, resolve indirect
  branches conservatively (mark unknown targets rather than guessing).
- **Recursive descent from known entry points** for accuracy, with **linear sweep** as a
  fallback to cover unreachable regions; flag regions where the two disagree, because
  those are exactly where data-in-code lives.
- **Robustness contract**: never crash, never infinite-loop, always make forward
  progress, print `.unknown 0x????????` rather than aborting.

**Test:** round-trip against the assembler, differential against any reference,
fuzz for robustness, golden files for output format, and "every byte accounted for"
on real binaries.

**Say this:** the decode tables must be the *same* tables the encoder uses, generated
from one spec, or the two will drift and you will chase phantom bugs.

### 3.2 "Design a trace format and its analyser."

**Clarify:** volume (GB or TB?), streaming or post-mortem, who reads it, must it be
readable by an unmodified third-party tool, is loss acceptable under overload?

**Design:**
- **Binary, self-describing, versioned.** File header with magic + version + a schema
  block describing record types, so old traces stay readable when the format evolves.
- **Fixed-size record headers** (`kind`, `timestamp`, `payload_len`) with variable
  payloads — enables skipping without parsing, and indexing.
- **Little-endian, explicit widths, no native alignment.** Cross-host reproducibility.
- **Ring buffer per producer, lock-free, one writer**, flushed by a consumer thread —
  avoids contention and preserves per-engine ordering without a global lock.
- **Global ordering** via a monotonic sequence or synchronised timestamps, because
  correlating engines is the main analysis question.
- **Sidecar index** (offsets of key events) built on first read, so repeat queries seek
  instead of scan.
- **Compression** per block (not per record) so you keep random access.

**Analyser:** streaming reader, timeline reconstruction, per-engine occupancy, gap
analysis, and a query layer. Emphasise: the format must be readable by a five-line
Python script — if analysing a trace requires the full toolchain, nobody will analyse traces.

### 3.3 "Design a plugin architecture so a new GPU engine can be added without touching core code."

**Clarify:** in-tree or out-of-tree plugins? Same compiler/ABI? Loaded at build time or
runtime? Do plugins need to be discoverable/enumerable? Version skew tolerated?

**Design:**
- **Narrow, stable, abstract interface** (`Engine`: identify, capabilities, encode,
  submit, check) with a version number in the interface itself.
- **In-tree**: static registration via a registry and a `REGISTER_*` macro.
  **Out-of-tree**: `dlopen` + a C-ABI entry point (`extern "C" Plugin* create_plugin(int abi_version)`)
  — a C ABI because C++ ABI compatibility across compiler versions is not something you
  want to depend on.
- **Capability query rather than type-switching**: core code asks "do you support X?"
  instead of asking "which engine are you?", which is what keeps the core closed to
  modification.
- **Refuse mismatched ABI versions loudly** at load time rather than crashing later.
- **Test harness for plugins**: a conformance suite any plugin must pass, so the
  interface's contract is executable rather than documented in prose.

---

## Part 4 — Behavioral preparation

### 4.1 Why it is weighted here

The JD says "excellent interpersonal skills," "coordinate with GPU architects," and
"work closely with HW & SW teams." That is not filler. Your framework sits between
hardware and software organisations; you will be told contradictory requirements by
people more senior than you, the spec will change under you, and when your tool is
down, other teams stop working. They are hiring for someone who handles that well.

### 4.2 STAR, correctly

**S**ituation (15s, context only) → **T**ask (10s, your specific responsibility) →
**A**ction (60s, *your* decisions, first person singular) → **R**esult (20s, quantified,
plus what you learned).

Two rules people break: say **"I"**, not "we" (they are hiring you, not your team); and
**quantify the result** ("cut regression time from 6 hours to 40 minutes" beats "made it
faster").

### 4.3 The eight stories to prepare

You need eight, each ~2 minutes, each reusable for several questions. Write them out.
The projects in this pack (P1–P7) are legitimate sources for several of them — a real
bug you hit in P3 or P4 is a real debugging story.

| # | Story | Answers the questions |
|---|---|---|
| 1 | **Hardest bug you debugged** | technical depth, tenacity, method |
| 2 | **A design you got wrong and had to change** | humility, learning, engineering judgment |
| 3 | **Disagreement with a colleague or a more senior engineer** | collaboration, influence without authority |
| 4 | **Working with ambiguous/changing requirements** | pre-silicon reality |
| 5 | **Something you optimised, with numbers** | performance engineering, measurement discipline |
| 6 | **Time you supported/unblocked other engineers using your code** | customer orientation |
| 7 | **A project you owned end-to-end** | ownership, scope, delivery |
| 8 | **Something you had to learn fast from nothing** | learning ability — critical for a GPU-adjacent hire |

**Story 1 is the most important.** For this team, a debugging story should show *method*:
how you narrowed it, what you ruled out and how, what tool you used or wrote, and how you
made it reproducible. "I stared at it until I saw it" is not an answer. Ideally include
a memory bug, a race, or a nondeterminism hunt — those are this team's daily life.

### 4.4 Sample answers

**Q: Tell me about the hardest bug you've debugged.** *(shape of a strong answer)*
> *(S)* While building a CUDA reduction for my own benchmark suite, results were correct
> at block size 256 but wrong roughly one run in twenty at 1024 — only in release builds.
> *(T)* I needed to know whether it was my kernel, a compiler issue, or my host-side
> verification. *(A)* First I made it deterministic and reproducible: fixed input data,
> fixed seed, looped the kernel a thousand times to get a reliable failure rate — about
> 4%. Intermittency plus build-configuration sensitivity pointed at a race rather than
> arithmetic, so I ran `compute-sanitizer --tool racecheck`, which flagged a shared-memory
> hazard in the final warp of the reduction. I had written the classic warp-synchronous
> optimisation, dropping `__syncthreads()` for the last 32 elements and relying on
> implicit warp lockstep — which stopped being guaranteed with Volta's independent thread
> scheduling. I fixed it with `__shfl_down_sync` and an explicit mask. *(R)* Failure rate
> went to zero over ten thousand runs, and the shuffle version was about 8% faster than
> the original. The lesson I actually took away was about my *method*: I had been
> re-running and hoping, and the moment I forced the bug to be reproducible and measured
> a failure rate, the diagnosis took fifteen minutes. Now the first thing I do with any
> intermittent failure is make it reproducible and quantify it.

Why this works: it is technically specific, it demonstrates tool knowledge
(`racecheck`), it contains real architecture understanding (independent thread
scheduling), and the lesson is about method rather than the specific bug.

**Q: How do you handle a requirement changing after you've built something?**
> That is the normal case in pre-silicon work rather than an exception, so I try to
> design for it rather than react to it. Concretely, I ask early which parts are expected
> to change, and I make those parts data rather than code — table-driven encoding is the
> clearest example, where an ISA revision becomes a table edit instead of a code change.
> When a change does land that I did not design for, I separate the immediate unblock
> from the structural fix: get the person who is blocked moving, then, if the same class
> of change is likely again, restructure so the next one is cheap. What I try not to do
> is either pre-abstract everything on speculation, which makes the code unreadable, or
> hard-code and hope, which just moves the cost onto whoever comes next.

**Q: Your tool is blocking three teams and you don't know why it's failing. What do you do?**
> Communicate first — tell them it is broken, that I am on it, and when they will hear
> from me again, because the worst outcome is three teams silently blocked while I debug.
> Then look for a workaround or a rollback that unblocks them while I find the real
> cause, because their throughput matters more than my elegance. Then debug properly:
> reproduce it, bisect against the last known-good version, and check the environment
> as well as the code, since "the tool broke" is often a dependency or a data change
> rather than my commit. Afterwards I'd add the failing case to the test suite and, if
> the failure was hard to diagnose, improve the error message or add the introspection
> that would have made it obvious — debuggability is a feature of the tool.

**Q: Tell me about a time you disagreed with a technical decision.**
> Structure it as: the disagreement was about *engineering*, not about *people*; I
> established what we actually disagreed on; I looked for data or a cheap experiment
> rather than arguing from opinion; I stated my position clearly once; and when the
> decision went the other way I committed to it fully and made it work. If the outcome
> later proved me right, note it without gloating; if it proved me wrong, say so plainly
> — that is a stronger answer, not a weaker one.

### 4.5 Questions to ask them

Asking nothing reads as no interest. Asking good questions reads as someone already
doing the job:

- What does the framework's current architecture look like, and what part of it is
  hardest to change today?
- How much of the ISA/architecture description is data-driven versus hand-written code?
- How is determinism maintained across the generator, the model, and the RTL?
- What does the triage workflow look like when a random test fails overnight?
- How much of the team's time goes to new features versus supporting internal users?
- How early in the architecture definition does the tools team get involved?
- What's a bug this infrastructure caught that would have been very expensive to find later?

That last one is a good question and people enjoy answering it.

---

## Part 5 — Review schedule and final week

### 5.1 Spaced repetition

Facts decay. Schedule the re-reads rather than trusting memory:

| Reviewed | Next review |
|---|---|
| Day 0 (first study) | Day 1 |
| Day 1 | Day 3 |
| Day 3 | Day 7 |
| Day 7 | Day 16 |
| Day 16 | Day 35 |

A review is **not** re-reading. A review is: close the file, answer the module's Q&A
from memory, and redraw the whiteboard-checklist diagrams on a blank page. Anything you
fumble resets to day 0.

### 5.2 The final two weeks

**T-14 to T-8:** one full pass of `qa-bank.md`, answering aloud. Mark every question you
could not answer cleanly. Re-study only those. Redraw every whiteboard checklist.

**T-7 to T-3:** mock interviews. Say answers out loud, standing, to a camera or a person.
This feels ridiculous and is the highest-leverage thing in this document — the gap
between "I know this" and "I can say this coherently under mild stress" is enormous and
is invisible until you try. Do two full design-round rehearsals with a timer.

**T-2:** rehearse all eight behavioral stories out loud. Time them; anything over three
minutes gets cut.

**T-1:** `cheatsheet.md` only. Numbers, diagrams, the design-round method. No new material —
learning something new the day before displaces something you already knew.

**Day of:** re-read the 8-minute CLARIFY rule and your "why this role" answer. Nothing else.

### 5.3 Final readiness checklist

You are ready when you can, on a blank page, without notes:

- [ ] Draw the SM block diagram and the full memory hierarchy with latency orders.
- [ ] Draw the complete graphics pipeline and mark programmable vs fixed-function.
- [ ] Draw the pre-silicon flow with speed/fidelity tradeoffs.
- [ ] Write a warp-shuffle reduction and a tiled SGEMM inner loop from memory.
- [ ] Compute occupancy given registers/thread, shared memory/block, and block size.
- [ ] Explain the rule of five, and write a correct move constructor and assignment.
- [ ] Explain acquire/release memory ordering with a concrete producer-consumer example.
- [ ] Trace `cudaMemcpy` from API call to bits in device memory.
- [ ] Design a table-driven decoder and explain why the encoder shares the table.
- [ ] List five sources of nondeterminism and how you eliminate each.
- [ ] Deliver the capstone framework design in 20 minutes with clarifying questions first.
- [ ] Tell all eight behavioral stories in under three minutes each.
- [ ] Name three questions you genuinely want to ask them.

---

## Red Flags

- **Designing before clarifying.** The single most common failure in the design round.
- **No testing story.** You designed a verification framework and never said how it is
  verified.
- **Ignoring determinism** in a domain where it is the load-bearing property.
- **"We" instead of "I"** throughout behavioral answers.
- **Unquantified results.** "It was much faster" invites the question you cannot answer.
- **A rehearsed-sounding weakness** ("I work too hard"). Name a real one and what you
  actively do about it.
- **Badmouthing previous employers or colleagues.** Instant, permanent damage.
- **No questions for them.**
- **Over-engineering in the design round** — proposing six layers of abstraction for a
  problem that needs two, and being unable to say what you would cut if given half the time.
- **Inventing NVIDIA internals.** Say what is public, say what you would need to learn.

---

## Whiteboard checklist

- [ ] The 45-minute design-round time budget, from memory.
- [ ] The eight universal clarifying questions.
- [ ] The capstone framework diagram (front end → IR → back ends → runtime → checking).
- [ ] The determinism rules: single seeded source, derived sub-seeds, banned inputs,
      captured manifest, enforced by CI.
- [ ] The failure workflow: reproduce → minimise → localise → attribute → file.
- [ ] Your eight behavioral stories, one line each, on a sticky note.

Next: **[Consolidated Q&A bank](qa-bank.md)** and **[cheat sheet](cheatsheet.md)**.
