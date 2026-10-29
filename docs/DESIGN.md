# NovaLang design notes

This document explains how the compiler is built, the decisions behind it, and what the
alternatives would have cost. The code is about 3,000 lines of C++ plus the Flex and Bison
specifications.

```text
 .nov ──► lexer.l ──► parser.y ──► sema.cpp ──────────────► codegen.cpp ──► LLVM ──► .out
          (Flex)      (Bison,      types, units, unit        IR, runtime      O0-O3    or
                      LALR(1))     inference, structs,       checks, BCE      pipeline in-memory
                                   bounds-check proofs       decisions                 JIT
```

| File | Responsibility |
| --- | --- |
| `src/lexer.l` | Tokens with line and column numbers. Unit annotations such as `<kg*m/s^2>` are lexed as a single token and parsed into an exponent map. |
| `src/parser.y` | Conflict-free LALR(1) grammar. Builds the AST and desugars `x \|> f(a)` into `f(x, a)`. |
| `src/ast.h` | AST and the `Type` / `Unit` representations. |
| `src/sema.cpp` | Name resolution, type checking and inference, struct layout checks, unit resolution, unit-variable inference, return-path analysis, and bounds-check elimination. |
| `src/codegen.cpp` | LLVM IR generation, the optimization pipeline, object emission and the ORC JIT. |
| `src/main.cpp` | Driver and flags. |

---

## 1. Units of measure

### Representation

A unit is a map from base-unit name to an integer exponent:

```text
kg*m/s^2   →  { kg: 1, m: 1, s: -2 }
```

Units form a free abelian group. Multiplying units adds exponents, dividing subtracts them,
and an exponent that reaches zero is removed, so `m/m` becomes the empty map
(dimensionless). Because the map is sorted (`std::map`), equal units always have the same
canonical form, and comparing units is a map comparison.

Aliases (`unit N = <kg*m/s^2>;`) are expanded into base units when they are declared, so `N`
and `kg*m/s^2` are the same type. The trade-off is that error messages and printed values
show base units (`kg*m/s^2`) rather than the alias the programmer wrote. Keeping the alias
would need a separate "display unit" carried next to the canonical one.

### Typing rules

| Expression | Rule |
| --- | --- |
| `a + b`, `a - b`, comparisons | Shapes and units must be identical. |
| `a * b`, `a / b` | Units multiply or divide. |
| `a % b` | `int` only, with identical units. |
| `x as float` | Strips the unit. This is the explicit escape hatch. |
| `x as float<m>` | Attaches a unit. |

### Erasure

Units exist only in the type checker. `float<m/s>` and `float` both lower to LLVM `double`.
This is the most important design decision in the language:

- Units cost nothing at runtime: no tagging, no boxing, no checks.
- Unit polymorphism (below) needs no monomorphization and no dictionary passing. A single
  machine-code body of `sqrt` serves `m^2 → m`, `s^2 → s` and every other instantiation.

The cost is that units can't be inspected at runtime (there's no way to ask a value for its
unit). `print` still shows units because the static type is known at every call site, so
the format string is baked in at compile time.

### Unit polymorphism and inference

A unit name beginning with `'` is a unit variable, as in F#:

```nova
fn sqrt(x: float<'u^2>) -> float<'u>
fn mean(xs: [float<'u>]) -> float<'u>
fn ratio(a: float<'a>, b: float<'b>) -> float<'a/'b>
```

**Inside the body**, unit variables are treated as rigid, opaque base units. `'u * 'u`
simplifies to `'u^2` by the same exponent arithmetic as `m * m`. The body is therefore checked
once, generically, and a body that doesn't respect the signature (`return x * x` where
`float<'u>` is expected) is rejected.

**At a call site**, each argument produces an equation over the group:

```text
param_unit('u, 'v, ...)  =  arg_unit
```

These are linear equations over integer exponent vectors. `instantiate()` in `sema.cpp`
solves them by repeated elimination:

