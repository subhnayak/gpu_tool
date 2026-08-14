# Module M2 — Design Patterns for Tools and Frameworks

## Why This Matters for This Role

NVIDIA's GPU development tools teams build frameworks that hundreds of chip architects, driver developers, and verification engineers extend for a *decade*. An instruction-set simulator written today for Blackwell must still be extensible when the next three architectures ship. A workload generator must let a verification engineer add a new engine or opcode by dropping in a single file — not by editing a 40-file switch-statement cascade.

This module isn't about reciting GoF definitions. It's about knowing *which* pattern solves *which* recurring framework problem, writing the C++ that actually compiles, and — critically — knowing when a pattern is overkill or actively harmful. Every example below is drawn from GPU tooling: instruction objects, opcode registries, IR passes, workload builders, command streams, randomization policies, and per-architecture back ends.

---

## 1. Creational Patterns in Tool Code

### 1.1 Factory Method

**The problem:** You have a base `Instruction` class. Each architecture (SM80, SM90, SM100) has its own concrete instruction types. The workload generator shouldn't know which concrete type it's creating — it just asks for "an ADD instruction for the current arch."

```cpp
// --- factory_method.h ---
#include <memory>
#include <string>
#include <cstdint>

// Product hierarchy
class Instruction {
public:
    virtual ~Instruction() = default;
    virtual void encode(uint8_t* buf) const = 0;
    virtual std::string mnemonic() const = 0;
};

// Creator hierarchy — one per architecture
class InstructionFactory {
public:
    virtual ~InstructionFactory() = default;

    // Factory method: subclasses decide the concrete type
    virtual std::unique_ptr<Instruction> createALU(
        const std::string& opcode,
        int dst, int src0, int src1) = 0;

    virtual std::unique_ptr<Instruction> createMemory(
        const std::string& opcode,
        int dst, uint64_t addr) = 0;
};

// --- sm90_instructions.cpp ---
class SM90Add : public Instruction {
    int dst_, src0_, src1_;
public:
    SM90Add(int d, int s0, int s1) : dst_(d), src0_(s0), src1_(s1) {}
    void encode(uint8_t* buf) const override {
        // SM90-specific binary encoding
        uint32_t word = (0x42u << 24) | (dst_ << 16) | (src0_ << 8) | src1_;
        std::memcpy(buf, &word, 4);
    }
    std::string mnemonic() const override { return "IADD3"; }
};

class SM90Factory : public InstructionFactory {
public:
    std::unique_ptr<Instruction> createALU(
            const std::string& opcode,
            int dst, int src0, int src1) override {
        if (opcode == "ADD") return std::make_unique<SM90Add>(dst, src0, src1);
        // ... other opcodes
        throw std::runtime_error("Unknown ALU op: " + opcode);
    }

    std::unique_ptr<Instruction> createMemory(
            const std::string& opcode,
            int dst, uint64_t addr) override;
};
```

**Key insight for the interview:** The factory method lets you *decouple creation policy from usage*. The workload generator codes to `InstructionFactory*` and never includes SM90-specific headers. New architectures = new factory subclass, zero changes to existing code.

### 1.2 Self-Registering Factory / Registry

This is arguably the single most important creational pattern in GPU tool frameworks. The goal: a new opcode or architecture is added by writing *one .cpp file*, and it's automatically available. No central enum, no master switch statement.

```cpp
// --- registry.h ---
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <stdexcept>
#include <mutex>

class Instruction; // forward decl

class InstructionRegistry {
public:
    using Creator = std::function<std::unique_ptr<Instruction>(/* operands */)>;

    static InstructionRegistry& instance() {
        static InstructionRegistry reg; // Meyers singleton — thread-safe in C++11+
        return reg;
    }

    void registerOpcode(const std::string& mnemonic, Creator creator) {
        std::lock_guard<std::mutex> lock(mu_);
        auto [it, inserted] = creators_.emplace(mnemonic, std::move(creator));
        if (!inserted)
            throw std::runtime_error("Duplicate opcode registration: " + mnemonic);
    }

    std::unique_ptr<Instruction> create(const std::string& mnemonic) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = creators_.find(mnemonic);
        if (it == creators_.end())
            throw std::runtime_error("Unknown opcode: " + mnemonic);
        return it->second();
    }

    // Introspection — enumerate all registered opcodes (framework debuggability)
    std::vector<std::string> registeredOpcodes() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> result;
        result.reserve(creators_.size());
        for (auto& [name, _] : creators_)
            result.push_back(name);
        return result;
    }

private:
    InstructionRegistry() = default;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Creator> creators_;
};

// RAII registration helper — drop one of these at file scope
struct OpcodeRegistrar {
    OpcodeRegistrar(const std::string& mnemonic, InstructionRegistry::Creator c) {
        InstructionRegistry::instance().registerOpcode(mnemonic, std::move(c));
    }
};

// Macro that generates a unique static variable
#define REGISTER_OPCODE(mnemonic, CreatorExpr) \
    static OpcodeRegistrar reg_##mnemonic(#mnemonic, CreatorExpr)
```

**Usage — each opcode is one self-contained file:**

```cpp
// --- opcodes/sm90_iadd3.cpp ---
#include "registry.h"
#include "sm90_instructions.h"

REGISTER_OPCODE(IADD3, []() {
    return std::make_unique<SM90Add>(/*defaults*/);
});
```

**Static initialization order fiasco — the caveat you MUST mention in the interview:**

The `OpcodeRegistrar` constructor runs *before* `main()`. If `InstructionRegistry::instance()` depended on another global, you'd hit the static initialization order fiasco. The Meyers singleton (function-local static) guarantees the registry is alive when first accessed. But beware: if you have *two* registries that depend on each other at static-init time, you need explicit init-ordering or a two-phase startup (collect registrations, then resolve references).

Also, in shared libraries (`dlopen`), registration happens at load time — but if the library is never loaded, those opcodes silently don't exist. Defensive framework code enumerates expected opcodes and warns on missing registrations.

### 1.3 Abstract Factory — Architecture Families

When you need a *family* of related objects (instructions, registers, encoder, decoder) that must all be from the same architecture:

```cpp
class ArchFactory {
public:
    virtual ~ArchFactory() = default;
    virtual std::unique_ptr<Instruction>   createInstruction(const std::string& op) = 0;
    virtual std::unique_ptr<RegisterFile>  createRegisterFile() = 0;
    virtual std::unique_ptr<Encoder>       createEncoder() = 0;
    virtual std::unique_ptr<Decoder>       createDecoder() = 0;
};

class SM90ArchFactory : public ArchFactory { /* ... */ };
class SM100ArchFactory : public ArchFactory { /* ... */ };

// The workload generator holds ArchFactory* and never mixes SM90 regs with SM100 encoders.
```

### 1.4 Builder — Complex Workload Assembly

A GPU workload has dozens of optional pieces: shader programs, buffer bindings, render state, synchronization primitives. A constructor with 20 parameters is unreadable. Builder separates construction from representation.

```cpp
class GpuWorkload {
    // Many fields: shaders, buffers, draw calls, barriers...
    friend class WorkloadBuilder;
    std::vector<std::unique_ptr<Instruction>> instructions_;
    std::vector<BufferBinding> buffers_;
    SyncConfig sync_;
    bool validated_ = false;
public:
    void execute(GpuContext& ctx) const;
};

class WorkloadBuilder {
    GpuWorkload wl_;
public:
    // Fluent interface — each method returns *this
    WorkloadBuilder& addInstruction(std::unique_ptr<Instruction> instr) {
        wl_.instructions_.push_back(std::move(instr));
        return *this;
    }

    WorkloadBuilder& bindBuffer(int slot, BufferHandle buf, size_t size) {
        wl_.buffers_.push_back({slot, buf, size});
        return *this;
    }

    WorkloadBuilder& withSync(SyncConfig sync) {
        wl_.sync_ = sync;
        return *this;
    }

    // Terminal operation — validates and moves out
    GpuWorkload build() {
        validate();
        wl_.validated_ = true;
        return std::move(wl_);
    }

private:
    void validate() {
        if (wl_.instructions_.empty())
            throw std::runtime_error("Workload has no instructions");
        // check buffer bindings match shader expectations, etc.
    }
};

// Usage:
// auto workload = WorkloadBuilder()
//     .addInstruction(registry.create("IADD3"))
//     .bindBuffer(0, buf, 4096)
//     .withSync({.barrier = true})
//     .build();
```

### 1.5 Prototype — Cloning Template Workloads

In stimulus generation, you often have a "golden" workload template and need thousands of variants with small mutations. Cloning is cheaper than rebuilding.

```cpp
class Workload {
public:
    virtual ~Workload() = default;
    virtual std::unique_ptr<Workload> clone() const = 0;
    virtual void mutate(RandomEngine& rng) = 0;
};

class MemoryStressWorkload : public Workload {
    std::vector<MemoryOp> ops_;
    AddressPattern pattern_;
public:
    std::unique_ptr<Workload> clone() const override {
        return std::make_unique<MemoryStressWorkload>(*this); // copy ctor
    }

    void mutate(RandomEngine& rng) override {
        // Randomize a few addresses while keeping the structure
        for (auto& op : ops_)
            op.address ^= rng.next() & pattern_.mask;
    }
};
```

### 1.6 Singleton — And Why It's Usually Wrong

Singletons appear everywhere in tool code: the "global config," the "device handle," the "logger." They *work*, but they create hidden coupling and destroy testability and determinism.

```cpp
// The classic Singleton — you'll see this in legacy code
class Config {
public:
    static Config& instance() {
        static Config cfg;
        return cfg;
    }
    int getArchVersion() const { return archVersion_; }
private:
    Config() { /* read from file or env */ }
    int archVersion_ = 90;
};
```

**Why it's a mistake in tool frameworks:**

1. **Testing is hard.** You can't inject a mock config — every test shares global state.
2. **Determinism is killed.** Two threads reading/writing a singleton create order-dependent results. In stimulus generation, non-determinism is a *bug*.
3. **Dependency hiding.** Functions that call `Config::instance()` have an invisible dependency. You can't see it in the signature.

**What to use instead:**

```cpp
// Explicit context object — pass it through the call chain
struct ToolContext {
    const ArchConfig& arch;
    Logger& logger;
    RandomEngine& rng;   // deterministic seed → deterministic runs
    // ...
};

void generateWorkload(ToolContext& ctx) {
    auto instr = ctx.arch.createInstruction("ADD");
    ctx.logger.log("Generated instruction: {}", instr->mnemonic());
}
```

This is *dependency injection* without a DI framework. The context is explicit, testable, and thread-safe if each thread gets its own.

### Interview Q&A — Creational Patterns

**Q: You need to add support for a brand-new GPU architecture. How do you minimize code changes?**
A: I'd use the self-registering factory pattern. Each architecture provides its own factory in a separate translation unit that statically registers itself with a global registry at startup. The core framework code never mentions specific architectures — it discovers them through the registry. Adding a new arch means adding new files and linking them in, with zero edits to existing code. The one caveat is ensuring the static registration runs before any code tries to query the registry, which the Meyers singleton guarantees for the registry itself.

*Follow-up: What if you're using shared libraries?* — With dlopen, registrations fire when the library loads. If the library isn't loaded, those registrations don't exist. So the framework should have an explicit "load architecture plugins from this directory" step at startup, and then validate that all expected architectures are present. Don't rely on silent auto-registration across library boundaries.

**Q: When would you use Builder vs. a constructor with default arguments?**
A: Builder is warranted when the object has many optional sub-parts that interact — like a GPU workload where buffer bindings must match shader inputs, and certain combinations are invalid. The builder can enforce these constraints in its `build()` method. A simple constructor with defaults is fine for value types with a handful of independent fields. The rule of thumb: if you'd need a "validation" step after construction, use a builder so the object is *always* valid once you have it.

