# Module M1: Modern C++ Mastery

## Why This Matters for This Role

You're interviewing for **System Software Engineer, GPU Development Tools** at NVIDIA. That means you'll be building compilers, assemblers, debuggers, profilers, or driver-level tooling that targets GPU hardware. The code you write will:

- Parse and emit binary instruction streams where a single aliasing violation or alignment fault is a silent corruption bug that ships to millions of GPUs.
- Run in performance-critical pipelines where a stray allocation in a hot loop means your shader compiler misses its time budget.
- Maintain ABI stability across driver versions so that tools built against one SDK work with the next.
- Use lock-free data structures and concurrent pipelines because modern GPUs have thousands of warps and the host-side tooling must keep up.

Every topic in this module maps directly to something you will do on the job. This isn't academic — it's the stuff that separates "knows C++" from "can ship production C++ in a systems team."

---

## 1. Value Semantics, Copy/Move, and Lifetimes

### The Mental Model

In C++, objects *are* values by default. When you write `Widget w2 = w1;`, you get an independent copy — not a reference to the same thing. This is **value semantics**, and it's the foundation of how C++ manages resources.

The compiler generates up to five special member functions for you: default constructor, destructor, copy constructor, copy assignment, move constructor, move assignment. The rules about when you need to write them yourself are called the **Rule of 0/3/5**.

### Rule of 0/3/5

**Rule of 0**: If your class only holds types that already manage themselves (like `std::string`, `std::vector`, `std::unique_ptr`), don't write any special members. The compiler-generated ones do exactly the right thing.

```cpp
// Rule of 0 — this is the ideal
struct ShaderModule {
    std::string name;
    std::vector<uint32_t> spirv_bytecode;
    std::unique_ptr<DebugInfo> debug_info;
    // No destructor, no copy/move — compiler does it all
};
```

**Rule of 3**: If you write a destructor, you almost certainly need a copy constructor and copy assignment operator too, because the default ones will do a shallow copy of whatever resource you're manually managing.

**Rule of 5**: In modern C++ (C++11+), if you write any of those three, you should also write a move constructor and move assignment operator, because the compiler won't generate them for you once you've declared any of the big three.

```cpp
// Rule of 5 — owning a raw resource
class InstructionBuffer {
    uint32_t* data_;
    size_t size_;
public:
    // Constructor
    explicit InstructionBuffer(size_t n)
        : data_(new uint32_t[n]), size_(n) {}

    // Destructor
    ~InstructionBuffer() { delete[] data_; }

    // Copy constructor
    InstructionBuffer(const InstructionBuffer& other)
        : data_(new uint32_t[other.size_]), size_(other.size_) {
        std::memcpy(data_, other.data_, size_ * sizeof(uint32_t));
    }

    // Copy assignment (copy-and-swap idiom)
    InstructionBuffer& operator=(InstructionBuffer other) {
        swap(*this, other);
        return *this;
    }

    // Move constructor
    InstructionBuffer(InstructionBuffer&& other) noexcept
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    // Move assignment
    InstructionBuffer& operator=(InstructionBuffer&& other) noexcept {
        if (this != &other) {
            delete[] data_;
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    friend void swap(InstructionBuffer& a, InstructionBuffer& b) noexcept {
        using std::swap;
        swap(a.data_, b.data_);
        swap(a.size_, b.size_);
    }
};
```

### std::move vs std::forward

`std::move` doesn't move anything. It's a **cast** — it casts its argument to an rvalue reference so that overload resolution picks the move constructor/assignment instead of the copy one.

```cpp
std::string name = "kernel_launch";
std::string other = std::move(name);  // name is now in a valid-but-unspecified state
```

`std::forward` is for **perfect forwarding** in template code. It preserves the value category (lvalue or rvalue) of the original argument.

```cpp
template<typename... Args>
auto make_instruction(Args&&... args) {
    return Instruction(std::forward<Args>(args)...);
}
```

### Reference Collapsing

When you have a template parameter `T&&` and `T` is deduced:
- If you pass an lvalue of type `X`, `T` = `X&`, and `T&&` = `X& &&` = `X&` (lvalue ref)
- If you pass an rvalue of type `X`, `T` = `X`, and `T&&` = `X&&` (rvalue ref)

The collapsing rules: `& &` → `&`, `& &&` → `&`, `&& &` → `&`, `&& &&` → `&&`. Anything with an `&` in it collapses to `&`.

### RVO/NRVO and Copy Elision

**RVO** (Return Value Optimization): When a function returns a temporary, the compiler constructs it directly in the caller's memory. Since C++17, this is **mandatory** — not an optimization, a guarantee.

```cpp
InstructionBuffer create_buffer() {
    return InstructionBuffer(1024);  // Mandatory elision in C++17
}
```

**NRVO** (Named RVO): When returning a named local variable, the compiler *may* elide the copy/move. Not mandatory, but every major compiler does it.

```cpp
InstructionBuffer create_buffer(size_t n) {
    InstructionBuffer buf(n);
    // ... fill buf ...
    return buf;  // NRVO — likely elided, but not guaranteed
}
```

**Don't `std::move` a return value** — it actually *prevents* NRVO:

```cpp
// BAD — prevents NRVO!
return std::move(buf);
```

### Dangling References and Lifetime Extension

A temporary bound to a `const&` or `&&` local variable has its lifetime extended to the scope of that variable:

```cpp
const std::string& ref = make_name();  // lifetime extended — OK
```

But this does NOT work through function return or through a member:

```cpp
// DANGLING — no lifetime extension through function return
const std::string& bad() {
    return std::string("oops");
}

// DANGLING — no lifetime extension through members
struct Holder {
    const std::string& ref;
};
Holder h{ std::string("oops") };  // dangling!
```

### Interview Q&A

**Q: What's the difference between std::move and std::forward?**
A: `std::move` unconditionally casts to an rvalue reference — you use it when you know you want to move from an object. `std::forward` conditionally casts based on the deduced template parameter — it preserves whether the original argument was an lvalue or rvalue. You use `std::forward` in forwarding references (`T&&` where `T` is deduced) to pass arguments through without losing their value category. Using `std::move` in a forwarding context would incorrectly move from lvalue arguments.
*Follow-up:* What happens if you `std::move` a const object? — You get a `const T&&`, which won't bind to a move constructor (which takes `T&&`), so it silently falls back to the copy constructor. No error, no warning — just a performance surprise.

**Q: Explain the Rule of 0. When would you deviate from it?**
A: Rule of 0 says if your class only holds self-managing types like smart pointers and standard containers, don't write any special members — the compiler-generated ones compose correctly. You deviate when you're wrapping a raw resource like a file descriptor, a CUDA handle, or raw memory from a custom allocator. In GPU tools, you might wrap a `CUmodule` handle that needs `cuModuleUnload` in the destructor — that's when you move to Rule of 5. Even then, the preferred pattern is to write one small RAII wrapper (Rule of 5) and then compose it into larger classes (Rule of 0).
*Follow-up:* Why is Rule of 5 not Rule of 6 (including default constructor)? — The default constructor is not tied to resource management. The five special members are specifically about lifecycle: how to destroy, copy, and move. A default constructor is about providing a valid default state, which is a separate concern.

**Q: When does NRVO fail?**
A: NRVO can fail when you return different named variables from different branches, when the return type doesn't match the local variable type, or when you explicitly `std::move` the return value. It also fails if the object is a function parameter (not a local). In those cases, the compiler falls back to move construction (if available) or copy construction. This matters in tool pipelines where you're returning large IR buffers — you want to structure your code so NRVO can fire.
*Follow-up:* Can copy elision fire even when the copy/move constructor has side effects? — Yes. Mandatory copy elision (C++17, prvalues) happens even if the constructor has side effects — the side effects are simply not executed. NRVO (named, non-mandatory) is allowed to skip side effects too, which is why you shouldn't rely on side effects in copy/move constructors.

**Q: What's a dangling reference and how do you prevent them in practice?**
A: A dangling reference points to memory that's been reclaimed — typically a destroyed temporary or a stack frame that's been unwound. The classic case is returning a reference to a local variable. You prevent them by: (1) returning by value, (2) using smart pointers for heap objects, (3) being careful with `string_view` and `span` which are non-owning, and (4) never storing references to temporaries beyond the current expression. In GPU tool code, this comes up a lot with string tables and symbol references that outlive the module they came from.
*Follow-up:* Does `std::string_view` extend the lifetime of a temporary `std::string`? — No. `string_view` is a non-owning view, and the standard provides no lifetime extension for it. If you write `std::string_view sv = get_name();` where `get_name()` returns `std::string` by value, the temporary is destroyed at the semicolon and `sv` dangles.