1. Apply the current substitution to every equation.
2. An equation with no unknowns is checked for equality.
3. An equation with exactly one unknown `'u` with exponent `k` gives
   `'u = (arg / rest)^(1/k)`. A solution exists only if every exponent in `arg / rest` is
   divisible by `k`. That's how `sqrt(9.0<m>)` is rejected with
   "m has no exact square root".
4. Repeat until no equation changes.

Equations in which several unknowns are still tangled (for example a parameter typed
`float<'a*'b>`) are reported as "cannot infer". Solving those in general requires Hermite or
Smith normal form over the integers, as in Kennedy's units-of-measure inference. Eliminating
one variable at a time covers every signature I've needed, and its errors point at a
specific argument.

**Two guards keep this sound:**

- **Renaming apart.** The callee's variables are renamed per call (`'u` → `'u#7`), so a
  generic function that calls another generic function with its own `'u` can't conflate them.
- **No return-only variables.** A variable that appears only in the return type
  (`fn f(x: float<'u>) -> float<'v>`) is rejected at the definition, because no call could
  ever determine it.

`sqrt` and `abs` are ordinary entries in the signature table with polymorphic types. The
checker has no special case for them, and codegen lowers them to the `llvm.sqrt` and
`llvm.fabs` intrinsics.

---

## 2. Parsing decisions

- **Unit annotations are a lexer token.** Writing `<m/s>` with angle brackets is ambiguous
  with the comparison operators at the grammar level (`x as float < y`). The lexer instead
  matches `<`, a unit expression and `>` with no spaces inside as one `UNIT_ANNOT` token. The
  only expressions this misreads are things like `a<b>c` written without spaces, which can't
  type-check anyway because `bool > int` is rejected.
- **Struct literals use named arguments.** NovaLang writes `Vec2(x: 1.0, y: 2.0)` rather than
  Rust's `Vec2 { x: 1.0 }`. Rust's form is ambiguous with blocks in `if v { ... }`, which is
  why Rust forbids struct literals in conditions. With named arguments, the parser decides
  after `IDENT '(' IDENT` by looking for a `:`, which works with one token of lookahead
  because `:` never follows an expression.
- **Assignment targets are expressions.** The rule `IDENT '=' expr` conflicts with
  `IDENT '[' ...` and `IDENT '.' ...`. Using `expr '=' expr` instead keeps the grammar
  conflict-free, and sema checks that the left side is an lvalue (`x`, `a[i]`, `s.f`, or a
  chain of them).
- The grammar reports **zero conflicts** under `bison -Wall`.

---

## 3. Structs

- **Value semantics.** Structs are LLVM named struct types, passed and returned as
  first-class aggregates. Assigning a struct copies it. A struct that contains an array
  shares the array, because arrays are reference values. In that sense the copy is shallow.
- **Field writes use GEP paths.** `bodies[i].vel.x -= dv` is lowered by `genAddress()` to one
  bounds check (when needed) plus struct GEPs, followed by a load and store of a single
  `double`. Reading a field of an addressable expression also uses a GEP instead of loading
  the whole struct. A field of an rvalue (`f().x`) uses `extractvalue`.
- **Infinite-size check.** A struct that contains itself by value, directly or through other
  structs, is rejected by a depth-first search over the containment graph. Containing itself
  through an array (`struct Node { kids: [Node] }`) is allowed, because an array field is a
  fixed-size pointer and length.
- **Declaration order doesn't matter.** LLVM struct types are created opaque first and given
  bodies afterwards, so fields can refer to structs declared later in the file.

---

## 4. Runtime safety and bounds-check elimination

### Checks

Array accesses are bounds-checked, and integer `/` and `%` are checked for zero. On failure
the program prints the source location and exits with code 101:

```text
runtime error at sort.nov:37:15: index 7 out of bounds for array of length 7
```

- **One compare per access.** The check is `icmp uge idx, len`. Treating the index as
  unsigned also catches negative indexes.
- **Cold failure path.** The panic block is marked cold with branch weights (1 : 2^20), so
  the hot path is a single predictable compare and branch, and code layout keeps the panic
  code out of the way.