*Follow-up: How do you make a builder work with move-only types like unique_ptr?* — The builder accumulates moved-in resources and then moves the entire built product out. The key is making `build()` return by value and using `std::move(wl_)` internally, which leaves the builder in a valid-but-empty state.

**Q: Why is Singleton problematic for a stimulus generator that must reproduce bugs deterministically?**
A: Deterministic replay means the same seed must produce the same stimulus. Singletons holding mutable state (random engine, config that gets patched at runtime, cached results) introduce hidden dependencies that change based on initialization order, thread scheduling, or previous test runs. Instead, I thread an explicit context object carrying the RNG and config through every call. Each test instantiates its own context with a known seed.

*Follow-up: What about a read-only singleton like an ISA descriptor table?* — That's more defensible — truly immutable data doesn't have the ordering or testing issues. But I'd still prefer it as a `const` global or a namespace-scoped `constexpr` table rather than a lazy-init singleton, so the data is available at compile time and visible in debuggers.

**Q: Explain the static initialization order fiasco in the context of the registry pattern.**
A: If two globals depend on each other at construction time, C++ doesn't guarantee which constructor runs first across translation units. The Meyers singleton (function-local static) solves this for *one* global because it's constructed on first use. But if Registry A's registrants try to access Registry B during their static init, and B's registrants try to access A, you get a circular dependency. The fix is to decouple the two: register everything in phase 1 (static init), then resolve cross-references in an explicit phase 2 (`initialize()` called from `main()`).

*Follow-up: How does this interact with `__attribute__((init_priority))` on GCC?* — It gives you control over init order within a single binary, but it's non-portable and doesn't work across shared library boundaries. I'd avoid it and design the system to not need ordered initialization — that's a more robust solution.

**Q: When is Prototype better than a Factory for creating workloads?**
A: Prototype shines when the "template" is expensive to construct — maybe it involves reading a trace file, running a pre-analysis pass, or computing a complex address map. Cloning that template and mutating a few fields is much cheaper than rebuilding from scratch. Factory is better when each instance is genuinely different. In fuzz testing a GPU pipeline, I'd use Prototype: clone the base workload, mutate random fields, submit, repeat.

*Follow-up: How do you handle deep vs. shallow clone for workloads containing shared resources?* — The workload's `clone()` does a deep copy of its instruction list and configuration, but shared immutable resources (like ISA descriptor tables) are referenced via `shared_ptr` and aliased — no deep copy needed. Mutable per-instance state (like allocated addresses) gets fresh copies.

---

## 2. Structural Patterns

### 2.1 Composite — Nested Command Groups

GPU command streams have hierarchical structure: a workload contains passes, passes contain draw call groups, groups contain individual commands. Composite lets you treat a single command and a group of commands uniformly.

```cpp
class CmdNode {
public:
    virtual ~CmdNode() = default;
    virtual void emit(CommandBuffer& cb) const = 0;
    virtual size_t size() const = 0;  // size in bytes when encoded
};

// Leaf: a single GPU method write
class MethodCmd : public CmdNode {
    uint32_t method_;
    uint32_t data_;
public:
    MethodCmd(uint32_t m, uint32_t d) : method_(m), data_(d) {}
    void emit(CommandBuffer& cb) const override {
        cb.write(method_, data_);
    }
    size_t size() const override { return 8; }
};

// Composite: a group of commands (e.g., a render pass)
class CmdGroup : public CmdNode {
    std::string name_;
    std::vector<std::unique_ptr<CmdNode>> children_;
public:
    explicit CmdGroup(std::string name) : name_(std::move(name)) {}

    void add(std::unique_ptr<CmdNode> child) {
        children_.push_back(std::move(child));
    }

    void emit(CommandBuffer& cb) const override {
        cb.beginGroup(name_);
        for (auto& child : children_)
            child->emit(cb);
        cb.endGroup(name_);
    }

    size_t size() const override {
        size_t total = 0;
        for (auto& child : children_)
            total += child->size();
        return total;
    }
};

// Usage — arbitrary nesting:
// auto workload = std::make_unique<CmdGroup>("frame");
// auto pass = std::make_unique<CmdGroup>("renderPass");
// pass->add(std::make_unique<MethodCmd>(0x1234, 0xAB));
// pass->add(std::make_unique<MethodCmd>(0x1238, 0xCD));
// workload->add(std::move(pass));
// workload->emit(cmdBuf);
```

### 2.2 Decorator — Layering Instrumentation

You want to add logging, validation, or performance counting to a command stream *without modifying the stream itself*.

```cpp
// Base interface
class CmdStream {
public:
    virtual ~CmdStream() = default;
    virtual void writeMethod(uint32_t addr, uint32_t data) = 0;
    virtual void fence() = 0;
};

// Concrete stream — writes to hardware or simulator
class HwCmdStream : public CmdStream {
public:
    void writeMethod(uint32_t addr, uint32_t data) override { /* ... */ }
    void fence() override { /* ... */ }
};

// Decorator base
class CmdStreamDecorator : public CmdStream {
protected:
    std::unique_ptr<CmdStream> inner_;
public:
    explicit CmdStreamDecorator(std::unique_ptr<CmdStream> inner)
        : inner_(std::move(inner)) {}

    void writeMethod(uint32_t addr, uint32_t data) override {
        inner_->writeMethod(addr, data);
    }
    void fence() override { inner_->fence(); }
};

// Logging decorator
class LoggingStream : public CmdStreamDecorator {
    Logger& log_;
public:
    LoggingStream(std::unique_ptr<CmdStream> inner, Logger& log)
        : CmdStreamDecorator(std::move(inner)), log_(log) {}

    void writeMethod(uint32_t addr, uint32_t data) override {
        log_.log("WRITE 0x{:08x} = 0x{:08x}", addr, data);
        inner_->writeMethod(addr, data);
    }
};

// Validation decorator — checks method addresses are valid
class ValidatingStream : public CmdStreamDecorator {
    const MethodTable& table_;
public:
    ValidatingStream(std::unique_ptr<CmdStream> inner, const MethodTable& t)
        : CmdStreamDecorator(std::move(inner)), table_(t) {}

    void writeMethod(uint32_t addr, uint32_t data) override {
        if (!table_.isValid(addr))
            throw std::runtime_error("Invalid method address: " + std::to_string(addr));
        inner_->writeMethod(addr, data);
    }
};

// Stack decorators like Unix pipes:
// auto stream = std::make_unique<HwCmdStream>();
// stream = std::make_unique<ValidatingStream>(std::move(stream), methods);
// stream = std::make_unique<LoggingStream>(std::move(stream), logger);
// stream->writeMethod(0x1234, 0xAB); // validates, then logs, then writes
```

### 2.3 Adapter — Wrapping Back Ends

Different simulators or hardware targets have incompatible APIs. Adapter presents a uniform interface.

```cpp
// Target interface the framework expects
class GpuBackend {
public:
    virtual ~GpuBackend() = default;
    virtual void submitWork(const CommandBuffer& cb) = 0;
    virtual void waitIdle() = 0;
};

// Adaptee — legacy C simulator API
extern "C" {
    void sim_push_commands(const uint32_t* data, size_t count);
    void sim_drain();
}

class SimulatorAdapter : public GpuBackend {
public:
    void submitWork(const CommandBuffer& cb) override {
        sim_push_commands(cb.data(), cb.wordCount());
    }
    void waitIdle() override {
        sim_drain();
    }
};
```

### 2.4 Facade

A framework has dozens of subsystems. Facade gives casual users a simple entry point without hiding the internals from power users.

```cpp
class StimGenFacade {
    ArchFactory& arch_;
    WorkloadBuilder builder_;
    InstructionRegistry& registry_;
    CmdStream& stream_;
public:
    // Simple API for common cases
    void generateRandomWorkload(int numInstructions, uint64_t seed) {
        RandomEngine rng(seed);
        for (int i = 0; i < numInstructions; ++i) {
            auto op = registry_.randomOpcode(rng);
            builder_.addInstruction(registry_.create(op));
        }
        auto wl = builder_.build();
        wl.emit(stream_);
    }
    // Power users still access arch_, registry_, etc. directly
};
```

### 2.5 Flyweight — Interning Instruction Descriptors

An ISA has ~2000 opcodes. Each instruction *instance* in a workload references its opcode descriptor, but we shouldn't duplicate the descriptor for every instance.

```cpp
// Shared, immutable descriptor — one per opcode
struct OpcodeDescriptor {
    std::string mnemonic;
    uint16_t encoding;
    uint8_t numSrcs;
    uint8_t numDsts;
    uint32_t latency;
    // ... many fields describing the opcode
};

// Flyweight pool — intern descriptors
class OpcodePool {
    std::unordered_map<std::string, std::shared_ptr<const OpcodeDescriptor>> pool_;
public:
    std::shared_ptr<const OpcodeDescriptor> get(const std::string& mnemonic) {
        auto it = pool_.find(mnemonic);
        if (it != pool_.end()) return it->second;
        throw std::runtime_error("Unknown opcode: " + mnemonic);
    }

    void load(const IsaSpec& spec) {
        for (auto& entry : spec.opcodes()) {
            pool_[entry.mnemonic] = std::make_shared<const OpcodeDescriptor>(entry);
        }
    }
};

// Instruction instance — lightweight, carries only per-instance state
class InstructionInstance {
    std::shared_ptr<const OpcodeDescriptor> desc_; // shared flyweight
    std::array<uint8_t, 4> operands_;              // per-instance extrinsic state
public:
    InstructionInstance(std::shared_ptr<const OpcodeDescriptor> desc,
                       std::array<uint8_t, 4> operands)
        : desc_(std::move(desc)), operands_(operands) {}

    const std::string& mnemonic() const { return desc_->mnemonic; }
    uint8_t dst() const { return operands_[0]; }
};
```

### 2.6 Proxy — Lazy Loading Trace Data

GPU traces can be gigabytes. A proxy represents them without loading everything upfront.

```cpp
class TraceData {
public:
    virtual ~TraceData() = default;
    virtual const std::vector<TraceEntry>& entries() = 0;
};

class LazyTraceProxy : public TraceData {
    std::string filePath_;
    std::unique_ptr<std::vector<TraceEntry>> cache_;
public:
    explicit LazyTraceProxy(std::string path) : filePath_(std::move(path)) {}

    const std::vector<TraceEntry>& entries() override {
        if (!cache_) {
            cache_ = std::make_unique<std::vector<TraceEntry>>();
            loadFromDisk(filePath_, *cache_); // expensive
        }
        return *cache_;
    }
};
```

### 2.7 pImpl — ABI Stability

When your framework is a shared library, changing private members breaks ABI. pImpl hides them.

```cpp
// --- gpu_context.h (stable public header) ---
#include <memory>

class GpuContext {
public:
    GpuContext();
    ~GpuContext();
    GpuContext(GpuContext&&) noexcept;
    GpuContext& operator=(GpuContext&&) noexcept;

    void submit(const CommandBuffer& cb);
    void sync();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- gpu_context.cpp (can change freely) ---
struct GpuContext::Impl {
    DeviceHandle device;
    ChannelPool channels;
    FenceTracker fences;
    // Add fields here without breaking ABI
};

GpuContext::GpuContext() : impl_(std::make_unique<Impl>()) {}
GpuContext::~GpuContext() = default;
GpuContext::GpuContext(GpuContext&&) noexcept = default;
GpuContext& GpuContext::operator=(GpuContext&&) noexcept = default;
```