**Q: Explain perfect forwarding and why it needs both `T&&` and `std::forward<T>`.**
A: Perfect forwarding lets a wrapper function pass arguments to an inner function without changing their value category. You need `T&&` (a forwarding reference, not an rvalue reference — the difference is that `T` is deduced) so that reference collapsing preserves lvalue-ness. And you need `std::forward<T>` because inside the function, a named rvalue reference is an lvalue — `std::forward` re-applies the rvalue-ness if and only if the original argument was an rvalue. Without `std::forward`, you'd always copy. Without `T&&`, you couldn't accept both lvalues and rvalues in the same overload.
*Follow-up:* What's the difference between a forwarding reference and an rvalue reference? — Syntactically they look the same (`T&&`), but a forwarding reference requires `T` to be a deduced template parameter. `void f(Widget&&)` is an rvalue reference — it only binds to rvalues. `template<typename T> void f(T&&)` is a forwarding reference — it binds to anything. `auto&&` is also a forwarding reference.

---

## 2. Templates

### Function and Class Templates

Templates are compile-time code generation. The compiler generates a new version of the function or class for each unique set of template arguments.

```cpp
// Function template
template<typename T>
T saturate_add(T a, T b) {
    T result = a + b;
    if (result < a) return std::numeric_limits<T>::max();  // overflow
    return result;
}

// Class template
template<typename Word, size_t N>
class RegisterFile {
    std::array<Word, N> regs_{};
public:
    Word& operator[](size_t i) { return regs_[i]; }
    const Word& operator[](size_t i) const { return regs_[i]; }
};
```

### Full and Partial Specialization

**Full specialization**: provide a concrete implementation for specific types.

```cpp
template<>
class RegisterFile<bool, 8> {
    uint8_t bits_ = 0;
public:
    // Pack 8 bools into a single byte
    bool get(size_t i) const { return (bits_ >> i) & 1; }
    void set(size_t i, bool v) { bits_ = (bits_ & ~(1 << i)) | (v << i); }
};
```

**Partial specialization** (class templates only): specialize on a pattern.

```cpp
// Specialize for pointer types
template<typename T, size_t N>
class RegisterFile<T*, N> {
    std::array<T*, N> regs_{};
public:
    T* operator[](size_t i) { return regs_[i]; }
    // Adds null-check behavior, etc.
};
```

### SFINAE and std::enable_if

**SFINAE** = Substitution Failure Is Not An Error. When the compiler tries to substitute template arguments and the result is ill-formed, that overload is silently removed from the candidate set instead of causing a compilation error.

```cpp
// Only enable for unsigned integer types
template<typename T>
typename std::enable_if<std::is_unsigned<T>::value, T>::type
decode_field(const uint8_t* stream, int bit_offset, int width) {
    // Bit extraction logic for instruction decoding
    T mask = (T(1) << width) - 1;
    // ... shift and mask from stream ...
    return /* extracted value */;
}
```

### Variadic Templates and Parameter Packs

```cpp
// Recursive approach (pre-C++17)
template<typename T>
void emit_fields(std::ostream& os, T&& val) {
    os << std::forward<T>(val);
}

template<typename T, typename... Rest>
void emit_fields(std::ostream& os, T&& val, Rest&&... rest) {
    os << std::forward<T>(val) << ", ";
    emit_fields(os, std::forward<Rest>(rest)...);
}

// Usage: emit_fields(std::cout, "opcode", 0x3F, "dst", reg);
```

### Fold Expressions (C++17)

```cpp
// Much cleaner
template<typename... Args>
auto sum(Args... args) {
    return (args + ...);  // unary right fold
}

// Comma fold for side effects
template<typename... Args>
void print_all(Args&&... args) {
    (std::cout << ... << args) << '\n';  // binary left fold
}

// Check if all args satisfy a predicate
template<typename... Ts>
constexpr bool all_integral() {
    return (std::is_integral_v<Ts> && ...);
}
```

### CRTP (Curiously Recurring Template Pattern)

Static polymorphism — no vtable, no virtual dispatch overhead:

```cpp
template<typename Derived>
class InstructionEncoder {
public:
    void encode(uint32_t* out) {
        // Common preamble
        static_cast<Derived*>(this)->encode_opcode(out);
        static_cast<Derived*>(this)->encode_operands(out);
        // Common postamble
    }
};

class ALUEncoder : public InstructionEncoder<ALUEncoder> {
    friend class InstructionEncoder<ALUEncoder>;
    void encode_opcode(uint32_t* out) { /* ALU-specific */ }
    void encode_operands(uint32_t* out) { /* ALU-specific */ }
};
```

This is huge in GPU tools — you get polymorphic behavior for instruction encoding without the cost of virtual dispatch in a hot loop that processes millions of instructions.

### Type Traits and Tag Dispatch

```cpp
// Tag dispatch — choose algorithm at compile time
struct scalar_tag {};
struct vector_tag {};

template<typename T>
struct operand_category { using type = scalar_tag; };

template<>
struct operand_category<float4> { using type = vector_tag; };

template<typename T>
void emit_operand(T val, scalar_tag) {
    // Emit as scalar register
}

template<typename T>
void emit_operand(T val, vector_tag) {
    // Emit as vector register
}

template<typename T>
void emit_operand(T val) {
    emit_operand(val, typename operand_category<T>::type{});
}
```

### C++20 Concepts (Brief Note)

Concepts replace SFINAE with readable, first-class constraints:

```cpp
template<typename T>
concept Encodable = requires(T t, uint32_t* buf) {
    { t.encode(buf) } -> std::same_as<size_t>;
    { t.opcode() } -> std::convertible_to<uint32_t>;
};

template<Encodable T>
void emit(T& instr, uint32_t* buf) {
    instr.encode(buf);
}
```

If your team is on C++20, concepts are strictly better than SFINAE for constraining templates. Know both — legacy codebases will have SFINAE everywhere.

### Interview Q&A

**Q: What is SFINAE and why does it matter?**
A: SFINAE means that when the compiler substitutes template arguments and the resulting type is ill-formed, that overload is silently discarded rather than causing a hard error. This lets you write multiple overloads that are selectively enabled based on type properties. It matters because it's how pre-C++20 code does compile-time dispatch — for example, choosing different instruction encoding paths based on whether an operand type is integral, floating-point, or a predicate register. The downside is that SFINAE error messages are terrible, which is why C++20 concepts were invented.
*Follow-up:* Can SFINAE work in the function body, or only in the signature? — Only in the "immediate context" — the signature, template parameter list, and return type. An error inside the function body is a hard error, not SFINAE. That's why you see `enable_if` in the return type or as a defaulted template parameter, never inside the function.

**Q: Explain CRTP and where you'd use it in GPU tooling.**
A: CRTP is when a class inherits from a template parameterized on itself: `class Derived : public Base<Derived>`. The base class can call derived class methods via `static_cast<Derived*>(this)` — giving you compile-time polymorphism with zero overhead. In GPU tools, you'd use it for instruction visitors or encoders where you have dozens of instruction types and you process millions of them in a compilation pass. Virtual dispatch adds an indirect branch per instruction, which trashes the branch predictor. CRTP gives you the extensibility of polymorphism with the performance of monomorphic code.
*Follow-up:* What's the main limitation of CRTP vs virtual dispatch? — You lose runtime polymorphism. You can't store different CRTP-derived objects in a single container and dispatch at runtime. If you need that, you're back to vtables (or `std::variant` with `std::visit`).

**Q: What's a fold expression and when would you use one?**
A: A fold expression (C++17) applies a binary operator across a parameter pack. `(args + ...)` sums all args; `(args && ...)` ANDs them. I'd use it for things like combining hash values of multiple fields, checking that all register indices are in range, or logging a variable number of diagnostic values. Before fold expressions, you needed recursive template instantiation which was more code and harder to read.
*Follow-up:* What's the difference between a unary left fold and a unary right fold? — `(... op args)` is left: `((a1 op a2) op a3)`. `(args op ...)` is right: `(a1 op (a2 op a3))`. For associative operators like `+` it doesn't matter, but for `-` or `/` the result differs. There are also binary folds that include an init value.

**Q: What's the difference between full and partial specialization?**
A: Full specialization provides an implementation for one specific set of template arguments — like `template<> class Foo<int, 4>`. Partial specialization fixes some arguments or constrains them by pattern — like `template<typename T> class Foo<T*, 4>`. Function templates only support full specialization, not partial. If you need partial specialization behavior for functions, use overloading or a class template with a static method. This distinction matters when writing type-trait-like utilities for instruction operand classification.
*Follow-up:* Why can't function templates be partially specialized? — It was a deliberate language design choice. Partial specialization of functions would interact badly with overload resolution — the compiler wouldn't know whether to treat something as a specialization or an overload. The workaround is to delegate to a class template or use tag dispatch/SFINAE.

**Q: How do variadic templates help in a real GPU tool codebase?**
A: Variadic templates let you write functions and classes that accept any number of arguments with full type safety. In GPU tools, you'd use them for: type-safe printf-style diagnostics (replacing `va_args`), instruction builder APIs that take a variable number of operands, compile-time tables of register classes, and factory functions that perfectly forward constructor arguments. The key advantage over C-style variadic functions is that you get compile-time type checking and you can operate on each argument's type individually. Combined with fold expressions, they make metaprogramming much more approachable.
*Follow-up:* How do you get the number of elements in a parameter pack? — `sizeof...(Args)` returns the count at compile time. It does NOT evaluate the arguments — it just counts them.