- **`inbounds` GEPs.** Element addresses use `inbounds` GEPs. That's sound because the index
  was just checked or proven in range, and it lets alias analysis assume the arithmetic
  doesn't wrap.

### Elimination in the front end (`provablySafeRange` in sema)

The front end proves, from the source, that a check is redundant. The canonical case is a
loop whose index ranges over an array's own length:

```nova
for i in 0..len(xs) { s += xs[i]; }
```

There, `a[i]` is proven safe when all of the following hold:

1. `i` is the variable of a range loop whose start is non-negative and whose end is
   `len(a)`.
2. `a`'s length can't change during the loop. An array's length lives in the fat pointer
   held by its binding and is never mutated, and writing elements (`a[j] = v`) leaves it
   alone. Only rebinding the variable (`a = other`) could shrink it. So `a` qualifies if it's
   immutable, or if the loop body never assigns to `a` itself (`rebinds()`, which is
   conservative about shadowing).
3. Both `i` and `a` resolve to the same variables the loop was set up with. The check
   compares `VarInfo` pointers, so a shadowing `let i = 0` inside the body doesn't qualify.

A small **sign analysis** extends this to triangular loops (`for j in i + 1..len(a)`). If `i`
is the index of an enclosing safe loop, then `0 <= i < len(a) < 2^60`, because no array can
be that large. So `i + c`, for a small non-negative literal `c`, is non-negative and can't
overflow.

### Measured effect

`bench/run.sh`, best of three runs, on a 4-core Xeon at 2.1 GHz:

| | clang -O2 | Nova -O2 | Nova -O2 unchecked | Nova -O0 | -O0 `--no-bce` | -O0 unchecked |
| --- | --- | --- | --- | --- | --- | --- |
| nbody (50M steps) | 2.38s | 2.57s | 2.52s | 8.05s | 12.91s | 7.62s |
| sum (2G adds) | 0.59s | 0.59s | 0.61s | 2.75s | 3.25s | 2.65s |
| pairs (2.2G mul-adds) | 0.40s | 0.41s | 0.41s | 2.82s | 4.58s | 2.72s |

What the numbers show:

- **At -O2, safety costs 0–2%.** Even with `--no-bce`, no bounds check survives in the -O2 IR
  of `nbody` or `sum`. After inlining, LLVM's own passes prove them redundant. Where a check
  does survive, the cold, well-predicted branch is nearly free.
- **LLVM can't prove the triangular loop.** In `pairs`, with an array length LLVM can't see,
  LLVM leaves the `xs[j]` check in `for j in i + 1..len(xs)` in place, because it can't show
  that `i + 1` doesn't wrap. The front end's sign analysis proves it. At `-O2` the
  difference doesn't show in wall-clock time: the loop still vectorizes, and the one
  remaining check costs nothing measurable.
- **The front-end pass matters at -O0 and in the JIT.** There, elimination makes N-body and
  `pairs` about 1.6× faster (12.9s → 8.05s and 4.58s → 2.82s), within 2–6% of running with
  no checks at all. That's where fast feedback matters most: debug builds and `--run`.

The honest conclusion is that a modern optimizer recovers most of the cost of checks by
itself. The front-end analysis is cheap insurance for unoptimized code and for patterns
(like triangular loops) that the optimizer can't prove.

---

## 5. Code generation

- **Locals live in entry-block allocas.** Every local is an `alloca` in the entry block, and
  `mem2reg`/SROA promote them to SSA registers. This avoids building SSA by hand in the front
  end, at the cost of noisier `-O0` IR.
- **Arrays are `{ ptr, i64 }` fat pointers.** Element storage is `malloc`ed and never freed.
  Memory management is the biggest thing NovaLang deliberately doesn't do. The natural next
  step is reference counting or an arena per function, and value-semantics structs keep that
  door open.