### Interview Q&A — Structural Patterns

**Q: When would you use Composite in a GPU command buffer system?**
A: GPU command buffers have natural tree structure — a frame contains render passes, a render pass contains draw call bundles, each bundle has individual method writes and state settings. Composite lets me treat a single method write and a group of commands through the same interface: both have `emit()`, both report `size()`. This means I can nest arbitrarily — useful for conditional rendering groups or multi-pass algorithms — and the emitting code doesn't care about depth.

*Follow-up: What's the cost of Composite?* — Every node is heap-allocated and accessed through virtual dispatch. For a hot inner loop encoding millions of commands, this overhead matters. There I'd flatten to a contiguous buffer of plain command structs and reserve the Composite for the *builder* side, not the *encoding* side.

**Q: You need to add performance counting to an existing command stream without modifying it. Which pattern?**
A: Decorator. I wrap the existing stream in a `CountingStream` that increments counters before forwarding each call to the inner stream. I can stack decorators — logging, validation, counting — in any order. This follows the open/closed principle: the original stream class is closed for modification but open for extension.

*Follow-up: How is this different from just subclassing?* — Subclassing gives you one static extension. Decorators compose at runtime. I can add logging in debug builds and skip it in release, without recompiling the stream class.

**Q: Why is pImpl important for a framework shipped as a shared library?**
A: Adding or removing private data members changes the class size, which breaks ABI — every client compiled against the old header will read the wrong offsets. pImpl moves the data behind a pointer so the header-visible class is always `sizeof(unique_ptr)`. I can add fields to `Impl` in the `.cpp` and ship a new `.so` without recompiling clients. The cost is one extra heap allocation and one pointer indirection per access.

*Follow-up: When would you NOT use pImpl?* — For small, frequently-instantiated value types in hot paths (like a `Vec3`). The heap allocation and indirection cost dominates. Also for header-only libraries where there's no binary compatibility concern.

**Q: Explain Flyweight in the context of instruction objects.**
A: An ISA might have 2000 opcodes. A workload might contain millions of instruction instances. Each instance should not duplicate the opcode descriptor (mnemonic, encoding format, latency, operand count). Instead, I intern the descriptors in a pool — one shared immutable descriptor per opcode. Each instruction instance holds a `shared_ptr<const OpcodeDescriptor>` plus only its per-instance state (register operands, immediates). Memory usage drops from gigabytes to megabytes.

*Follow-up: Why shared_ptr and not a raw pointer to a pool-owned object?* — Both work. `shared_ptr` is safer if instruction instances outlive the pool (e.g., cached in analysis results). A raw pointer is fine if the pool's lifetime strictly contains all instances — and it avoids the atomic ref-count overhead. I'd document the lifetime contract and use raw pointers in the hot path.

**Q: How would you design a proxy for a multi-gigabyte GPU trace?**
A: The proxy implements the same `TraceData` interface as the real data but delays reading the file until `entries()` is first called. I'd likely go further and implement a *windowed* proxy that only loads the trace section currently being analyzed, using memory-mapped I/O and a page cache. The proxy could also prefetch the next section while the current one is being processed.

*Follow-up: What about thread safety?* — I'd use `std::call_once` or `std::atomic<bool>` for the lazy initialization to ensure exactly one thread does the load. The loaded data is immutable once populated, so no further synchronization is needed for reads.

---

## 3. Behavioral Patterns

### 3.1 Visitor — IR / AST Traversal (Deep Dive)

Visitor is the bread and butter of compilers, disassemblers, and analysis tools. It's the solution to: *I have a fixed set of node types and I keep adding new operations over them.*

**The double-dispatch mechanism:**

```cpp
// Forward declarations for all node types
class ALUInstr;
class MemInstr;
class BranchInstr;
class BarrierInstr;

// Visitor interface — one method per concrete node type
class InstrVisitor {
public:
    virtual ~InstrVisitor() = default;
    virtual void visit(ALUInstr& instr) = 0;
    virtual void visit(MemInstr& instr) = 0;
    virtual void visit(BranchInstr& instr) = 0;
    virtual void visit(BarrierInstr& instr) = 0;
};

// IR node base
class IRInstr {
public:
    virtual ~IRInstr() = default;
    virtual void accept(InstrVisitor& v) = 0; // first dispatch (virtual)
};

// Concrete node — dispatches to the right overload (second dispatch)
class ALUInstr : public IRInstr {
public:
    int opcode;
    int dst, src0, src1;

    void accept(InstrVisitor& v) override {
        v.visit(*this); // compiler picks visit(ALUInstr&) — second dispatch
    }
};

class MemInstr : public IRInstr {
public:
    int opcode;
    int dst;
    uint64_t address;
    int width;

    void accept(InstrVisitor& v) override { v.visit(*this); }
};

class BranchInstr : public IRInstr {
public:
    int target;
    bool conditional;

    void accept(InstrVisitor& v) override { v.visit(*this); }
};

class BarrierInstr : public IRInstr {
public:
    int barrierId;

    void accept(InstrVisitor& v) override { v.visit(*this); }
};
```

**Concrete visitors — each is a separate pass:**

```cpp
// Disassembly printer
class Disassembler : public InstrVisitor {
    std::ostream& out_;
public:
    explicit Disassembler(std::ostream& out) : out_(out) {}

    void visit(ALUInstr& i) override {
        out_ << "R" << i.dst << " = ALU(op=" << i.opcode
             << ", R" << i.src0 << ", R" << i.src1 << ")\n";
    }
    void visit(MemInstr& i) override {
        out_ << "R" << i.dst << " = LOAD [0x" << std::hex << i.address << "]\n";
    }
    void visit(BranchInstr& i) override {
        out_ << "BRANCH -> " << i.target
             << (i.conditional ? " (cond)" : " (uncond)") << "\n";
    }
    void visit(BarrierInstr& i) override {
        out_ << "BARRIER #" << i.barrierId << "\n";
    }
};

// Register usage analysis
class RegUsageAnalyzer : public InstrVisitor {
    std::set<int> usedRegs_;
public:
    void visit(ALUInstr& i) override {
        usedRegs_.insert(i.dst);
        usedRegs_.insert(i.src0);
        usedRegs_.insert(i.src1);
    }
    void visit(MemInstr& i) override { usedRegs_.insert(i.dst); }
    void visit(BranchInstr&) override { /* no regs */ }
    void visit(BarrierInstr&) override { /* no regs */ }

    const std::set<int>& usedRegisters() const { return usedRegs_; }
};

// Running a pass over an IR:
// std::vector<std::unique_ptr<IRInstr>> program;
// Disassembler dis(std::cout);
// for (auto& instr : program)
//     instr->accept(dis);
```

**The expression problem tradeoff — critical for interview:**

```
                  Easy to add new operations | Easy to add new types
  ─────────────────────────────────────────────────────────────────
  Visitor              ✓                     |        ✗
  Inheritance          ✗                     |        ✓
```