---

## 3. Memory Management

### Smart Pointers

```
  unique_ptr<T>              shared_ptr<T>
  ┌──────────┐               ┌──────────┐
  │ T* ptr   │               │ T* ptr   │──────────┐
  └────┬─────┘               │ ctrl*    │──┐       │
       │                     └──────────┘  │       │
       ▼                                   ▼       ▼
  ┌──────────┐               ┌──────────┐ ┌──────┐
  │  Object  │               │ ctrl blk │ │Object│
  └──────────┘               │ strong: 2│ └──────┘
                              │ weak:   1│
                              │ deleter  │
                              │ allocator│
                              └──────────┘
```

### unique_ptr

Exclusive ownership — zero overhead over a raw pointer.

```cpp
// Basic usage
auto module = std::make_unique<ShaderModule>("vertex.glsl");

// Custom deleter — essential for wrapping C APIs
struct CudaModuleDeleter {
    void operator()(CUmodule* m) const {
        cuModuleUnload(*m);
        delete m;
    }
};
using CudaModulePtr = std::unique_ptr<CUmodule, CudaModuleDeleter>;

// unique_ptr for arrays
auto buffer = std::make_unique<uint32_t[]>(1024);
```

### shared_ptr and the Control Block

`shared_ptr` uses reference counting. The control block stores the strong count, weak count, deleter, and (with `make_shared`) the object itself.

```cpp
// make_shared: ONE allocation (control block + object together)
auto node = std::make_shared<IRNode>(Opcode::ADD, dst, src0, src1);

// new + shared_ptr: TWO allocations
std::shared_ptr<IRNode> node2(new IRNode(Opcode::MUL, dst, src0, src1));
```

**Why `make_shared` is better**: one allocation instead of two, better cache locality, exception safe. **Downside**: the object memory can't be freed until all `weak_ptr`s are gone too, because the object lives inside the control block.

### weak_ptr and Cyclic References

```cpp
// Classic cycle: parent ↔ child
struct IRNode {
    std::shared_ptr<IRNode> child;   // owns child
    std::weak_ptr<IRNode> parent;    // does NOT own parent — breaks cycle
};
```

`weak_ptr::lock()` returns a `shared_ptr` if the object is still alive, or an empty `shared_ptr` if it's been destroyed.

### Placement New and Custom Allocators

When you're building a compiler that processes millions of IR nodes, you don't want millions of individual `malloc` calls. You want an **arena allocator**.

```cpp
// Placement new — construct an object in pre-allocated memory
alignas(IRNode) char buffer[sizeof(IRNode)];
IRNode* node = new (buffer) IRNode(Opcode::ADD);
// Must manually call destructor:
node->~IRNode();
```

### Arena/Pool/Bump Allocation

```cpp
// Simple bump allocator — allocate linearly, free everything at once
class Arena {
    char* base_;
    char* current_;
    char* end_;
public:
    explicit Arena(size_t size)
        : base_(static_cast<char*>(std::malloc(size)))
        , current_(base_)
        , end_(base_ + size) {}

    ~Arena() { std::free(base_); }

    template<typename T, typename... Args>
    T* alloc(Args&&... args) {
        // Align
        size_t space = end_ - current_;
        void* ptr = current_;
        if (!std::align(alignof(T), sizeof(T), ptr, space))
            throw std::bad_alloc();
        current_ = static_cast<char*>(ptr) + sizeof(T);
        return new (ptr) T(std::forward<Args>(args)...);
    }

    void reset() { current_ = base_; }  // "free" everything
};
```

**Why GPU tool frameworks need this**: A shader compiler pass creates millions of IR nodes, processes them, and then discards them all. Arena allocation makes this nearly free — allocation is a pointer bump, deallocation is a single `reset()`. Individual `new`/`delete` would fragment the heap and blow the allocator's mutex contention in multithreaded builds.

### Alignment

```cpp
// alignas — request specific alignment
struct alignas(64) CacheLine {
    uint32_t data[16];
};

// alignof — query alignment requirement
static_assert(alignof(CacheLine) == 64);

// Over-aligned allocation (C++17)
auto* p = new (std::align_val_t(64)) CacheLine;
```

Alignment matters for GPU instruction buffers that need to be DMA'd to device memory with specific alignment constraints.

### Interview Q&A