- **Short-circuit `&&` and `||`** are lowered to branches that meet at a `phi`.
- **Symbols are mangled.** User functions are emitted as `nova.<name>` with internal
  linkage, and a generated C `main` calls `nova.main`. A NovaLang function can be called
  `printf` without clashing with libc, and internal linkage lets LLVM inline and delete
  freely.
- **`print` is static.** It's lowered by static type: the format string, including the unit
  suffix, is built at compile time, with a generated loop for arrays and recursion for nested
  structs.

### Two pipeline bugs found by benchmarking

The first benchmark run showed NovaLang about 15% behind C on N-body, and *nothing* was
vectorized, not even a plain array sum. Two separate causes:

1. **No target information.** The `PassBuilder` was built without a `TargetMachine`, so
   `TargetTransformInfo` fell back to a default cost model that reports a vector width of 1.
   The data layout was also only set at object-emission time, *after* optimization. The fix
   was to create the host `TargetMachine` first, set the module's data layout and triple
   from it, and pass it to the `PassBuilder`.
2. **Vectorization off by default.** A bare `PassBuilder` leaves `LoopVectorization` and
   `SLPVectorization` off in `PipelineTuningOptions`. Clang turns them on at `-O2`. I found
   this by running `opt -O2` on NovaLang's own unoptimized IR: it produced 127 vector
   instructions, while my in-process pipeline produced none.

After both fixes, N-body went from 2.85s to 2.57s (127 SLP-vectorized instructions), and
`sum` and `pairs` reached parity with clang. The remaining ~8% on N-body is mostly phase
ordering. In the C version `N = 5` is a literal, so clang fully unrolls the force loop before
inlining. In NovaLang the length becomes a constant only after `advance` is inlined into
`main`. Making C's `N` a runtime value costs C about 5%.

### Target CPU

`novac` builds for the host CPU and its features (the equivalent of `-march=native`), both
when emitting objects and in the JIT. Executables are fast on the build machine but may not
run on older CPUs. A `--target-cpu` flag would be the way to trade that back.

### JIT

`--run` compiles the module with LLVM ORC (`LLJIT`) and calls `main` in-process. External
symbols (`printf`, `malloc`, `exit`, `dprintf`) resolve against the running `novac` process
through `DynamicLibrarySearchGenerator`. CI runs the whole test suite through both the
ahead-of-time and JIT paths, at `-O0` and `-O2`, and requires identical output, exit codes and
runtime errors.

---

## 6. Diagnostics

All stages report through one `Diagnostics` object, which prints errors in the style of
clang and rustc, with the source line and a caret:

```text
prog.nov:6:16: error: argument 1 of 'sqrt' expects float<'u^2>, got float<m> (m has no exact square root)
  6 |     print(sqrt(9.0<m>));
    |                ^
```

Analysis continues after an error, so one run reports every error it can find. An unknown
type propagates as `Unknown`, which suppresses follow-on errors.

---

## 7. Testing

`tests/cases/` holds 371 programs. Each states its expected behavior in comments:

```nova
// expect: 5 m             expected stdout lines
// exit: 3                 expected exit code
// error: <text>           compilation must fail with this diagnostic
// runtime-error: <text>   must abort with exit code 101 and this message
// stats: <text>           `--stats` must report this (used to test bounds-check elimination)
```

The suite runs in four configurations: ahead-of-time and JIT, each at `-O0` and `-O2`.
Running the same programs across optimization levels and execution engines catches bugs
that a single configuration hides, such as undefined behavior in the generated IR that only
the optimizer exploits. CI also checks that the three benchmarks produce output identical to
their C counterparts.

---

## 8. What I'd do next

- **Memory management:** reference counting for arrays, or arena allocation per call.
- **Full unit inference:** Hermite normal form, to solve signatures that mix several unknown
  unit variables in one parameter.
- **Unit-polymorphic structs:** `struct Vec2<'u> { x: float<'u>, y: float<'u> }`.
- **Display units:** keep the programmer's alias (`N`, `J`) for printing and error messages.
- **Debug info:** emit DWARF line tables with `DIBuilder`, so gdb can step through `.nov`
  source.