- **Visitor** is right when the node types are *stable* (your ISA nodes don't change often) but you keep adding new passes (analysis, optimization, printing, encoding).
- **Inheritance (virtual methods)** is right when operations are stable but types keep being added.

In GPU tool compilers, the IR node set is usually stable within an architecture generation, and you constantly add passes → Visitor wins.

**Acyclic Visitor — when the node set isn't fully stable:**

```cpp
// Marker interface — no methods
class VisitorBase {
public:
    virtual ~VisitorBase() = default;
};

// Per-type visitor interface — each is independent
template <typename T>
class TypedVisitor {
public:
    virtual void visit(T& node) = 0;
};

class IRNode {
public:
    virtual ~IRNode() = default;
    virtual void accept(VisitorBase& v) = 0;
};

class ALUNode : public IRNode {
public:
    void accept(VisitorBase& v) override {
        // dynamic_cast — succeeds only if visitor handles ALUNode
        if (auto* av = dynamic_cast<TypedVisitor<ALUNode>*>(&v))
            av->visit(*this);
        // Otherwise silently skip — visitor doesn't care about this type
    }
};

// A visitor that only handles ALU and Mem nodes:
class PartialAnalyzer : public VisitorBase,
                        public TypedVisitor<ALUNode>,
                        public TypedVisitor<MemNode> {
public:
    void visit(ALUNode& n) override { /* ... */ }
    void visit(MemNode& n) override { /* ... */ }
    // Doesn't implement TypedVisitor<BranchNode> — silently skipped
};
```

The cost is a `dynamic_cast` per node per visit — slow in hot loops. Use only when the node set genuinely evolves across architecture generations.

**std::variant + std::visit — the modern closed-set alternative:**

```cpp
#include <variant>
#include <vector>

struct ALUOp  { int opcode, dst, src0, src1; };
struct MemOp  { int dst; uint64_t addr; int width; };
struct BrOp   { int target; bool cond; };
struct BarOp  { int barId; };

using IRInstrV = std::variant<ALUOp, MemOp, BrOp, BarOp>;

// "Visitor" is just an overload set:
struct Disassemble {
    std::ostream& out;
    void operator()(const ALUOp& a) { out << "ALU R" << a.dst << "\n"; }
    void operator()(const MemOp& m) { out << "MEM R" << m.dst << "\n"; }
    void operator()(const BrOp& b)  { out << "BR " << b.target << "\n"; }
    void operator()(const BarOp& r) { out << "BAR #" << r.barId << "\n"; }
};

void disassemble(const std::vector<IRInstrV>& program, std::ostream& out) {
    Disassemble dis{out};
    for (auto& instr : program)
        std::visit(dis, instr);
}
```

**When to prefer variant:** The type set is small and truly closed; you want value semantics and cache-friendly contiguous storage (no heap allocation per node); you want the compiler to enforce exhaustive handling (a missing overload is a compile error).

**When to prefer classic Visitor:** The type set is large (50+ node kinds); you need polymorphic ownership via `unique_ptr<IRNode>`; the framework is extended by other teams who add visitors without recompiling the IR.

### 3.2 Strategy — Pluggable Policies

```cpp
// Policy interface for register allocation in a workload generator
class RegAllocPolicy {
public:
    virtual ~RegAllocPolicy() = default;
    virtual int allocate(const InstructionInstance& instr,
                         const RegisterState& state) = 0;
};

class SequentialAlloc : public RegAllocPolicy {
    int next_ = 0;
public:
    int allocate(const InstructionInstance&, const RegisterState&) override {
        return next_++;
    }
};

class RandomAlloc : public RegAllocPolicy {
    RandomEngine& rng_;
    int maxRegs_;
public:
    RandomAlloc(RandomEngine& rng, int maxRegs) : rng_(rng), maxRegs_(maxRegs) {}
    int allocate(const InstructionInstance&, const RegisterState&) override {
        return rng_.nextInt(maxRegs_);
    }
};

class HazardAwareAlloc : public RegAllocPolicy {
public:
    int allocate(const InstructionInstance& instr,
                 const RegisterState& state) override {
        // Pick a register that won't cause a data hazard
        for (int r = 0; r < state.numRegs(); ++r)
            if (!state.hasPendingWrite(r))
                return r;
        return 0; // fallback — will stall
    }
};

// The generator doesn't know or care which policy it's using:
class WorkloadGenerator {
    std::unique_ptr<RegAllocPolicy> regAlloc_;
public:
    explicit WorkloadGenerator(std::unique_ptr<RegAllocPolicy> policy)
        : regAlloc_(std::move(policy)) {}

    void generate(/*...*/) {
        // ...
        int reg = regAlloc_->allocate(instr, regState);
        // ...
    }
};
```

### 3.3 Command — GPU Methods as First-Class Objects

```cpp
class GpuCommand {
public:
    virtual ~GpuCommand() = default;
    virtual void execute(GpuChannel& ch) = 0;
    virtual void undo(GpuChannel& ch) {}   // optional — for state tracking
    virtual std::string describe() const = 0;

    // Serialization for replay
    virtual void serialize(std::ostream& out) const = 0;
    static std::unique_ptr<GpuCommand> deserialize(std::istream& in);
};

class SetMethodCmd : public GpuCommand {
    uint32_t method_;
    uint32_t newValue_;
    uint32_t oldValue_ = 0;
public:
    SetMethodCmd(uint32_t m, uint32_t v) : method_(m), newValue_(v) {}

    void execute(GpuChannel& ch) override {
        oldValue_ = ch.readMethod(method_);
        ch.writeMethod(method_, newValue_);
    }

    void undo(GpuChannel& ch) override {
        ch.writeMethod(method_, oldValue_);
    }

    std::string describe() const override {
        return "SET 0x" + toHex(method_) + " = 0x" + toHex(newValue_);
    }

    void serialize(std::ostream& out) const override {
        out.write(reinterpret_cast<const char*>(&method_), 4);
        out.write(reinterpret_cast<const char*>(&newValue_), 4);
    }
};

// Command queue — supports replay, logging, undo
class CommandQueue {
    std::vector<std::unique_ptr<GpuCommand>> history_;
public:
    void submit(std::unique_ptr<GpuCommand> cmd, GpuChannel& ch) {
        cmd->execute(ch);
        history_.push_back(std::move(cmd));
    }

    void undoLast(GpuChannel& ch) {
        if (!history_.empty()) {
            history_.back()->undo(ch);
            history_.pop_back();
        }
    }

    void replayAll(GpuChannel& ch) {
        for (auto& cmd : history_)
            cmd->execute(ch);
    }
};
```

### 3.4 Observer — Event/Callback Systems

```cpp
class SimEvent {
public:
    uint64_t cycle;
    std::string unit;
    std::string description;
};

class SimObserver {
public:
    virtual ~SimObserver() = default;
    virtual void onEvent(const SimEvent& event) = 0;
};

class Simulator {
    std::vector<SimObserver*> observers_;  // non-owning
public:
    void addObserver(SimObserver* obs) { observers_.push_back(obs); }

    void notifyAll(const SimEvent& event) {
        for (auto* obs : observers_)
            obs->onEvent(event);
    }

    void step() {
        // ... run one cycle ...
        notifyAll({cycle_, "SM", "Warp 3 issued IADD3"});
    }
};

// Concrete observer — coverage collector
class CoverageCollector : public SimObserver {
    std::unordered_map<std::string, int> hitCounts_;
public:
    void onEvent(const SimEvent& event) override {
        hitCounts_[event.description]++;
    }
};
```

### 3.5 State Machine — Protocol Modeling

```cpp
enum class ChannelState { Idle, Running, Faulted, Halted };

class GpuChannelFSM {
    ChannelState state_ = ChannelState::Idle;
public:
    ChannelState state() const { return state_; }

    void onSubmit() {
        if (state_ != ChannelState::Idle)
            throw std::runtime_error("Cannot submit: channel not idle");
        state_ = ChannelState::Running;
    }

    void onComplete() {
        if (state_ != ChannelState::Running)
            throw std::runtime_error("Unexpected completion");
        state_ = ChannelState::Idle;
    }

    void onFault() {
        state_ = ChannelState::Faulted; // can transition from any state
    }

    void onReset() {
        if (state_ == ChannelState::Faulted)
            state_ = ChannelState::Idle;
    }
};
```

### 3.6 Template Method

```cpp
class ArchTestBase {
public:
    // Template method — fixed algorithm skeleton
    void run() {
        auto ctx = setupContext();        // overridden per-arch
        auto workload = buildWorkload();  // overridden
        validate(workload);              // common logic
        submitAndWait(ctx, workload);    // common logic
        auto result = readBack(ctx);     // overridden
        checkResult(result);             // common logic
    }

protected:
    virtual ~ArchTestBase() = default;
    virtual GpuContext setupContext() = 0;
    virtual GpuWorkload buildWorkload() = 0;
    virtual ResultData readBack(GpuContext& ctx) = 0;

    // Default implementations that subclasses CAN override
    virtual void validate(const GpuWorkload& wl) {
        if (wl.empty()) throw std::runtime_error("empty workload");
    }

    void submitAndWait(GpuContext& ctx, const GpuWorkload& wl) { /* ... */ }
    void checkResult(const ResultData& data) { /* ... */ }
};
```

### 3.7 Chain of Responsibility — Pass Pipelines

```cpp
class IRPass {
public:
    virtual ~IRPass() = default;
    virtual void run(IRProgram& program) = 0;
    virtual std::string name() const = 0;
};

class PassPipeline {
    std::vector<std::unique_ptr<IRPass>> passes_;
    Logger& log_;
public:
    explicit PassPipeline(Logger& log) : log_(log) {}

    void addPass(std::unique_ptr<IRPass> pass) {
        passes_.push_back(std::move(pass));
    }

    void run(IRProgram& program) {
        for (auto& pass : passes_) {
            log_.log("Running pass: {}", pass->name());
            pass->run(program);
            if (program.hasErrors()) {
                log_.log("Pass {} produced errors, stopping pipeline", pass->name());
                return;  // early termination
            }
        }
    }
};

// Usage:
// pipeline.addPass(std::make_unique<DeadCodeElimination>());
// pipeline.addPass(std::make_unique<RegisterAllocation>());
// pipeline.addPass(std::make_unique<InstructionScheduling>());
// pipeline.addPass(std::make_unique<BinaryEncoding>());
// pipeline.run(program);
```

### 3.8 Interpreter — Stimulus IR

A lightweight pattern for a domain-specific "stimulus description language":

```cpp
// AST nodes for a tiny stimulus description language
class StimExpr {
public:
    virtual ~StimExpr() = default;
    virtual uint64_t evaluate(StimContext& ctx) const = 0;
};

class Literal : public StimExpr {
    uint64_t value_;
public:
    explicit Literal(uint64_t v) : value_(v) {}
    uint64_t evaluate(StimContext&) const override { return value_; }
};

class RegRef : public StimExpr {
    std::string name_;
public:
    explicit RegRef(std::string n) : name_(std::move(n)) {}
    uint64_t evaluate(StimContext& ctx) const override {
        return ctx.getRegister(name_);
    }
};

class BinOp : public StimExpr {
    char op_;
    std::unique_ptr<StimExpr> lhs_, rhs_;
public:
    BinOp(char op, std::unique_ptr<StimExpr> l, std::unique_ptr<StimExpr> r)
        : op_(op), lhs_(std::move(l)), rhs_(std::move(r)) {}

    uint64_t evaluate(StimContext& ctx) const override {
        auto l = lhs_->evaluate(ctx);
        auto r = rhs_->evaluate(ctx);
        switch (op_) {
            case '+': return l + r;
            case '&': return l & r;
            case '|': return l | r;
            default:  throw std::runtime_error("Unknown op");
        }
    }
};
```

### Interview Q&A — Behavioral Patterns

**Q: Why do compilers use Visitor instead of putting the operations in virtual methods on the nodes?**
A: Because compilers have a stable set of IR node types but constantly add new passes — optimization, analysis, lowering, printing. With virtual methods, adding a new operation means modifying every node class. With Visitor, adding a new pass means adding one new class that implements the visitor interface. The tradeoff is the expression problem: Visitor makes adding new operations easy but adding new node types hard (you have to update every visitor). In a compiler, that's the right tradeoff because the IR is stable.

*Follow-up: When would you switch to std::variant + std::visit?* — When the node set is small (say, under 15 types), I want value semantics and contiguous memory layout for cache efficiency, and I want the compiler to enforce exhaustive pattern matching. It's great for a closed IR in a single tool. It doesn't work well when external teams need to add new node types without recompiling the variant definition.

**Q: Design a pluggable randomization strategy for a stimulus generator.**
A: I'd define a `RandomizationPolicy` interface with methods like `pickOpcode()`, `pickRegister()`, `pickAddress()`. Concrete strategies implement different distributions: uniform random, biased toward corner cases, sequential sweep, or scenario-specific targeting. The generator takes a `RandomizationPolicy&` via constructor injection. This lets us switch policies per test scenario without changing the generator. It's the Strategy pattern with dependency injection.

*Follow-up: How do you ensure determinism?* — Each policy gets its own `RandomEngine` initialized with a known seed, passed through the context object. I'd also make policies stateless except for the RNG state — no hidden caches or globals.

**Q: How would you use Command pattern for GPU command stream replay?**
A: Each GPU method write becomes a Command object with `execute()`, `undo()`, and `serialize()` methods. A CommandQueue records history. For replay, I deserialize the commands and re-execute them. For debugging, I can print a human-readable log of every command. For bisection, I can replay the first N commands to find where a failure starts. The key design choice is whether commands capture state for undo — that costs memory but enables powerful debugging.

*Follow-up: What about batching for performance?* — For high-throughput replay, I'd have a `BatchCommand` that aggregates many small method writes into a single memcpy-sized block. The batch's `execute()` copies the whole block to the command buffer. Individual commands are kept only for debugging/bisection modes.

**Q: Explain double dispatch in Visitor without using the word "visitor."**
A: I have a collection of objects of different types behind a common base pointer. I want to call a function that depends on both the object's runtime type and which operation I'm performing. The first dispatch is the virtual `accept()` call, which resolves the object's type. Inside `accept()`, the object calls back to the operation object's `visit()` with `*this`, which is now the concrete type — that's the second dispatch, resolved by overload resolution at compile time. Two dispatches, two type resolutions, hence "double dispatch."

*Follow-up: Why not use dynamic_cast instead?* — You could, but it's slower (RTTI lookup vs. vtable call), error-prone (you might forget a type), and not enforced by the compiler. The Visitor interface forces every visitor to handle every type — the compiler tells you if you forgot one.

**Q: When is a State pattern better than a switch on an enum?**
A: When the state-specific behavior is complex enough that the switch cases would be dozens of lines each, and when transitions have side effects (logging, validation, resource management). The State pattern puts each state's behavior in its own class, making it testable in isolation. For a GPU channel with states like Idle/Running/Faulted/Halted, each state has different rules about what operations are valid, what faults mean, and how resets work. A switch statement for all that becomes unmaintainable.

*Follow-up: What about a simple 3-state FSM?* — A switch on enum is perfectly fine. The pattern overhead (one class per state, a state pointer, heap allocation on transition) isn't justified for simple FSMs. Use State pattern when states have *rich* behavior, not just transition logic.

---

## 4. Modern C++ Idioms That Replace or Improve Classic Patterns

### 4.1 Type Erasure — Build It from Scratch

Type erasure lets you hold heterogeneous types with value semantics — no base class required.

```cpp
#include <memory>
#include <iostream>

// Type-erased Instruction handle — any type that has encode() and mnemonic()
class AnyInstruction {
    // Internal interface — hidden from users
    struct Concept {
        virtual ~Concept() = default;
        virtual void encode(uint8_t* buf) const = 0;
        virtual std::string mnemonic() const = 0;
        virtual std::unique_ptr<Concept> clone() const = 0;
    };

    // Model — wraps any concrete type that satisfies the concept
    template <typename T>
    struct Model final : Concept {
        T obj_;
        explicit Model(T obj) : obj_(std::move(obj)) {}
        void encode(uint8_t* buf) const override { obj_.encode(buf); }
        std::string mnemonic() const override { return obj_.mnemonic(); }
        std::unique_ptr<Concept> clone() const override {
            return std::make_unique<Model>(obj_);
        }
    };

    std::unique_ptr<Concept> impl_;

public:
    // Constructor accepts ANY type with encode() and mnemonic()
    template <typename T>
    AnyInstruction(T obj) : impl_(std::make_unique<Model<T>>(std::move(obj))) {}

    // Value semantics — copyable
    AnyInstruction(const AnyInstruction& other) : impl_(other.impl_->clone()) {}
    AnyInstruction& operator=(const AnyInstruction& other) {
        impl_ = other.impl_->clone();
        return *this;
    }
    AnyInstruction(AnyInstruction&&) noexcept = default;
    AnyInstruction& operator=(AnyInstruction&&) noexcept = default;

    void encode(uint8_t* buf) const { impl_->encode(buf); }
    std::string mnemonic() const { return impl_->mnemonic(); }
};

// Now ANY type with the right methods works — no inheritance required:
struct NopInstr {
    void encode(uint8_t* buf) const { std::memset(buf, 0, 4); }
    std::string mnemonic() const { return "NOP"; }
};

// Usage:
// std::vector<AnyInstruction> program;
// program.push_back(NopInstr{});  // no base class needed!
```

**Small-object optimization (SOO):**

The above always heap-allocates. For small types (up to ~32 bytes), we can store them inline:

```cpp
class AnyInstructionSBO {
    struct Concept { /* same as above */ };

    template <typename T>
    struct Model final : Concept { /* same as above */ };

    static constexpr size_t BufSize = 32;
    static constexpr size_t BufAlign = alignof(std::max_align_t);

    alignas(BufAlign) char buf_[BufSize];
    Concept* ptr_ = nullptr; // points into buf_ or to heap

    bool isLocal() const {
        return reinterpret_cast<const char*>(ptr_) >= buf_
            && reinterpret_cast<const char*>(ptr_) < buf_ + BufSize;
    }

public:
    template <typename T>
    AnyInstructionSBO(T obj) {
        if constexpr (sizeof(Model<T>) <= BufSize
                      && alignof(Model<T>) <= BufAlign) {
            ptr_ = new (buf_) Model<T>(std::move(obj)); // placement new
        } else {
            ptr_ = new Model<T>(std::move(obj)); // heap fallback
        }
    }

    ~AnyInstructionSBO() {
        if (ptr_) {
            if (isLocal()) ptr_->~Concept();
            else delete ptr_;
        }
    }

    void encode(uint8_t* buf) const { ptr_->encode(buf); }
    std::string mnemonic() const { return ptr_->mnemonic(); }
};
```

### 4.2 CRTP — Static Polymorphism

```cpp
template <typename Derived>
class InstrBase {
public:
    void emit(CommandBuffer& cb) {
        // Calls the derived class's implementation — no virtual call
        static_cast<Derived*>(this)->emitImpl(cb);
    }

    std::string describe() {
        return static_cast<Derived*>(this)->describeImpl();
    }
};

class FastALU : public InstrBase<FastALU> {
public:
    void emitImpl(CommandBuffer& cb) {
        cb.writeRaw(encodedData_, sizeof(encodedData_));
    }
    std::string describeImpl() { return "FADD"; }
private:
    uint8_t encodedData_[8] = {};
};

// The compiler inlines emitImpl through the CRTP base — zero overhead.
// But: you can't put FastALU and FastMem in the same vector<InstrBase*>,
// because InstrBase<FastALU> and InstrBase<FastMem> are different types.
```

**When CRTP beats virtual dispatch:** In hot encoding loops where you process millions of instructions of a known type. The compiler inlines everything.

**When CRTP loses:** When you need runtime heterogeneity (a vector of mixed instruction types). Then you need type erasure or virtual dispatch. Also, CRTP increases code bloat (each instantiation duplicates the base template) and produces worse error messages.

### 4.3 Policy-Based Design

```cpp
// Policies are template parameters — composed at compile time
template <typename AllocPolicy, typename EncodingPolicy>
class InstrGenerator {
    AllocPolicy alloc_;
    EncodingPolicy encode_;
public:
    InstrGenerator(AllocPolicy a, EncodingPolicy e)
        : alloc_(std::move(a)), encode_(std::move(e)) {}

    void generate(IRProgram& prog) {
        for (auto& instr : prog) {
            int reg = alloc_.allocateReg(instr);
            encode_.encode(instr, reg);
        }
    }
};

// Policies are plain types — no inheritance required
struct LinearAlloc {
    int next = 0;
    int allocateReg(const IRInstr&) { return next++; }
};

struct SM90Encoding {
    void encode(IRInstr& instr, int reg) { /* SM90-specific */ }
};

// Compose at compile time:
// InstrGenerator<LinearAlloc, SM90Encoding> gen(LinearAlloc{}, SM90Encoding{});
```

### 4.4 Tag Dispatch and if constexpr

```cpp
// Tag dispatch — select implementation at compile time
struct Scalar {};
struct Vector {};
struct Matrix {};

template <typename DataTag>
void processOperand(const Operand& op, Scalar) {
    // scalar-specific handling
}

template <typename DataTag>
void processOperand(const Operand& op, Vector) {
    // vector-specific handling — uses SIMD intrinsics
}

// Modern replacement — if constexpr:
enum class OpKind { Scalar, Vector, Matrix };

template <OpKind K>
void processOperand(const Operand& op) {
    if constexpr (K == OpKind::Scalar) {
        // scalar path
    } else if constexpr (K == OpKind::Vector) {
        // vector path — can reference types that don't exist for Scalar
    } else {
        static_assert(K == OpKind::Matrix);
        // matrix path
    }
}
```

### 4.5 Concepts as Interface Documentation

```cpp
template <typename T>
concept InstructionLike = requires(T t, uint8_t* buf) {
    { t.encode(buf) } -> std::same_as<void>;
    { t.mnemonic() } -> std::convertible_to<std::string>;
    { t.sizeBytes() } -> std::same_as<size_t>;
};

template <typename T>
concept RegAllocPolicyLike = requires(T t, const IRInstr& i, const RegState& s) {
    { t.allocateReg(i, s) } -> std::same_as<int>;
};

// Now the concept documents the interface AND the compiler checks it:
template <InstructionLike Instr>
void emitAll(const std::vector<Instr>& program, CommandBuffer& cb) {
    for (auto& instr : program) {
        uint8_t buf[64];
        instr.encode(buf);
        cb.write(buf, instr.sizeBytes());
    }
}
```

### 4.6 The Cost Model of Virtual Calls

**What actually happens:**

```
ptr->encode(buf);

1. Load ptr          → cache miss if ptr is cold
2. Load ptr->vptr    → usually hot (same cache line as object)
3. Load vptr[slot]   → CACHE MISS if this vtable hasn't been seen recently
4. Indirect call     → branch prediction miss on first call or mixed types
5. Function body     → no inlining possible across the indirect call
```

**When it matters:** In a hot inner loop encoding 10 million instructions per second, the inlining barrier alone can cost 2-5x. The branch predictor also struggles if the loop processes mixed instruction types (polymorphic dispatch is unpredictable).

**When it doesn't matter:** In framework setup, pass orchestration, builder API calls — anything that runs once or runs at millisecond granularity. Optimizing virtual calls there is premature optimization.

**The practical rule:** Use virtual dispatch for your framework API and pass infrastructure. In the inner loop of your encoder or simulator, use `std::variant`, CRTP, or hand-written type-specific loops.

### Interview Q&A — Modern C++ Idioms

**Q: Build a type-erased handle for "anything that can be encoded." Explain the mechanism.**
A: I create three layers: a `Concept` interface with pure virtuals for `encode()` and `mnemonic()`, a `Model<T>` template that wraps any concrete type satisfying those operations, and an outer `AnyInstruction` value type that holds `unique_ptr<Concept>`. The constructor is a template that creates `Model<T>`, erasing the concrete type. The user sees value semantics; the implementation uses virtual dispatch internally. It's the same technique `std::function` uses: Concept = callable interface, Model = the lambda/functor wrapper.

*Follow-up: How would you add small-object optimization?* — I embed a `alignas(max_align_t) char buf[32]` inside AnyInstruction. If `sizeof(Model<T>) <= 32`, I placement-new into the buffer instead of heap-allocating. The destructor checks whether the pointer falls within the buffer to decide between `ptr->~Concept()` and `delete ptr`. This avoids heap allocation for small types, which is the common case for instruction handles.

**Q: When would you choose CRTP over virtual dispatch?**
A: When I know the concrete type at compile time and I'm in a hot loop. For example, an encoder that processes a batch of known-type instructions — CRTP lets the compiler inline the encoding logic. The cost is that I lose runtime heterogeneity (can't put different CRTP types in the same container) and I get code bloat from template instantiation. For framework APIs where different types coexist, I use virtual dispatch or type erasure.