**Q: What's the difference between `make_shared` and using `new` with `shared_ptr`?**
A: `make_shared` does a single allocation that holds both the control block and the object. `shared_ptr<T>(new T(...))` does two separate allocations — one for the object and one for the control block. The single allocation is faster, more cache-friendly, and exception-safe (there's no window where the `new` succeeded but the `shared_ptr` construction didn't). The downside of `make_shared` is that the object's memory isn't freed until all weak references are also gone, because the object and control block share the same allocation. If you have large objects with long-lived weak_ptrs, this can be a problem.
*Follow-up:* When would you NOT use `make_shared`? — When you need a custom deleter (it doesn't support that), when the object is very large and you have long-lived `weak_ptr`s (memory stays allocated too long), or when you need to call a non-public constructor.

**Q: Why would a shader compiler use arena allocation instead of standard `new`/`delete`?**
A: A shader compiler pass might create millions of small IR nodes — SSA values, basic blocks, edges. With `new`/`delete`, each allocation goes through the general-purpose allocator, which has overhead for headers, free-list management, and thread synchronization. Arena allocation makes each allocation a pointer bump — essentially O(1) with no locking. At the end of the pass, you free the entire arena in one call. This dramatically reduces allocation overhead, eliminates fragmentation, and improves cache locality because nodes are laid out sequentially in memory.
*Follow-up:* How do you handle destructors in an arena allocator? — If your objects are trivially destructible, you just drop the arena. If they have non-trivial destructors, you need to keep a list of destructor calls to run before freeing the arena (like `std::pmr::monotonic_buffer_resource` paired with containers that use polymorphic allocators).

**Q: Explain `weak_ptr`. When would you use it?**
A: `weak_ptr` is a non-owning observer of a `shared_ptr`-managed object. It doesn't affect the strong reference count, so the object can be destroyed while `weak_ptr`s still exist. You call `lock()` to get a `shared_ptr` — if the object is gone, you get `nullptr`. Use it to break cycles in graph structures (like IR control-flow or data-flow graphs), for caches where you don't want to keep objects alive just because they're cached, and for observer patterns where the observer shouldn't keep the subject alive.
*Follow-up:* Does `weak_ptr::lock()` introduce a race condition in multithreaded code? — No. `lock()` is atomic — it atomically checks the strong count and increments it if non-zero. If two threads call `lock()` simultaneously, both either succeed or fail cleanly. This is safe by design.

**Q: What's placement new and when do you need it?**
A: Placement new constructs an object at a specific memory address that you've already allocated. You use it when you're implementing a custom allocator, building a memory pool, or working with memory-mapped I/O regions. The key thing is that you're responsible for the memory lifetime separately from the object lifetime — you must call the destructor manually and you must ensure the memory is properly aligned. In GPU tools, you'd use it for constructing command structures in mapped device memory or in pre-allocated instruction buffers.
*Follow-up:* What happens if you use placement new on misaligned memory? — Undefined behavior. On most x86 systems it'll silently work but may be slower. On ARM or when using SIMD types with strict alignment requirements, it'll trap or silently produce wrong results. Always use `std::align` or `alignas` to ensure correct alignment.

**Q: When should you use `unique_ptr` vs `shared_ptr`?**
A: Default to `unique_ptr` — it has zero overhead and makes ownership crystal clear. Use `shared_ptr` only when ownership is genuinely shared, which is rarer than people think. In a compiler, a basic block is owned by the function — that's `unique_ptr`. Cross-references between blocks use raw pointers or indices. `shared_ptr` makes sense for things like interned string tables or type objects that are shared across the entire compilation unit with no clear single owner. If you're reaching for `shared_ptr` a lot, your ownership model is probably unclear.
*Follow-up:* Can you convert a `unique_ptr` to a `shared_ptr`? What about the reverse? — Yes, `unique_ptr` implicitly converts to `shared_ptr` (ownership transfers). The reverse is intentionally impossible — you'd need to know that no other `shared_ptr` references exist, which can't be statically guaranteed.

---

## 4. constexpr, noexcept, Exception Safety, and RAII

### constexpr

`constexpr` means "this can be evaluated at compile time." Since C++14, `constexpr` functions can have loops, local variables, and multiple statements.

```cpp
constexpr uint32_t encode_opcode(uint8_t major, uint8_t minor, uint8_t func) {
    return (static_cast<uint32_t>(major) << 24) |
           (static_cast<uint32_t>(minor) << 16) |
           (static_cast<uint32_t>(func) << 8);
}

// Evaluated at compile time
constexpr uint32_t ADD_OP = encode_opcode(0x01, 0x00, 0x0A);

// constexpr lookup table
constexpr std::array<const char*, 4> reg_names = {"r0", "r1", "r2", "r3"};
```

**C++20 consteval**: Forces compile-time evaluation — if it can't be done at compile time, it's an error. Think of it as "`constexpr` but mandatory."

### noexcept and Move Semantics

**Critical rule**: `std::vector` will only use your move constructor during reallocation if it's marked `noexcept`. Otherwise, it falls back to copying for exception safety.

```cpp
class IRNode {
public:
    // If this isn't noexcept, vector<IRNode> will COPY on reallocation
    IRNode(IRNode&& other) noexcept
        : opcode_(other.opcode_)
        , operands_(std::move(other.operands_)) {}

    IRNode& operator=(IRNode&& other) noexcept = default;
};
```

Why? If a move throws halfway through a vector reallocation, some elements have been moved and some haven't — the vector is in an inconsistent state and can't roll back. Copying is safe because the originals are untouched. If the move is `noexcept`, the vector knows it can't fail, so it uses it.

### Exception Safety Guarantees

| Level | Guarantee | Description |
|-------|-----------|-------------|
| **Nothrow** | Operation cannot throw | Destructors, move ops (ideally), `swap` |
| **Strong** | If it throws, state is rolled back | Like a database transaction |
| **Basic** | If it throws, invariants are preserved but state may change | No leaks, no corruption |
| **None** | No guarantees | Unacceptable in production code |

### RAII (Resource Acquisition Is Initialization)

RAII is the most important idiom in C++. Tie resource lifetime to object lifetime.

```cpp
class MappedMemory {
    void* ptr_;
    size_t size_;
public:
    MappedMemory(size_t size)
        : ptr_(mmap(nullptr, size, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANON, -1, 0))
        , size_(size) {
        if (ptr_ == MAP_FAILED) throw std::system_error(errno, std::generic_category());
    }
    ~MappedMemory() { munmap(ptr_, size_); }
    // Rule of 5: delete copy, implement move
    MappedMemory(const MappedMemory&) = delete;
    MappedMemory& operator=(const MappedMemory&) = delete;
    MappedMemory(MappedMemory&& o) noexcept : ptr_(o.ptr_), size_(o.size_) {
        o.ptr_ = nullptr; o.size_ = 0;
    }
    MappedMemory& operator=(MappedMemory&& o) noexcept {
        if (this != &o) { if (ptr_) munmap(ptr_, size_); ptr_ = o.ptr_; size_ = o.size_; o.ptr_ = nullptr; o.size_ = 0; }
        return *this;
    }
    void* data() { return ptr_; }
};
```

### Interview Q&A

**Q: Why must move constructors be noexcept for std::vector to use them?**
A: When `std::vector` reallocates, it needs to transfer elements from the old buffer to the new one. If it moves elements and a move throws halfway through, the old buffer is partially gutted — some elements have been moved-from — and the vector can't restore the original state. By requiring `noexcept`, the vector knows the transfer can't fail, so it's safe to use move. Without `noexcept`, it falls back to copying, which preserves the originals as a rollback. This can be a major performance cliff in compiler pipelines with large vectors of IR nodes.
*Follow-up:* How can you check at compile time if a type is nothrow-movable? — `std::is_nothrow_move_constructible_v<T>`. You can `static_assert` on this in your code to catch accidental `noexcept` violations early.

**Q: What is RAII and why is it fundamental to C++?**
A: RAII ties resource lifetime to object lifetime — you acquire the resource in the constructor and release it in the destructor. Since C++ guarantees destructors run when objects go out of scope (including during stack unwinding from exceptions), this means resources are always cleaned up. It replaces manual cleanup code, `goto cleanup` patterns, and `finally` blocks. Every C++ resource — memory, file handles, mutexes, GPU contexts, mapped buffers — should be wrapped in an RAII type. If you're calling a cleanup function manually, you're doing it wrong.
*Follow-up:* Does RAII work if an exception is thrown in a constructor? — The destructor for a partially-constructed object is NOT called. But destructors of fully-constructed members and bases ARE called. So if your members are RAII types, their resources are cleaned up. This is another reason to prefer RAII members over raw resources — if you have a raw pointer in a constructor and throw after `new` but before the object is complete, you leak.

**Q: Explain the three exception safety guarantees with an example.**
A: **Nothrow**: `std::swap` on integers — can't fail. **Strong**: `std::vector::push_back` — if reallocation fails, the vector is unchanged. **Basic**: `operator=` that allocates then copies — if the copy throws, the old data is gone but the object is in a valid state (maybe empty). In GPU tools, destructors must be nothrow (releasing device handles should not throw). Copy operations on large IR structures should aim for strong guarantee via copy-and-swap. Basic is the minimum acceptable level.
*Follow-up:* How does copy-and-swap provide the strong guarantee? — You make a copy of the argument (which might throw), then swap the copy into `*this` (which is noexcept). If the copy throws, `*this` is untouched. The swap is just pointer/member swaps — can't fail.

**Q: What's the difference between constexpr and consteval?**
A: `constexpr` means *can* be evaluated at compile time — the compiler will do it if all inputs are compile-time constants, but it can also run at runtime with runtime inputs. `consteval` (C++20) means *must* be evaluated at compile time — if you call it with runtime values, it's a compilation error. Use `consteval` for things like opcode encoding tables where you want to guarantee no runtime computation happens. `constexpr` is more flexible for functions that serve double duty.
*Follow-up:* Can a constexpr function call a non-constexpr function? — Yes, but only on the runtime path. If the compiler is evaluating it at compile time and hits a non-constexpr call, that path fails compile-time evaluation. The function can still be used at runtime on that path.

**Q: Why is noexcept important beyond just move semantics?**
A: `noexcept` lets the compiler generate smaller code — it doesn't need to emit stack unwinding information for noexcept functions. It also enables optimizations in standard library algorithms that check `noexcept` (not just `vector::push_back` but also `sort`, `swap`, etc.). And it documents your contract — if you mark something `noexcept` and it throws, `std::terminate` is called immediately, which is a loud failure instead of undefined behavior. In GPU tool code, noexcept is especially important for destructors and swap operations.
*Follow-up:* What happens if a noexcept function does throw? — `std::terminate()` is called. The stack may or may not be unwound (implementation-defined). This is intentionally harsh — it's better to crash visibly than to silently corrupt state.

---

## 5. The C++ Memory Model and Concurrency

### std::atomic and Memory Ordering

The C++ memory model defines what values a thread can observe when reading shared memory. Without atomics or synchronization, concurrent reads and writes to the same variable are **data races** — undefined behavior, full stop.

```
Thread 1                Thread 2
─────────               ─────────
x.store(42, release)    while(!ready.load(acquire))
ready.store(true, rel)      ;
                        assert(x.load(acquire)==42) // guaranteed!
```

### Memory Orders Explained

```cpp
std::atomic<int> counter{0};
std::atomic<bool> ready{false};
int data = 0;

// RELAXED — no ordering guarantees, just atomicity
counter.fetch_add(1, std::memory_order_relaxed);  // Fast counter

// ACQUIRE/RELEASE — establishes happens-before
// Thread 1 (producer)
data = 42;
ready.store(true, std::memory_order_release);  // Everything before this is visible

// Thread 2 (consumer)
while (!ready.load(std::memory_order_acquire))  // Syncs with the release
    ;
assert(data == 42);  // Guaranteed!

// SEQ_CST — total order across all seq_cst operations (default, slowest)
counter.store(1, std::memory_order_seq_cst);
```

**When to use what**:
- **relaxed**: Counters, statistics, progress indicators — where you just need atomicity
- **acquire/release**: Producer-consumer patterns, publish-subscribe, lock implementations
- **seq_cst**: When you need a total order visible to all threads — rare, expensive, default

### Data Races vs Race Conditions

- **Data race**: Two threads access the same memory location, at least one writes, no synchronization. This is **undefined behavior** in C++.
- **Race condition**: A logic bug where correctness depends on thread scheduling. Not UB, but still a bug.

```cpp
// DATA RACE — UB!
int counter = 0;
// Thread 1: counter++;
// Thread 2: counter++;  // Boom: undefined behavior

// RACE CONDITION — not UB, but wrong
std::atomic<int> counter{0};
// Thread 1: if (counter.load() < 10) counter.store(counter.load() + 1);
// Thread 2: same — TOCTOU bug, but no UB because atomic
```

### Threading Primitives

```cpp
#include <thread>
#include <mutex>
#include <condition_variable>

class CompilationQueue {
    std::queue<ShaderJob> jobs_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool done_ = false;

public:
    void push(ShaderJob job) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            jobs_.push(std::move(job));
        }  // lock released before notify
        cv_.notify_one();
    }

    std::optional<ShaderJob> pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [&] { return !jobs_.empty() || done_; });
        if (jobs_.empty()) return std::nullopt;
        auto job = std::move(jobs_.front());
        jobs_.pop();
        return job;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            done_ = true;
        }
        cv_.notify_all();
    }
};
```

### False Sharing

When two threads write to different variables that happen to be on the same cache line (typically 64 bytes), the cache line bounces between cores:

```
  Core 0 Cache          Core 1 Cache
  ┌─────────────┐       ┌─────────────┐
  │ [a] [b] ... │ ←──── │ [a] [b] ... │   Same cache line!
  └─────────────┘  ping └─────────────┘
                   pong

  Fix: pad to separate cache lines
```

```cpp
struct alignas(64) PaddedCounter {
    std::atomic<uint64_t> count{0};
    // Padding to fill rest of cache line is implicit from alignas
};

// Now each counter gets its own cache line
PaddedCounter per_thread_counts[NUM_THREADS];
```

### thread_local

```cpp
// Each thread gets its own copy — no synchronization needed
thread_local Arena thread_arena(1024 * 1024);  // 1MB per-thread arena

void compile_shader(ShaderJob& job) {
    // Allocate from thread-local arena — no locking!
    auto* ir = thread_arena.alloc<IRModule>();
    // ... compile ...
    thread_arena.reset();
}
```

### C++20 jthread

```cpp
// jthread automatically joins on destruction and supports cancellation
std::jthread worker([](std::stop_token stoken) {
    while (!stoken.stop_requested()) {
        // do work
    }
});
// No need to call join() — destructor does it
```

### Interview Q&A

**Q: Explain acquire/release semantics with a concrete example.**
A: Acquire/release creates a "happens-before" relationship between threads. When Thread 1 does a `release` store, everything it wrote before that store becomes visible to any Thread 2 that does an `acquire` load and sees the stored value. Think of it as a synchronization point — release says "publish all my preceding writes" and acquire says "import all writes that happened before the matching release." This is how you implement a lock-free publish pattern: write your data, then release-store a flag; the consumer acquire-loads the flag and is guaranteed to see the data.
*Follow-up:* Is acquire/release sufficient for implementing a spinlock? — Yes. Lock is an acquire on the lock variable (CAS or exchange), unlock is a release store. This ensures that all memory accesses inside the critical section are visible to the next lock acquirer. You don't need seq_cst for locks.

**Q: What is false sharing and how do you fix it?**
A: False sharing happens when two threads write to different variables that share the same cache line. Even though there's no logical sharing, the hardware invalidates the cache line on each write, causing it to bounce between cores at the cost of ~100 cycles per access instead of ~4. You fix it by padding or aligning data to cache-line boundaries using `alignas(64)`. In a multithreaded shader compiler with per-thread counters, false sharing can cause a 10x slowdown compared to properly padded counters.
*Follow-up:* How do you detect false sharing? — Performance counters (like `perf stat` on Linux) showing high L1/L2 cache miss rates despite small working sets. Tools like `perf c2c` directly detect false sharing. Intel VTune has a "false sharing" analysis mode.

**Q: What's the difference between `lock_guard` and `unique_lock`?**
A: `lock_guard` is simpler — it locks on construction and unlocks on destruction, period. `unique_lock` is more flexible — it supports deferred locking, timed locking, manual unlock/relock, and is movable. You need `unique_lock` with `condition_variable::wait()` because `wait()` needs to unlock and relock the mutex internally. Use `lock_guard` when you just need a simple scoped lock, `unique_lock` when you need any advanced behavior.
*Follow-up:* What about `scoped_lock` (C++17)? — `scoped_lock` can lock multiple mutexes simultaneously without deadlock (using `std::lock` internally). Use it when locking more than one mutex. For a single mutex, `lock_guard` and `scoped_lock` are interchangeable.

**Q: Why is a data race undefined behavior and not just "you might get a wrong value"?**
A: The C++ standard says data races are UB because the compiler and hardware both perform optimizations that assume single-threaded access to non-atomic variables. The compiler might reorder writes, cache a value in a register, or eliminate a "redundant" load. The CPU might reorder stores in a store buffer. With UB, the compiler is free to assume data races don't happen, which enables these optimizations. If data races were merely "unspecified values," the compiler would need to be much more conservative about optimizations on shared memory, hurting single-threaded performance.
*Follow-up:* Can volatile prevent data races? — No. `volatile` prevents the compiler from optimizing away loads/stores, but it doesn't provide atomicity or memory ordering. Two threads writing to a `volatile int` is still a data race. Use `std::atomic`.

**Q: When would you use relaxed memory ordering?**
A: Relaxed ordering provides atomicity (no torn reads/writes) but no ordering guarantees with respect to other memory operations. Use it for: monotonic counters where you just need the final total, progress indicators that are read opportunistically, statistics gathering, and as a building block inside more complex lock-free algorithms where ordering is provided by other means (like an acquire/release fence elsewhere). It's the cheapest atomic operation — on x86 a relaxed load is literally a plain load instruction.
*Follow-up:* On x86, are all loads acquire and all stores release? — Effectively yes — x86 has a strong memory model (TSO). Loads are not reordered with other loads, stores are not reordered with other stores. But you still need to use `std::atomic` with proper orderings because: (1) the compiler can still reorder, and (2) your code should be portable. Relaxed on x86 is free, but on ARM it actually makes a difference.

---

## 6. Undefined Behavior and Low-Level Correctness

### Why This Matters for GPU Tools

GPU development tools encode and decode binary instruction streams. You're reading bit-packed fields from byte arrays, casting between pointer types, working with specific endianness assumptions, and often operating on data that doesn't align with C++ types. **Every rule in this section is a trap that can corrupt instruction encodings silently.**

### Strict Aliasing

The compiler assumes pointers of different types don't alias the same memory (with exceptions for `char*`, `unsigned char*`, `std::byte*`).

```cpp
// UNDEFINED BEHAVIOR — strict aliasing violation
float fast_inverse_sqrt(float x) {
    int32_t i = *(int32_t*)&x;  // UB! float* and int32_t* alias
    i = 0x5f3759df - (i >> 1);
    return *(float*)&i;          // UB!
}

// CORRECT — use memcpy (optimizes to the same code)
float fast_inverse_sqrt_correct(float x) {
    int32_t i;
    std::memcpy(&i, &x, sizeof(i));  // OK — memcpy is blessed
    i = 0x5f3759df - (i >> 1);
    float result;
    std::memcpy(&result, &i, sizeof(result));
    return result;
}

// C++20 — std::bit_cast (best option)
float fast_inverse_sqrt_modern(float x) {
    auto i = std::bit_cast<int32_t>(x);
    i = 0x5f3759df - (i >> 1);
    return std::bit_cast<float>(i);
}
```

### Type Punning Methods

| Method | Legal? | Constexpr? | Notes |
|--------|--------|------------|-------|
| `reinterpret_cast` | Usually UB | No | Only legal for char/byte pointers |
| `union` reading inactive member | UB in C++ (OK in C) | No | Common compiler extension, not portable |
| `memcpy` | Always legal | No (pre-C++20) | Compiler optimizes away the copy |
| `std::bit_cast` (C++20) | Always legal | Yes | Best option, requires trivially copyable types |

### Signed Overflow

```cpp
// UNDEFINED BEHAVIOR
int x = INT_MAX;
x += 1;  // UB! Compiler may assume this doesn't happen

// The compiler can optimize based on this:
bool check(int x) { return x + 1 > x; }
// Compiler may optimize to: return true;  (because signed overflow is UB)

// SAFE alternative: use unsigned or check before
uint32_t safe_add(uint32_t a, uint32_t b) {
    return a + b;  // Unsigned overflow is well-defined (wraps)
}
```

### Alignment Faults

```cpp
// DANGEROUS — misaligned access
char buffer[10];
uint32_t* p = reinterpret_cast<uint32_t*>(buffer + 1);  // Misaligned!
uint32_t val = *p;  // May crash on ARM, silently slow on x86

// SAFE — use memcpy
uint32_t val_safe;
std::memcpy(&val_safe, buffer + 1, sizeof(val_safe));
```

### Bitfields and Portability

```cpp
// DO NOT USE for binary protocol/instruction encoding!
struct InstructionBits {
    uint32_t opcode : 7;     // Bit ordering is implementation-defined
    uint32_t dst    : 5;     // Layout is NOT portable
    uint32_t src0   : 5;
    uint32_t src1   : 5;
    uint32_t func   : 10;
};
// Compiler decides bit ordering, padding, and whether fields cross
// storage boundaries. Different compilers/platforms lay this out differently.

// CORRECT approach for binary encoding: manual bit manipulation
struct Instruction {
    uint32_t raw;

    uint32_t opcode() const { return raw & 0x7F; }
    uint32_t dst()    const { return (raw >> 7) & 0x1F; }
    uint32_t src0()   const { return (raw >> 12) & 0x1F; }
    // Explicit, portable, predictable
};
```

### Endianness

```cpp
// Network/file format byte swapping
constexpr uint32_t bswap32(uint32_t x) {
    return ((x >> 24) & 0x000000FF) |
           ((x >>  8) & 0x0000FF00) |
           ((x <<  8) & 0x00FF0000) |
           ((x << 24) & 0xFF000000);
}

// C++20: std::endian
#if __cplusplus >= 202002L
static_assert(std::endian::native == std::endian::little,
              "This GPU tool assumes little-endian host");
#endif
```

### Integer Promotion

```cpp
// Surprise! Small types promote to int
uint8_t a = 200, b = 100;
auto result = a + b;  // result is int (300), not uint8_t!

// This matters for shift operations
uint8_t mask = 0xFF;
auto shifted = mask << 24;  // mask promotes to int (signed!), then shifts
// On 32-bit int: 0xFF << 24 = 0xFF000000 which is negative as int — UB potential

// Safe:
auto shifted_safe = static_cast<uint32_t>(mask) << 24;  // Explicitly unsigned
```

### Interview Q&A

**Q: What is strict aliasing and why does it matter for instruction encoding?**
A: Strict aliasing is the compiler's assumption that pointers of different types don't point to the same memory. This lets it optimize aggressively — if you write through a `float*`, it assumes no `int*` reads are affected and can reorder or cache values. In instruction encoding, you often want to reinterpret bytes as integers or vice versa. Using `reinterpret_cast` for this violates strict aliasing and is UB — the compiler might optimize away your writes or reorder them. Use `memcpy` or `std::bit_cast` instead, which the compiler optimizes to the same machine code but are standards-legal.
*Follow-up:* Why is `memcpy` not a performance concern? — Because every modern compiler recognizes `memcpy` of small, fixed-size types and replaces it with a register move or nothing at all. It's a "compiler blessed" type-punning mechanism — zero overhead.

**Q: Why shouldn't you use bitfields for binary instruction formats?**
A: Bitfield layout is implementation-defined: the order of bits within a storage unit, whether a field can span storage boundaries, and padding are all up to the compiler. MSVC and GCC lay out the same bitfield struct differently. On a GPU tools team, your instruction encoding must be bit-exact and portable — you're generating binary that hardware will execute. Use explicit shift-and-mask operations so the encoding is deterministic and platform-independent.
*Follow-up:* Are bitfields ever appropriate? — Yes, for internal-only flags and options where you want compact storage and don't care about binary layout — like compiler IR node flags. Just never for wire formats, file formats, or instruction encodings.

**Q: What's the difference between UB from signed overflow vs well-defined unsigned wrap?**
A: Signed integer overflow is UB in C++ — the compiler may assume it never happens and optimize accordingly. For example, `x + 1 > x` can be optimized to `true` for signed `x` because overflow is UB. Unsigned overflow wraps modulo 2^N by definition — `UINT_MAX + 1 == 0`. For GPU instruction encoding, always use unsigned types for bit manipulation. Signed types invite the compiler to make assumptions that break your carefully constructed bit patterns.
*Follow-up:* Does `-fwrapv` fix signed overflow UB? — For GCC/Clang, yes — it makes signed overflow wrap like unsigned. But it's a non-standard extension, it may disable other optimizations, and your code is no longer portable. Better to just use unsigned types for bit operations.

**Q: How do you safely read a uint32_t from an unaligned byte buffer?**
A: Use `memcpy`. Cast the byte pointer to `uint32_t*` and dereferencing is both a strict aliasing violation and a potential alignment fault. `memcpy(&value, buffer + offset, sizeof(uint32_t))` is always safe — the compiler knows the source may be unaligned and generates appropriate code (on x86, it's a single `mov`; on ARM, it might be multiple byte loads). This is the standard pattern for parsing binary instruction streams from arbitrary byte positions.
*Follow-up:* What about `__attribute__((packed))`? — It works on GCC/Clang for structs, forcing no padding and generating unaligned accesses. But it's non-standard, it doesn't solve the aliasing issue, and it can silently be slower on architectures without hardware unaligned access support. `memcpy` is the portable, standards-blessed approach.

**Q: Explain integer promotion and why it's dangerous for bit manipulation.**
A: Integer types smaller than `int` (like `uint8_t`, `uint16_t`) are promoted to `int` before arithmetic operations. This is dangerous because `int` is signed, so operations like left-shifting a `uint8_t` by a large amount can overflow a signed `int` — which is UB. The fix is to explicitly cast to `uint32_t` (or the appropriate unsigned type) before performing bit operations. This is one of those subtle bugs that only shows up when someone shifts a byte by 24 bits and the compiler decides the signed overflow means it can do whatever it wants.
*Follow-up:* Does `uint16_t a = 0xFFFF; uint16_t b = a * a;` overflow? — The multiplication promotes both operands to `int`. If `int` is 32 bits, `0xFFFF * 0xFFFF = 0xFFFE0001`, which fits in `int`, so no overflow. The result is then narrowed back to `uint16_t` by truncation. But if `int` were 16 bits (legal but rare), you'd get signed overflow UB.

---

## 7. Compilation Model and ABI

### Translation Units and ODR

A **translation unit** (TU) is a source file after preprocessing — with all `#include`s expanded. Each TU is compiled independently. The **One Definition Rule** (ODR) says each entity can have at most one definition across all TUs (with exceptions for inline functions, templates, and constexpr).

```
  header.h          a.cpp          b.cpp
  ┌──────────┐     ┌──────────┐  ┌──────────┐
  │ class Foo │◄────│#include  │  │#include  │
  │ { ... };  │     │"header.h"│  │"header.h"│
  └──────────┘     │Foo::bar()│  │ main()   │
                   │{ ... }   │  │{ Foo f; }│
                   └─────┬────┘  └─────┬────┘
                         │             │
                    a.o (TU)       b.o (TU)
                         └──────┬──────┘
                           linker
                              │
                         executable
```

### Inline and Linkage

- **`inline`** doesn't mean "inline this function." It means "this symbol can be defined in multiple TUs and the linker should pick one." Required for functions defined in headers.
- **Internal linkage** (`static`, anonymous namespace): symbol is private to the TU.
- **External linkage** (default for non-static functions): symbol is visible to the linker across TUs.

### Name Mangling

C++ encodes function signatures into symbol names to support overloading:

```
void encode(int)        → _Z6encodei       (GCC/Clang)
void encode(float)      → _Z6encodef
void encode(int, float) → _Z6encodeif
```

`extern "C"` disables mangling — essential for C interop and plugin APIs:

```cpp
extern "C" {
    void* create_encoder();       // Symbol is just "create_encoder"
    void  destroy_encoder(void*);
}
```

### VTable and Object Layout

```cpp
class Base {
    int x;
    virtual void foo();
    virtual void bar();
};

class Derived : public Base {
    int y;
    void foo() override;
    virtual void baz();
};
```

```
  Base object layout:        Derived object layout:
  ┌──────────────┐           ┌──────────────┐
  │ vptr ────────┼──┐        │ vptr ────────┼──┐
  │ x            │  │        │ x (from Base)│  │
  └──────────────┘  │        │ y            │  │
                    │        └──────────────┘  │
                    ▼                          ▼
  Base vtable:              Derived vtable:
  ┌──────────────┐          ┌──────────────┐
  │ &Base::foo   │          │ &Derived::foo│  (overridden)
  │ &Base::bar   │          │ &Base::bar   │  (inherited)
  └──────────────┘          │ &Derived::baz│  (new)
                            └──────────────┘
```

**Multiple inheritance layout:**

```cpp
class A { virtual void f(); int a; };
class B { virtual void g(); int b; };
class C : public A, public B { void f() override; void g() override; int c; };
```

```
  C object layout:
  ┌──────────────┐  ◄── this for A* and C*
  │ A::vptr      │
  │ a            │
  ├──────────────┤  ◄── this for B* (adjusted pointer!)
  │ B::vptr      │
  │ b            │
  ├──────────────┤
  │ c            │
  └──────────────┘

  When casting C* to B*, the pointer is adjusted forward by sizeof(A-subobject).
  The B::vptr in C points to thunks that adjust 'this' back before calling C's overrides.
```

### ABI Stability and PIMPL

Adding a virtual function, changing member layout, or even reordering members can break ABI. **PIMPL** hides implementation details behind a pointer:

```cpp
// public_api.h — stable ABI
class Compiler {
public:
    Compiler();
    ~Compiler();
    void compile(const char* source);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// compiler.cpp — can change freely
struct Compiler::Impl {
    IRModule module;
    OptimizationPipeline pipeline;
    DiagnosticEngine diag;
    // Adding members here doesn't break ABI
};

Compiler::Compiler() : impl_(std::make_unique<Impl>()) {}
Compiler::~Compiler() = default;  // Must be in .cpp where Impl is complete
void Compiler::compile(const char* source) {
    impl_->pipeline.run(impl_->module, source);
}
```

### Static vs Shared Libraries and LTO

- **Static library** (`.a`/`.lib`): Linked at compile time, symbols are copied into the executable. No runtime dependency, but binary is larger.
- **Shared library** (`.so`/`.dll`): Loaded at runtime, symbols resolved dynamically. Smaller binary, allows updating the library without relinking, but ABI must be stable.
- **LTO** (Link-Time Optimization): The compiler defers optimization to link time, allowing cross-TU inlining, dead code elimination, and devirtualization. `-flto` on GCC/Clang. Significant build time cost but can give 5-20% performance improvement.

### Interview Q&A

**Q: What is the One Definition Rule and what happens when you violate it?**
A: ODR says each entity (function, class, variable) can have at most one definition across the whole program. If two TUs define the same non-inline function, the linker will error. The more insidious case is when two TUs see *different* definitions of the same class (maybe from including different versions of a header) — this is an ODR violation that's usually silent and causes subtle corruption because different TUs disagree on object layout. ODR violations are undefined behavior that no tool is required to diagnose.
*Follow-up:* How can ODR violations happen in practice? — Header-only libraries with `#ifdef`s that evaluate differently in different TUs, macros that change struct layout, including different versions of the same header, or defining the same class differently in two source files. Sanitizers like `-fsanitize=undefined` and gold linker's `--detect-odr-violations` can catch some cases.

**Q: Explain the PIMPL idiom and why it matters for GPU driver tools.**
A: PIMPL (Pointer to Implementation) puts all private members behind a single pointer to a forward-declared struct. The header only exposes the public API and the pointer. This means you can add, remove, or reorder private members without breaking ABI — consumers of the header don't need to recompile. For GPU driver tools, this is critical because you ship shared libraries (like the CUDA compiler API) that must maintain binary compatibility across driver versions. PIMPL costs one pointer indirection and one heap allocation, which is negligible for API-level objects.
*Follow-up:* What's the performance cost of PIMPL? — One extra indirection on every member access (likely a cache miss on first access), and one heap allocation on construction. For hot inner-loop objects, this is too expensive. For API boundary objects (compilers, contexts, device handles), it's free in practice.

**Q: How does virtual inheritance differ from regular multiple inheritance in layout?**
A: In regular multiple inheritance, each base has its own sub-object with a fixed offset from the derived object. In virtual inheritance, the virtual base is shared and its offset from the derived object is determined at runtime via the vtable (or a vbptr). This means `static_cast` to a virtual base isn't possible — you need `dynamic_cast`. The layout is more complex: the derived object stores an offset or pointer to the shared virtual base, which can be at different offsets depending on the most-derived class. It solves the diamond problem but adds runtime and complexity costs.
*Follow-up:* When would you use virtual inheritance in a GPU tool codebase? — Almost never. Diamond inheritance is a design smell. If you encounter it, prefer composition or interfaces (abstract classes with no data members). You might see it in legacy code or in deep class hierarchies for IR node types, but modern codebases use `std::variant` or CRTP instead.

**Q: What is name mangling and why does `extern "C"` matter?**
A: C++ encodes function signatures into symbol names (mangling) to support overloading — `encode(int)` and `encode(float)` get different symbols. The mangling scheme is compiler-specific (GCC and MSVC mangle differently). `extern "C"` disables mangling, giving you plain C-style symbol names. This is essential for: (1) calling C libraries from C++, (2) creating plugin APIs with `dlsym`/`GetProcAddress`, (3) exposing C-compatible APIs from C++ shared libraries for cross-language interop (Python, Rust, etc.).
*Follow-up:* Is the C++ ABI standardized? — No. There's the Itanium C++ ABI that GCC, Clang, and most Unix compilers follow, but MSVC uses a different ABI. Even within the Itanium ABI, different standard library implementations (libstdc++ vs libc++) aren't ABI-compatible. This is why stable public APIs use `extern "C"` or PIMPL.

**Q: What is LTO and when would you enable it?**
A: Link-Time Optimization delays optimization to link time, giving the optimizer visibility across all translation units. This enables cross-TU inlining, interprocedural constant propagation, dead code elimination, and devirtualization. For a GPU shader compiler, LTO can be significant because you have many small functions across TUs (IR visitors, pattern matchers) that benefit from cross-TU inlining. The cost is much longer link times — sometimes 10x. Use it for release builds but not for development iteration.
*Follow-up:* What's Thin LTO vs Full LTO? — Full LTO merges all TUs into one giant IR and optimizes globally — best optimization but enormous memory use and no parallelism. Thin LTO keeps TUs separate but shares summaries — the linker can parallelize and uses much less memory, with most of the optimization benefit. Thin LTO is usually the right default.

---

## 8. Performance

### Cache-Friendly Data Layout

Modern CPUs are fast; memory is slow. A cache miss to main memory costs ~100 cycles vs ~4 cycles for an L1 hit. **Data layout determines performance more than algorithm choice for many workloads.**

```
  CPU
  ┌────────────────────────────┐
  │ Core                       │
  │  ┌───────┐                 │
  │  │ L1 4c │←── 64KB         │
  │  │ L2 12c│←── 256KB-1MB    │
  │  └───────┘                 │
  │  L3  ~40c ←── 4-32MB       │
  └────────────────────────────┘
         ↓ ~100 cycles
  ┌────────────────────────────┐
  │       Main Memory          │
  └────────────────────────────┘
```

### AoS vs SoA

**Array of Structures** (AoS): Each entity is a struct. Intuitive but cache-unfriendly if you only access some fields.

```cpp
// AoS — bad for iterating over just positions
struct Particle {
    float x, y, z;        // 12 bytes
    float vx, vy, vz;     // 12 bytes
    uint32_t color;        // 4 bytes
    uint32_t flags;        // 4 bytes
};  // 32 bytes total

std::vector<Particle> particles(1'000'000);

// This loads 32 bytes per particle but only uses 12
for (auto& p : particles) {
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    p.z += p.vz * dt;
}
```

**Structure of Arrays** (SoA): Each field is a separate array. Cache-friendly when processing one field at a time.

```cpp
// SoA — cache-friendly for position updates
struct Particles {
    std::vector<float> x, y, z;
    std::vector<float> vx, vy, vz;
    std::vector<uint32_t> color;
    std::vector<uint32_t> flags;
};

Particles p;
// This streams through contiguous floats — perfect cache utilization
for (size_t i = 0; i < n; ++i) {
    p.x[i] += p.vx[i] * dt;
    p.y[i] += p.vy[i] * dt;
    p.z[i] += p.vz[i] * dt;
}
```

In GPU tools, SoA matters for IR node processing — if your pass only reads opcodes, don't force it to load the entire node (with operands, debug info, etc.) into cache.

### Branch Prediction

```cpp
// Unpredictable branch — ~15 cycle penalty per mispredict on modern CPUs
for (auto& instr : instructions) {
    if (instr.opcode() == OP_BRANCH) {  // Rare and unpredictable
        handle_branch(instr);
    } else {
        handle_alu(instr);
    }
}

// Better: sort by opcode first, or group by type
// Or use branchless techniques:
uint32_t result = (condition) ? value_a : value_b;
// Compiler often generates cmov instead of branch
```

### std::vector Growth and Reserve

`std::vector` typically doubles its capacity when full. Each reallocation copies (or moves if noexcept) all elements.

```cpp
// BAD — causes ~20 reallocations for 1M elements
std::vector<IRNode> nodes;
for (int i = 0; i < 1'000'000; ++i) {
    nodes.push_back(make_node(i));  // Multiple reallocations
}

// GOOD — one allocation
std::vector<IRNode> nodes;
nodes.reserve(1'000'000);  // Allocate once
for (int i = 0; i < 1'000'000; ++i) {
    nodes.push_back(make_node(i));
}
```

### Small-Buffer Optimization (SBO)

Many standard types store small data inline to avoid heap allocation:

```cpp
// std::string typically has ~22 bytes of inline storage
std::string short_name = "r0";       // No heap allocation (SBO)
std::string long_name = "very_long_register_name_that_exceeds_sbo";  // Heap

// std::function typically has ~32 bytes of inline storage
std::function<void()> small = [] { };  // No heap allocation
std::function<void()> big = [large_capture] { };  // May heap-allocate
```

### When Move Is Not Applied

Move doesn't fire when you expect:
1. Const objects — `std::move(const_obj)` gives `const T&&`, binds to copy ctor
2. Missing `noexcept` — `vector` falls back to copy during reallocation
3. Return by `std::move(local)` — prevents NRVO, may not even move
4. Objects still in use — moving from `x` then using `x` is a logic bug

### Allocation Costs

Every `malloc`/`new` call:
1. Acquires a lock (in many allocators) or does atomic operations
2. Searches a free list or bumps a pointer
3. May trigger a `brk`/`mmap` syscall
4. Introduces a TLB entry that competes for TLB slots

For a compiler pass processing millions of nodes, this overhead is significant. This is why arena allocation (section 3) exists.

### Measuring with Benchmarks

```cpp
// Use Google Benchmark for microbenchmarks
#include <benchmark/benchmark.h>

static void BM_VectorPushBack(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<int> v;
        for (int i = 0; i < state.range(0); ++i)
            v.push_back(i);
        benchmark::DoNotOptimize(v.data());
    }
}
BENCHMARK(BM_VectorPushBack)->Range(8, 1 << 20);

static void BM_VectorReserved(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<int> v;
        v.reserve(state.range(0));
        for (int i = 0; i < state.range(0); ++i)
            v.push_back(i);
        benchmark::DoNotOptimize(v.data());
    }
}
BENCHMARK(BM_VectorReserved)->Range(8, 1 << 20);

BENCHMARK_MAIN();
```

Always use `DoNotOptimize` to prevent the compiler from eliminating your code, and `ClobberMemory` to force memory writes to be visible.

### Interview Q&A

**Q: What is AoS vs SoA and when does it matter?**
A: AoS (Array of Structures) stores each entity's fields together. SoA (Structure of Arrays) stores each field in its own array. SoA is better when you iterate over many entities but only access a few fields — you get better cache utilization because you're streaming through contiguous data instead of skipping over unused fields. In GPU tools, if a compiler pass only reads opcodes from IR nodes, SoA means you load a cache line of 16 opcodes at once instead of loading one full node (with operands, metadata, debug info) to get one opcode. This can be a 4-8x speedup for linear scans.
*Follow-up:* What are the downsides of SoA? — More complex code (you manage N arrays instead of one), harder to add/remove entities (you must keep all arrays in sync), poor cache behavior if you access many fields of the same entity, and harder to pass individual entities to functions (you need an index instead of a reference).

**Q: Why is `reserve()` important for `std::vector`?**
A: Without `reserve`, `vector` starts with a small capacity and doubles it each time it's full. Each doubling requires allocating a new buffer, moving all elements, and freeing the old buffer. For N elements, you get ~log2(N) reallocations and copy/move ~2N elements total. With `reserve(N)`, you do exactly one allocation. For a compiler that builds a vector of a million IR nodes, the difference is between one allocation and ~20 reallocations with millions of moves. The moves might be cheap (pointer swaps), but the allocations aren't.
*Follow-up:* Does `reserve()` change `size()`? — No. `reserve` sets `capacity` but `size` stays the same. `resize` changes `size` and constructs/destroys elements. This is a common confusion.

**Q: What is small-buffer optimization?**
A: SBO stores small objects inline within the container (like `std::string` or `std::function`) instead of heap-allocating. A typical `std::string` has ~22 bytes of inline buffer — short strings like register names fit without a `malloc`. This eliminates allocation overhead and gives better cache locality. The tradeoff is that the container is larger even when empty. In GPU tools, SBO matters because you have millions of short strings (register names, labels) and millions of small closures (visitors, predicates).
*Follow-up:* How does SBO affect move semantics? — With SBO, move can't just swap pointers — it must copy the inline buffer element by element. This means small-object move is O(N) where N is the buffer size, not O(1). For very hot code paths, this can matter.

**Q: How does branch prediction affect compiler pass performance?**
A: Modern CPUs predict branch outcomes and speculatively execute the predicted path. A misprediction costs ~15-20 cycles of pipeline flush. If your instruction visitor has a switch/case on opcode and the opcodes are randomly distributed, you get frequent mispredictions. Solutions: sort instructions by opcode before processing, group by type, use computed dispatch (function pointer table) which the branch predictor handles differently, or use branchless techniques (cmov). In practice, sorting by opcode before a linear pass can give 2-3x speedup on modern CPUs.
*Follow-up:* How can you measure branch mispredictions? — `perf stat -e branch-misses,branches your_binary` on Linux gives you the miss rate. Above 5% is usually a problem. Intel VTune and AMD uProf provide similar metrics. You can also use `__builtin_expect` (`[[likely]]/[[unlikely]]` in C++20) to hint the compiler, but this doesn't help if the branch is genuinely unpredictable.

**Q: When does a move not actually move?**
A: Several cases: (1) Moving a const object — `std::move(const_obj)` produces `const T&&` which binds to the copy constructor, not the move constructor. No error, silent copy. (2) Move constructor not `noexcept` and used with `vector::push_back` — vector copies instead for exception safety. (3) `return std::move(local)` — prevents NRVO, and in some cases doesn't even move. (4) Types with no move constructor — falls back to copy. (5) After copy elision — there's no move because the object is constructed in place. Always check with `std::is_nothrow_move_constructible_v` in your static assertions.
*Follow-up:* What's the state of a moved-from object? — The standard says it's in a "valid but unspecified" state. You can destroy it or assign to it, but you can't rely on its value. For standard types: a moved-from `vector` is empty, a moved-from `string` is empty, a moved-from `unique_ptr` is null. For your own types, document the moved-from state.

---

## Red Flags

Common wrong answers that signal "this person will write bugs in production systems code":

1. **"I'd use `reinterpret_cast` to read the instruction bits as an int."** — Strict aliasing violation. Use `memcpy` or `std::bit_cast`.

2. **"Bitfields are perfect for instruction encoding."** — Bit layout is implementation-defined. Manual shift-and-mask is the only portable option.

3. **"I always use `shared_ptr` for ownership."** — Shows lack of understanding of ownership semantics. `unique_ptr` should be the default; `shared_ptr` is for genuinely shared ownership.

4. **"std::move actually moves the object."** — It's a cast to rvalue reference. The move constructor does the actual moving. And moving from a const object silently copies.

5. **"`volatile` provides thread safety."** — No. `volatile` prevents compiler optimizations on the variable but provides no atomicity or memory ordering. Use `std::atomic`.

6. **"I don't bother with `noexcept` on move constructors."** — This silently kills `std::vector` performance by forcing copies during reallocation.

7. **"Data races just give you wrong values, right?"** — No. Data races are undefined behavior in C++. The compiler may assume they don't exist and optimize accordingly. Your program may time-travel.

8. **"I'd use `new` and `delete` for the IR nodes in the compiler."** — In a compiler pass creating millions of nodes, this fragments the heap and adds mutex contention. Use arena allocation.

9. **"Virtual functions are fine for per-instruction dispatch."** — For millions of instructions per compilation, virtual dispatch adds an indirect branch per instruction. Use CRTP, switch statements, or function pointer tables.

10. **"The PIMPL pattern is just about hiding implementation."** — It's primarily about ABI stability. Hiding is a secondary benefit.

11. **"I'd use `int` for bit manipulation."** — `int` is signed. Signed overflow is UB, and left-shifting into the sign bit is UB. Use unsigned types.

12. **"We can just use `seq_cst` everywhere for thread safety."** — Shows lack of understanding of memory ordering costs. On ARM, `seq_cst` inserts full barriers. Know when `relaxed` or `acquire/release` suffice.

---

## Whiteboard Checklist

Things you must be able to write or draw from memory in an interview:

### Code You Should Be Able to Write
- [ ] Rule-of-5 class with copy-and-swap idiom
- [ ] Simple bump/arena allocator with aligned allocation
- [ ] CRTP base with compile-time dispatch
- [ ] `std::enable_if` SFINAE constraint on a function template
- [ ] Fold expression for summing a parameter pack
- [ ] RAII wrapper for a C-style handle (constructor/destructor/move, deleted copy)
- [ ] Lock-free producer-consumer with acquire/release atomics
- [ ] Thread-safe queue with mutex + condition_variable
- [ ] PIMPL class with header and implementation split
- [ ] Instruction bit-field extraction with shift-and-mask (not bitfields)
- [ ] Safe type punning with `memcpy` / `std::bit_cast`
- [ ] `constexpr` function for compile-time computation

### Diagrams You Should Be Able to Draw
- [ ] `shared_ptr` control block layout (strong count, weak count, object, deleter)
- [ ] vtable layout for single and multiple inheritance
- [ ] Object layout with multiple inheritance (showing pointer adjustment)
- [ ] Cache line and false sharing
- [ ] AoS vs SoA memory layout
- [ ] Acquire/release happens-before relationship between two threads
- [ ] Translation unit → object file → linker → executable pipeline
- [ ] `std::vector` reallocation (old buffer, new buffer, capacity vs size)

### Concepts You Should Explain in 30 Seconds
- [ ] Why `memcpy` is the correct type-punning tool (strict aliasing)
- [ ] Why `noexcept` on move constructors affects vector performance
- [ ] The difference between a data race and a race condition
- [ ] Why arena allocators are essential for compiler IR
- [ ] Reference collapsing rules and how `std::forward` uses them
- [ ] Why bitfields are unsuitable for instruction encoding
- [ ] ODR violations and why they're silent UB
- [ ] PIMPL for ABI stability across shared library versions
- [ ] Signed vs unsigned overflow behavior and its impact on optimization
- [ ] When NRVO can and cannot fire