*Follow-up: Can you combine CRTP with virtual dispatch?* — Yes, a common pattern is CRTP for the hot path within a single type and virtual dispatch at the boundary between types. For example, `InstrBase<Derived>` provides inlined helper methods, while the top-level `Instruction` base class has a virtual `encode()` that each CRTP-derived class overrides.

**Q: Why might you prefer std::variant over a class hierarchy for IR nodes?**
A: Three reasons: value semantics (no heap allocation per node), contiguous storage in vectors (cache-friendly), and exhaustive match checking (the compiler errors if you forget a case in `std::visit`). The downside is that the variant's type list is closed — adding a new node type requires modifying the variant definition and recompiling everything. For a small, stable IR (< 15 types) in a single tool, variant is better. For a large framework IR extended by other teams, use a class hierarchy.

*Follow-up: What's the runtime cost of std::visit?* — With a good compiler, `std::visit` compiles to a jump table indexed by the variant's discriminant — equivalent to a switch statement. The cost is one indirect jump, which is well-predicted by the branch predictor since the discriminant is an integer. It's faster than virtual dispatch because there's no pointer chase through a vtable.

**Q: When does virtual call overhead actually matter in GPU tools?**
A: It matters in the innermost loop of an instruction encoder, a simulator cycle loop, or a high-throughput trace parser — anything that runs millions of iterations per second. The cost isn't just the indirect call; it's the inlining barrier. The compiler can't inline through a virtual call, which prevents optimizations like vectorization, constant propagation, and loop unrolling. In framework orchestration code (pass pipelines, builder APIs), virtual call overhead is noise — measured in nanoseconds against millisecond-scale operations.

*Follow-up: How do you measure this?* — Profile with perf or VTune. Look at the "indirect branch misprediction" and "instruction cache miss" counters. If a virtual call site shows > 5% branch misprediction rate, it's worth devirtualizing. If it doesn't show up in the profile, leave it alone.

**Q: What is policy-based design and when does it beat Strategy?**
A: Policy-based design composes behavior via template parameters instead of virtual interfaces. The compiler inlines policy calls and can optimize across the boundary. It beats Strategy when the policy is known at compile time and performance matters — like choosing between SM90 and SM100 encoding in a batch encoder. Strategy (virtual dispatch) beats it when the policy must change at runtime or be selected from user input. Policies give you zero-cost abstraction; strategies give you runtime flexibility.

*Follow-up: What's the compile-time cost?* — Each unique combination of policies generates a separate class template instantiation. With N allocation policies × M encoding policies, you get N×M instantiations. This can explode compile times and binary size. Use explicit template instantiation in .cpp files to limit the damage.

---

## 5. Framework and API Design for Internal Expert Users

### 5.1 Designing for a Decade

Your framework will be extended by hundreds of engineers who didn't write it. They'll add new architectures, new engines, new opcodes. The framework's job is to make those additions *easy and safe*.

**Separating stable interface from implementation:**

```
┌──────────────────────────────────────┐
│         Public API (headers)          │  ← Changes require client recompile
│  ArchFactory, Instruction, CmdStream │
├──────────────────────────────────────┤
│       Extension points (headers)      │  ← New archs/opcodes implement these
│  InstrVisitor, RegAllocPolicy, Pass   │
├──────────────────────────────────────┤
│     Implementation (cpp files)        │  ← Can change freely
│  SM90Factory, PassPipeline internals  │
└──────────────────────────────────────┘
```

### 5.2 API vs ABI Compatibility

| Change | API break? | ABI break? |
|---|---|---|
| Add a new virtual method at the end | No | **Yes** — vtable layout changes |
| Add a private data member | No | **Yes** — sizeof changes |
| Change a function's default argument | No | No (resolved at compile time) |
| Remove a public method | **Yes** | **Yes** |
| Add a new overload | Usually no | No |
| Change an enum's underlying values | **Yes** | **Yes** |
| Add a new enum value at the end | Usually no | No |

**Rule:** Use pImpl for classes that cross library boundaries. Use abstract interfaces (pure virtual) for extension points. Never add virtual methods to a published interface — create a new interface (`InstructionV2`) or use the registry pattern.

### 5.3 Extension Points

```
Extension Mechanism         When to Use
──────────────────────────────────────────────────────────────
Static registry             New opcodes, architectures
dlopen/plugin               Optional features, late binding
Callbacks / std::function   One-off customization points
Policy templates            Hot-path behavior selection
Virtual method override     Deep behavioral customization
Data tables                 ISA descriptions, register maps
```

### 5.4 Table-Driven / Data-Driven Design

This is the "one place to change" principle. When a new architecture ships, ideally you edit *data*, not code:

```cpp
// ISA descriptor table — one row per opcode
struct OpcodeEntry {
    const char* mnemonic;
    uint16_t encoding;
    uint8_t numSrcs;
    uint8_t numDsts;
    uint8_t execUnit;    // ALU, LSU, SFU, etc.
    uint32_t latency;
    uint32_t flags;      // bitmask: canPredicate, hasSideEffects, etc.
};

// This table IS the ISA definition. Adding an opcode = adding a row.
constexpr OpcodeEntry sm90_isa[] = {
    {"IADD3",  0x0010, 2, 1, 0, 4, 0},
    {"IMAD",   0x0024, 3, 1, 0, 4, 0},
    {"LDG",    0x0980, 1, 1, 1, 200, 0x01}, // hasSideEffects
    {"STG",    0x0990, 2, 0, 1, 200, 0x01},
    {"BRA",    0x0947, 0, 0, 2, 1, 0x02},   // canPredicate
    // ... hundreds more
};

// All tool code reads from this table — no switch statements on opcodes:
class TableDrivenEncoder {
    const OpcodeEntry* table_;
    size_t tableSize_;
public:
    TableDrivenEncoder(const OpcodeEntry* t, size_t n)
        : table_(t), tableSize_(n) {}

    const OpcodeEntry* lookup(const std::string& mnemonic) const {
        for (size_t i = 0; i < tableSize_; ++i)
            if (mnemonic == table_[i].mnemonic)
                return &table_[i];
        return nullptr;
    }
};
```

### 5.5 Error Handling Models

| Model | Pros | Cons | Best for |
|---|---|---|---|
| Exceptions | Clean happy path, stack unwinding | Hidden control flow, perf cost, ABI issues | Framework initialization, user errors |
| Error codes | Explicit, C-compatible | Easy to ignore, clutters signatures | C APIs, kernel interfaces |
| `std::expected<T,E>` | Explicit, composable, no overhead | C++23, verbose without monadic ops | Internal framework functions |

**Recommendation for a GPU tool framework:** Use `expected` or error codes at the API boundary (where exceptions can't cross `.so` boundaries safely). Use exceptions for truly exceptional conditions (bug-level invariant violations). Provide error context — the message should say *what was expected, what was found, and what the user should do.*

### 5.6 Making the Framework Debuggable

```cpp
class Framework {
public:
    // Introspection
    void dumpRegisteredOpcodes(std::ostream& out) const;
    void dumpPassPipeline(std::ostream& out) const;
    void dumpArchConfig(std::ostream& out) const;

    // Dry-run mode — validates without executing
    void setDryRun(bool enable);

    // Determinism
    void setSeed(uint64_t seed); // same seed → same output, always

    // Verbose error messages
    // BAD:  throw std::runtime_error("invalid opcode");
    // GOOD: throw std::runtime_error(
    //   "Unknown opcode 'FADD.X' for arch SM90. "
    //   "Did you mean 'FADD'? Available opcodes: FADD, FMUL, FFMA. "
    //   "Check ISA table at gpu/isa/sm90.def");
};
```

### Interview Q&A — Framework Design

**Q: How do you design an API that hundreds of engineers will extend for years?**
A: I focus on three things. First, separate stable interfaces from implementation using abstract base classes and pImpl. Second, prefer extension via composition — registries, plugins, policy injection — over inheritance. Third, make the framework *debuggable*: good error messages that say what went wrong and what to do, introspection APIs to dump state, dry-run modes, and deterministic behavior from a seed. I'd rather ship a smaller API that's hard to misuse than a large one that's flexible but fragile.

*Follow-up: How do you deprecate an API method?* — Mark it `[[deprecated("Use newMethod() instead, removal in v4.0")]]`, log a warning on first use including the call site, keep it working for two release cycles, then remove. The key is the log message — engineers in a large org don't read deprecation notices in headers, but they do notice runtime warnings in their test output.

**Q: Why is table-driven design critical for GPU tools?**
A: Because ISAs change with every architecture generation. If your encoder has a switch statement with 2000 cases, adding an opcode requires modifying that switch, recompiling, and hoping you didn't break another case. With a table, adding an opcode is adding one row of data. The encoder reads the table and is generic. This is the "one place to change" principle — a new ISA is a new table file, not a code change.

*Follow-up: What if different opcodes need genuinely different encoding logic?* — The table entry includes a function pointer or an encoding-format enum. You have a small set of encoding formats (maybe 10), and the encoder switches on format, not on opcode. New opcodes reuse existing formats. Only truly novel encoding formats require code changes.

**Q: Exceptions vs error codes for a framework shared as a .so?**
A: Exceptions can't safely cross shared library boundaries when the library and client are compiled with different compilers or exception ABIs. So at the API boundary I use error codes or `expected<T, ErrorCode>`. Inside the library, I use exceptions for internal error handling — they don't escape. For truly fatal errors (invariant violations), I use `assert` or `std::terminate` because those indicate bugs, not user errors.

*Follow-up: What about `std::expected`?* — It's the best of both worlds: explicit like error codes, composable like exceptions, zero overhead. The only downside is it's C++23 and without monadic operations the code is verbose. I'd use it if the codebase is on C++23; otherwise, a custom `Result<T, E>` type.

**Q: How do you ensure a tool framework is deterministic?**
A: All randomness flows from a single seed passed through the context object — no `rand()`, no `time(NULL)`, no reading `/dev/urandom` except to generate the initial seed. Hash map iteration order is fixed (use ordered maps or sort after collection). Thread scheduling non-determinism is avoided by using deterministic parallel algorithms or single-threaded execution modes. The framework has a "replay" mode that re-reads the seed from a log and reproduces the exact same run.

*Follow-up: What about floating-point determinism?* — That's harder. I'd use `-ffp-contract=off` and avoid reassociation optimizations. If cross-platform bitwise reproducibility is required, I'd use fixed-point arithmetic or a software float library for the critical paths. For GPU stimulus generation, this usually means using integer arithmetic for address computation and reserving float only for the actual GPU operations being tested.

**Q: How would you handle a plugin architecture with dlopen?**
A: Each plugin is a shared library that exports a C function `registerPlugin(Registry&)`. At startup, the framework scans a plugin directory, `dlopen`s each library, `dlsym`s the registration function, and calls it. The plugin registers its factories, passes, or opcodes with the framework's registries. The C boundary avoids ABI issues. I'd version the plugin API with an integer — the registration function checks the framework version and refuses to load if incompatible. Plugins that fail to load produce a warning, not a crash.

*Follow-up: How do you handle plugin unloading?* — Generally, don't. `dlclose` invalidates all function pointers and vtable pointers from that library — if anything still references them, you get undefined behavior. Instead, load plugins at startup and keep them loaded for the process lifetime. If you must support hot-reload, use a process-restart approach.

---

## 6. SOLID Applied Concretely

### The BAD Design — A God Class

```cpp
// DON'T DO THIS — real code like this exists in legacy frameworks
class WorkloadGenerator {
    ArchType arch_;  // enum: SM80, SM90, SM100
    EngineType engine_; // enum: Graphics, Compute, Copy, Video
    RandomEngine rng_;

public:
    void generate() {
        switch (arch_) {
            case SM80:
                switch (engine_) {
                    case Graphics:
                        generateSM80Graphics(); break;
                    case Compute:
                        generateSM80Compute(); break;
                    // 4 engines × every arch = explosion
                }
                break;
            case SM90:
                switch (engine_) {
                    case Graphics:
                        generateSM90Graphics(); break;
                    // ...
                }
                break;
            // Adding SM100 means adding another case to EVERY switch
        }
    }

    // 20 private methods, one per arch×engine combination
    void generateSM80Graphics() { /* 500 lines */ }
    void generateSM80Compute() { /* 400 lines */ }
    void generateSM90Graphics() { /* 600 lines */ }
    // ...

    // Validation has the same switch-on-arch pattern
    bool validate() {
        switch (arch_) { /* ... same explosion ... */ }
    }
};
```

**Problems:** Adding SM100 requires modifying this file (violates OCP). Graphics and Compute share nothing but are in the same class. The class is 5000+ lines. Testing one arch requires building all of them.

### Step 1 — Single Responsibility (S)

Split by responsibility: generation, validation, encoding are separate concerns.

```cpp
class WorkloadGenerator {
    InstructionFactory& instrFactory_;
    RegAllocPolicy& regAlloc_;
    Validator& validator_;
public:
    GpuWorkload generate(const WorkloadSpec& spec);
};

class Validator {
public:
    virtual ~Validator() = default;
    virtual bool validate(const GpuWorkload& wl) = 0;
};

class Encoder {
public:
    virtual ~Encoder() = default;
    virtual std::vector<uint8_t> encode(const GpuWorkload& wl) = 0;
};
```

### Step 2 — Open/Closed (O)

Use the registry pattern so new architectures don't modify existing code:

```cpp
// New arch = new file, no edits to existing code
// sm100_factory.cpp:
REGISTER_ARCH("SM100", []() {
    return std::make_unique<SM100Factory>();
});
```

### Step 3 — Liskov Substitution (L)

**The violation that shows up in hardware modeling:**

```cpp
class GpuEngine {
public:
    virtual void submitGraphicsWork(const DrawCall& dc) = 0;
    virtual void submitComputeWork(const Dispatch& dp) = 0;
};

class CopyEngine : public GpuEngine {
public:
    void submitGraphicsWork(const DrawCall&) override {
        throw std::runtime_error("Copy engine can't do graphics!");
        // LSP VIOLATION — substituting CopyEngine for GpuEngine breaks callers
    }
    void submitComputeWork(const Dispatch&) override {
        throw std::runtime_error("Copy engine can't do compute!");
    }
};
```

**Fix — Interface Segregation (I) solves this:**

```cpp
class GraphicsCapable {
public:
    virtual ~GraphicsCapable() = default;
    virtual void submitGraphicsWork(const DrawCall& dc) = 0;
};

class ComputeCapable {
public:
    virtual ~ComputeCapable() = default;
    virtual void submitComputeWork(const Dispatch& dp) = 0;
};

class CopyCapable {
public:
    virtual ~CopyCapable() = default;
    virtual void submitCopy(const CopyOp& op) = 0;
};

// Graphics engine implements GraphicsCapable + ComputeCapable
class GraphicsEngine : public GraphicsCapable, public ComputeCapable {
public:
    void submitGraphicsWork(const DrawCall& dc) override { /* ... */ }
    void submitComputeWork(const Dispatch& dp) override { /* ... */ }
};

// Copy engine only implements CopyCapable — no lies
class CopyEngine : public CopyCapable {
public:
    void submitCopy(const CopyOp& op) override { /* ... */ }
};
```

### Step 4 — Dependency Inversion (D)

High-level policy (workload generation) doesn't depend on low-level details (SM90 encoding). Both depend on abstractions:

```cpp
// High-level module depends on abstractions, not concretions
class StimGenerator {
    InstructionFactory& factory_;  // abstraction
    Encoder& encoder_;             // abstraction
    RegAllocPolicy& alloc_;        // abstraction
public:
    StimGenerator(InstructionFactory& f, Encoder& e, RegAllocPolicy& a)
        : factory_(f), encoder_(e), alloc_(a) {}

    std::vector<uint8_t> run(const TestSpec& spec) {
        auto wl = buildWorkload(spec);
        return encoder_.encode(wl);
    }
};

// Assembly at the composition root — the only place that knows concrete types:
int main() {
    SM90Factory factory;
    SM90Encoder encoder;
    HazardAwareAlloc alloc;
    StimGenerator gen(factory, encoder, alloc);
    gen.run(spec);
}
```

### When NOT to Use Inheritance

```cpp
// BAD — inheritance for code reuse
class SM90Instruction : public SM80Instruction { /* override a few things */ };
// SM90 "is-a" SM80? No. They share encoding format but have different semantics.

// GOOD — composition
class SM90Instruction {
    EncodingFormat format_;  // shared via composition, not inheritance
    SM90Semantics semantics_;
};

// GOOD — free functions for shared logic
namespace encoding {
    uint32_t packOperands(int dst, int src0, int src1);
    // Shared by SM80 and SM90 without coupling them via inheritance
}

// GOOD — data-driven
// Both SM80 and SM90 use the same encoder with different tables
```

### Interview Q&A — SOLID

**Q: Show me a Liskov Substitution violation in GPU engine modeling.**
A: A common mistake is making all engines inherit from one `GpuEngine` base with methods for every capability: `submitGraphics()`, `submitCompute()`, `submitCopy()`. A copy engine inherits GpuEngine but throws on `submitGraphics()`. That violates LSP — calling code that takes `GpuEngine&` can't safely call any method without checking the type first, which defeats the point of polymorphism. The fix is interface segregation: `GraphicsCapable`, `ComputeCapable`, `CopyCapable` as separate interfaces. Each engine implements only the interfaces it supports.

*Follow-up: How do you discover capabilities at runtime?* — I'd use `dynamic_cast` or a capabilities query: `engine.supports(Capability::Graphics)`. Better yet, the factory only gives you the specific interface you asked for — you request a `GraphicsCapable*` and get null if the engine doesn't support it.

**Q: What's wrong with inheriting SM90Instruction from SM80Instruction?**
A: It couples SM90 to SM80's implementation details. If SM80's encoding format changes in a bugfix, SM90 breaks unexpectedly. It also lies about the relationship — SM90 is not a refinement of SM80, it's a different architecture that happens to share some encoding conventions. Use composition: both SM80 and SM90 instructions can contain an `EncodingFormat` component, or better yet, use a data-driven encoder that reads from architecture-specific tables.

*Follow-up: When IS inheritance appropriate in hardware modeling?* — For genuine specialization within one architecture. For example, `SM90ALUInstr` and `SM90MemInstr` inherit from `SM90Instr` because they genuinely are kinds of SM90 instructions with shared SM90 encoding format. But cross-architecture inheritance is almost always wrong.

**Q: Apply the Open/Closed Principle to adding a new opcode.**
A: With the registry pattern, I create a new `.cpp` file that defines the opcode's class and registers it. The framework's code is never modified — it's closed for modification. It's open for extension because the registry is the extension point. The static registration macro ensures the new opcode is available at runtime without any central file needing an update. The only build system change is adding the new file to the link.

*Follow-up: What if the new opcode needs a new encoding format that the encoder doesn't support?* — Then the encoder needs modification — OCP doesn't mean *never* modifying code, it means minimizing the blast radius. I'd add the new format to the table-driven encoder as a new row, which is a localized change. The registry pattern ensures that opcode addition is zero-edit; encoder format addition is small-edit.

**Q: How does Dependency Inversion apply to a stimulus generator?**
A: The generator depends on abstractions — `InstructionFactory`, `Encoder`, `RegAllocPolicy` — not on `SM90Factory` or `SM90Encoder`. This means I can test the generator with mock implementations, swap architectures without recompiling the generator, and add new architectures by implementing the existing interfaces. The concrete type wiring happens at the composition root — `main()` or a factory function — which is the only place that knows all the concrete types.

*Follow-up: Isn't this over-engineering for a small tool?* — For a script-sized tool, yes. But for a framework used by hundreds of engineers across multiple architecture generations, the cost of the abstractions is paid once and saves thousands of hours of "modify every file" churn over the framework's lifetime. The investment is proportional to the expected lifetime and user count.

**Q: Composition over inheritance — give a concrete GPU tools example.**
A: Instead of `class SM90Encoder : public SM80Encoder` (inheritance for reuse), I compose: `class Encoder { EncodingFormatTable table_; }`. Both SM80 and SM90 create an `Encoder` with their respective format tables. Shared logic lives in the `Encoder` class; per-architecture variation is in the data. This means changing SM80's table can't accidentally affect SM90, testing is independent, and adding SM100 is adding a table file, not a class hierarchy.

*Follow-up: What about protected methods in base classes?* — Protected methods create a contract between base and derived that's hard to change and hard to test. I prefer passing a helper object or free function that both classes can call. If I must use inheritance, I keep the base class abstract with no protected non-virtual methods.

---

## 7. Anti-Patterns in This Domain

### 7.1 Deep Inheritance Mirroring Hardware Taxonomy

```
Instruction → SM_Instruction → SM90_Instruction → SM90_ALU → SM90_IADD3
```

This 5-level hierarchy mirrors how a hardware architect thinks about the taxonomy, not how a software engineer should organize code. Every level adds coupling. Change the base, and five layers of overrides might break.

**Fix:** Flat hierarchy (one level of inheritance from a common base) + composition + data tables.

### 7.2 Switch-on-Type Sprawl

```cpp
// This pattern appears 47 times across the codebase:
if (auto* alu = dynamic_cast<ALUInstr*>(instr)) { ... }
else if (auto* mem = dynamic_cast<MemInstr*>(instr)) { ... }
else if (auto* br = dynamic_cast<BranchInstr*>(instr)) { ... }
// Miss one type → silent bug. Add a type → find all 47 sites.
```

**Fix:** Visitor, or `std::variant` with exhaustive `std::visit`.

### 7.3 Premature Abstraction

```cpp
// We might need to support Vulkan AND DX12 AND Metal AND our proprietary API!
class AbstractRenderAPIAbstractionFactoryInterface { ... };
// Reality: we only ever use one internal API. This abstraction cost
// engineering years and is now maintained by everyone, used by no one.
```

**Rule of three:** Don't abstract until you have three concrete cases.

### 7.4 Over-Templating

```cpp
template <typename Arch, typename Engine, typename AllocPolicy,
          typename EncPolicy, typename SchedPolicy, typename ValidPolicy,
          int MaxRegs, bool EnableProfiling>
class InstrGenerator { ... };
// Compile time: 45 minutes. Error message: 800 lines.
// The customer: "I just wanted to add a test."
```

**Fix:** Template the hot inner loop. Use virtual dispatch for everything else.

### 7.5 Hidden Global State Killing Determinism

```cpp
// Somewhere deep in a utility function:
static int counter = 0; // Invisible. Order-dependent. Non-deterministic under threads.
```

**Fix:** All mutable state in the context object. `grep -r "static.*=" src/` is your friend.

### 7.6 Leaky Abstractions Across Architecture Boundary

```cpp
class Instruction {
public:
    virtual uint32_t sm90ControlWord() const { return 0; } // SM90-specific!
    // Now every Instruction "knows" about SM90. SM100 adds sm100ControlWord().
    // The base class accumulates one method per architecture forever.
};
```

**Fix:** Architecture-specific data lives in architecture-specific types, accessed through the architecture factory or via a `getProperty<T>(key)` generic mechanism.

### Interview Q&A — Anti-Patterns

**Q: You're reviewing code that has a 6-level instruction class hierarchy. What's the problem?**
A: Deep hierarchies create fragile coupling — changing a method at level 2 can break levels 3 through 6. They also make it hard to combine capabilities across branches (an instruction that's both "predicated" and "memory" might need multiple inheritance). I'd flatten to at most 2 levels: an abstract `Instruction` interface and concrete types per architecture. Shared behavior goes into composition (helper classes) or free functions, not intermediate base classes.

*Follow-up: What if there's genuinely shared behavior?* — I'd use a composition approach: the shared behavior becomes a component that concrete instructions contain, not inherit from. For example, `PredicationHandler` is a member, not a base class.

**Q: How do you prevent switch-on-type sprawl?**
A: At code review, I flag any `dynamic_cast` chain or switch on a type enum. These should be replaced with Visitor (if the type set is stable and operations grow) or virtual methods (if operations are stable and types grow). The key is that adding a new type should produce a *compile error* at all sites that need updating, not a silent runtime fallthrough.

*Follow-up: What if there are only 3 types and they'll never change?* — Then a switch or `if constexpr` on a variant is fine. The anti-pattern is when the switch is repeated in 47 places and the type set has grown to 25 over five years.

**Q: What's the danger of over-templating in a framework?**
A: Three things: compile times that block iteration (45-minute builds kill productivity), error messages that are unreadable (a missing method in a template policy generates 800 lines of template instantiation backtrace), and binary size bloat from instantiating the same template with many policy combinations. The fix is to template only the performance-critical inner loop and use virtual dispatch or type erasure for the rest.

*Follow-up: How do you diagnose template bloat?* — Use `-ftime-trace` (Clang) or Templight to see which templates take the most compile time. Check binary size with `nm --size-sort`. If a template generates > 100KB of code per instantiation, it's a candidate for devirtualization.

**Q: You find a `static int counter` inside a utility function. Why is this bad for stimulus generation?**
A: It's hidden global state. Two tests running in sequence will see different counter values, making results depend on test ordering. Under threading, it's a data race. For deterministic stimulus generation, every piece of mutable state must flow through the context object with a known seed. I'd grep the codebase for `static.*=` (excluding `static const` and `static constexpr`) and eliminate or move each one.

*Follow-up: What about thread_local?* — `thread_local` avoids data races but still creates hidden state that varies with thread count and scheduling. For determinism, pass explicit per-thread contexts.

**Q: How do you spot a leaky abstraction in a GPU tools codebase?**
A: When a base class has methods named after specific architectures (`sm90ControlWord()`) or when "general" functions take architecture-specific parameters that are ignored for other architectures. The fix is to move architecture-specific behavior into the architecture's own types and access it through the architecture factory. The base class should have only operations that are meaningful for *every* architecture.

*Follow-up: What about a generic property bag approach?* — Something like `instr.getProperty<uint32_t>("controlWord")` works but loses type safety and discoverability. I prefer a middle ground: the base class has the universal operations, and you `static_cast` or query for an architecture-specific interface when you need arch-specific data. The caller must know it's working with SM90 — which it usually does because it got the instruction from an SM90 factory.

---

## Design Exercise Walkthroughs

### Exercise A: Instruction Set Class Model for Multiple Architectures

**Requirements questions I'd ask:**

1. How many architectures? How different are their instruction formats?
2. Do instructions need to be mixed across architectures in one program?
3. Is the instruction set stable within one arch generation or does it evolve?
4. What operations do we need: encoding, decoding, disassembly, analysis, simulation?
5. What's the hot path — encoding millions of instructions or one-off analysis?

**Design:**

```
                        ┌────────────────┐
                        │  <<interface>>  │
                        │  Instruction    │
                        │────────────────│
                        │+ encode()      │
                        │+ mnemonic()    │
                        │+ operands()    │
                        │+ accept(Vis&)  │
                        └───────┬────────┘
                                │
              ┌─────────────────┼─────────────────┐
              │                 │                  │
     ┌────────┴───────┐  ┌─────┴──────┐  ┌───────┴────────┐
     │ SM90Instruction│  │SM100Instr  │  │ SM110Instr     │
     │────────────────│  │────────────│  │────────────────│
     │ OpcodeDesc* d  │  │ OpcodeD* d │  │ OpcodeDesc* d  │
     │ Operand ops[4] │  │ Operand[]  │  │ Operand ops[]  │
     └────────────────┘  └────────────┘  └────────────────┘

     Each arch reads its OpcodeDescriptor from a data table.
     Visitor handles cross-cutting operations.
     Factory+Registry creates the right type.
```

```cpp
// Core instruction — one class per architecture, driven by data tables
class SM90Instruction : public Instruction {
    const OpcodeDescriptor* desc_; // flyweight — from ISA table
    std::array<Operand, 4> ops_;
public:
    SM90Instruction(const OpcodeDescriptor* desc, std::array<Operand, 4> ops)
        : desc_(desc), ops_(ops) {}

    void encode(uint8_t* buf) const override {
        // Generic encoding driven by desc_->format
        SM90Encoding::encode(buf, *desc_, ops_);
    }

    std::string mnemonic() const override { return desc_->mnemonic; }

    const std::array<Operand, 4>& operands() const override { return ops_; }

    void accept(InstrVisitor& v) override { v.visitSM90(*this); }
};
```

**Alternatives considered:**

1. *One class per opcode* — doesn't scale to 2000+ opcodes. Use data tables instead.
2. *Single class + variant for arch-specific payload* — works but loses arch-specific type checking.
3. *std::variant<SM90Instr, SM100Instr>* — works if arch is known at compile time; doesn't allow runtime arch selection.

**Tradeoff:** The data-table approach (flyweight descriptors + generic encoder) means adding an opcode is a data change. Adding an *architecture* is a code change (new Instruction subclass, new table, new factory) but follows the registry pattern so it's additive.

### Exercise B: Plugin Architecture for New GPU Engines

**Requirements:**

- Adding a new engine (e.g., "Optical Flow") should require zero changes to core code.
- Engines have different capabilities (graphics, compute, copy, video).
- Each engine has its own command format and method table.

**Design:**

```
┌───────────────────────────────────────────────────────┐
│                   EngineRegistry                       │
│  map<string, EngineFactory>                            │
│  register(name, factory)                               │
│  create(name) → unique_ptr<Engine>                     │
└───────────────────────────┬───────────────────────────┘
                            │ creates
                 ┌──────────┴──────────┐
                 │    <<interface>>     │
                 │      Engine         │
                 │─────────────────────│
                 │+ name() : string    │
                 │+ caps() : CapSet    │
                 │+ createStream()     │
                 │+ methodTable()      │
                 └─────────┬───────────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
   ┌──────┴──────┐  ┌─────┴──────┐  ┌──────┴────────┐
   │GraphicsEng  │  │ ComputeEng │  │ OptFlowEng    │
   │             │  │            │  │ (plugin .so)  │
   └─────────────┘  └────────────┘  └───────────────┘
```

```cpp
// --- engine_plugin.h (stable API) ---
class Engine {
public:
    virtual ~Engine() = default;
    virtual std::string name() const = 0;
    virtual CapabilitySet capabilities() const = 0;
    virtual std::unique_ptr<CmdStream> createStream() = 0;
    virtual const MethodTable& methodTable() const = 0;
};

// --- Plugin entry point (C linkage for ABI safety) ---
// optical_flow_engine.cpp — compiled as optical_flow.so
extern "C" void registerPlugin(EngineRegistry& registry) {
    registry.registerEngine("OpticalFlow", []() {
        return std::make_unique<OpticalFlowEngine>();
    });
}

// --- Framework loading ---
class PluginLoader {
public:
    void loadDirectory(const std::string& dir, EngineRegistry& registry) {
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".so") {
                void* handle = dlopen(entry.path().c_str(), RTLD_NOW);
                if (!handle) {
                    log_.warn("Failed to load {}: {}", entry.path(), dlerror());
                    continue;
                }
                auto regFn = (void(*)(EngineRegistry&))dlsym(handle, "registerPlugin");
                if (!regFn) {
                    log_.warn("No registerPlugin in {}", entry.path());
                    continue;
                }
                regFn(registry);
                log_.info("Loaded engine plugin: {}", entry.path());
            }
        }
    }
};
```

**Key decisions:**

- C linkage at the plugin boundary avoids C++ ABI issues.
- Engine interface is abstract — plugins implement it freely.
- Capabilities are queried, not assumed — no Liskov violations.
- The framework warns on failed loads instead of crashing.

### Exercise C: IR + Pass Pipeline for a Workload Generator

**Requirements:**

- Represent a GPU workload as an IR (instructions, blocks, barriers).
- Transform the IR through passes: randomization, scheduling, register allocation, encoding.
- Passes should be composable, orderable, and individually testable.

**Design:**

```
┌─────────────┐    ┌──────────────┐    ┌──────────────────────────────┐
│ WorkloadSpec │───>│  IR Builder  │───>│  IRProgram                   │
│ (input)      │    │              │    │  blocks: [IRBlock]           │
└─────────────┘    └──────────────┘    │  block: [IRInstr]            │
                                       └───────────────┬──────────────┘
                                                       │
                                                       ▼
                                        ┌──────────────────────────────┐
                                        │       PassPipeline           │
                                        │  [Randomize]                 │
                                        │  [RegAlloc]                  │
                                        │  [Schedule]                  │
                                        │  [InsertBarriers]            │
                                        │  [Encode]                    │
                                        └───────────────┬──────────────┘
                                                        │
                                                        ▼
                                                 ┌──────────────┐
                                                 │ Binary output │
                                                 └──────────────┘
```

```cpp
// --- IR ---
struct IRBlock {
    std::string label;
    std::vector<std::unique_ptr<IRInstr>> instrs;
};

struct IRProgram {
    std::vector<IRBlock> blocks;
    std::vector<std::string> errors;
    bool hasErrors() const { return !errors.empty(); }

    void dump(std::ostream& out) const {
        for (auto& block : blocks) {
            out << block.label << ":\n";
            Disassembler dis(out);
            for (auto& instr : block.instrs)
                instr->accept(dis);
        }
    }
};

// --- Pass interface ---
class IRPass {
public:
    virtual ~IRPass() = default;
    virtual std::string name() const = 0;
    virtual void run(IRProgram& program) = 0;
};

// --- Concrete passes ---
class RandomizationPass : public IRPass {
    RandomEngine& rng_;
    RandomizationPolicy& policy_;
public:
    RandomizationPass(RandomEngine& rng, RandomizationPolicy& pol)
        : rng_(rng), policy_(pol) {}

    std::string name() const override { return "Randomize"; }

    void run(IRProgram& program) override {
        for (auto& block : program.blocks)
            for (auto& instr : block.instrs)
                policy_.randomize(*instr, rng_);
    }
};

class RegisterAllocationPass : public IRPass {
    RegAllocPolicy& alloc_;
public:
    explicit RegisterAllocationPass(RegAllocPolicy& a) : alloc_(a) {}
    std::string name() const override { return "RegAlloc"; }
    void run(IRProgram& program) override {
        RegisterState state;
        for (auto& block : program.blocks)
            for (auto& instr : block.instrs)
                alloc_.allocate(*instr, state);
    }
};

// --- Pipeline construction ---
PassPipeline buildStandardPipeline(ToolContext& ctx) {
    PassPipeline pipeline(ctx.logger);
    pipeline.addPass(std::make_unique<RandomizationPass>(ctx.rng, ctx.randPolicy));
    pipeline.addPass(std::make_unique<RegisterAllocationPass>(ctx.regAlloc));
    pipeline.addPass(std::make_unique<SchedulingPass>(ctx.arch));
    pipeline.addPass(std::make_unique<BarrierInsertionPass>(ctx.arch));
    pipeline.addPass(std::make_unique<EncodingPass>(ctx.encoder));
    return pipeline;
}
```

**Tradeoffs:**

- *Visitor per pass vs. modifying IR in place:* I chose in-place mutation because passes transform the IR sequentially, and copying the entire IR between passes is wasteful. The Visitor is used *within* a pass for traversal.
- *Pass ordering:* The pipeline enforces order. A future improvement would be a dependency graph between passes (like LLVM's PassManager).
- *Error handling:* Passes append to `program.errors`. The pipeline checks after each pass and stops early. This gives clear error attribution.

---

## Red Flags

These should raise your eyebrow in an interview or code review:

| Red Flag | Why | Fix |
|---|---|---|
| God class > 1000 lines | Untestable, unmaintainable | Split by responsibility |
| `dynamic_cast` chains | Fragile, non-exhaustive | Visitor or variant |
| `switch(archType)` in > 3 places | Adding an arch = editing N files | Registry + factory |
| 6-level inheritance hierarchy | Fragile base class problem | Flatten + composition |
| `static` mutable variables | Non-determinism, thread-unsafe | Context object |
| Architecture name in base class | Leaky abstraction | Arch-specific subtype |
| Template with > 4 parameters | Compile-time explosion | Virtual dispatch for non-hot paths |
| `new` without `unique_ptr` | Memory leak risk | Smart pointers always |
| Public data members in API | Can never change layout | Accessors + pImpl |
| No error messages, just `throw` | Undebuggable framework | Contextual error strings |
| Comments saying "// TODO: fix this" with dates from 5 years ago | Technical debt | Fix or delete |

---

## Whiteboard Checklist

Use this checklist when designing a system in an interview:

```
□ What are the extension points? (new arch, new engine, new opcode)
□ Can I add one without editing existing files? (OCP, registry)
□ Where is the mutable state? (context object, not globals)
□ Is the design testable? (dependency injection, no singletons)
□ Is it deterministic? (seeded RNG, ordered iteration)
□ Interface vs inheritance — am I inheriting for reuse? (composition instead)
□ What's in the hot path? (virtual dispatch OK? need CRTP/variant?)
□ What crosses the ABI boundary? (pImpl, C linkage for plugins)
□ How does a new engineer add a feature? (docs, examples, error messages)
□ How does the system fail? (error handling model, validation, dry-run)
□ What's the data that changes per generation? (tables, not code)
□ Am I abstracting too early? (rule of three)
□ Can I draw the ownership graph? (unique_ptr = ownership, raw ptr = borrowing)
□ What does the Visitor/variant tradeoff look like for my node types?
□ How do I deprecate this API in 3 years without breaking 200 users?
```

---

*End of Module M2. Next: M3 — Concurrency, Memory Models, and GPU Architecture Fundamentals.*
