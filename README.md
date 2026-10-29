# NovaLang

A statically typed language with **units of measure in the type system**, compiled to native
code through LLVM. The compiler is written in C++ (about 3,000 lines) with a Flex lexer, a
Bison parser, a semantic analyzer that does type and unit inference, and an LLVM backend that
emits native executables or runs programs in memory with the ORC JIT.

```nova
unit m;
unit s;

struct Vec2 { x: float<m>, y: float<m> }

fn square(x: float<'u>) -> float<'u^2> { return x * x; }          // unit-polymorphic
fn length(v: Vec2) -> float<m> { return sqrt(square(v.x) + square(v.y)); }

fn main() {
    let v = Vec2(x: 3.0<m>, y: 4.0<m>);
    let t = 2.0<s>;
    print(length(v) / t);        // 2.5 m/s
    // print(length(v) + t);     // error: operator '+' cannot be applied to float<m> and float<s>
    // print(sqrt(t));           // error: s has no exact square root
}
```

## Highlights

- **Dimensional analysis at compile time.** `+`, `-` and comparisons require matching units.
  `*` and `/` combine them, and units that cancel disappear. Units are erased before code
  generation, so they cost nothing at runtime.
- **Unit polymorphism with inference.** Functions can be generic over units:
  `fn sqrt(x: float<'u^2>) -> float<'u>`. At each call, the checker solves for `'u` over
  integer exponents, so `sqrt` of an area gives a length and `sqrt` of a length is a compile
  error. Because units are erased, one compiled function serves every instantiation.
- **Structs** with value semantics, nested field updates (`bodies[i].vel.x -= dv`), and
  arrays of structs.
- **Memory-safe arrays.** Array accesses are bounds-checked and integer division by zero is
  caught, with the source location reported and exit code 101. The front end proves many
  checks redundant (`for i in 0..len(a)`, triangular loops, and arrays whose binding is
  never reassigned) and removes them.
- **Two backends:** ahead-of-time native executables, or `--run` to JIT-compile and run in
  memory.
- **Performance at parity with C.** NovaLang is within measurement noise of clang on the
  array kernels and about 8% behind on N-body (see [Benchmarks](#benchmarks)).
- **Diagnostics** in the style of clang and rustc, showing the source line and a caret.
- **371 test programs**, run ahead-of-time and through the JIT, at `-O0` and `-O2`, in CI.

The design decisions and trade-offs are written up in [docs/DESIGN.md](docs/DESIGN.md).

## Building

Requirements: a C++17 compiler, CMake ≥ 3.16, LLVM 18 (with its development headers), Flex
and Bison.

```bash
sudo apt-get install -y llvm-18-dev flex bison cmake      # Ubuntu / Debian
cmake -S . -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build -j
```

## Usage

```bash
./build/novac examples/physics.nov --run          # JIT: compile and run in memory
./build/novac examples/physics.nov -O2            # writes physics.out
./build/novac bench/nbody.nov -O2 --stats         # report runtime checks emitted / elided
```

| Flag | Effect |
| --- | --- |
| `-o <file>` | Output executable (default: `<input>.out`). |
| `-O0` … `-O3` | Optimization level (default `-O0`). |
| `--run` | JIT-compile and run without writing a file. |
| `--emit-llvm` / `--emit-ast` | Print the LLVM IR or the syntax tree. |
| `--check` | Parse and type-check only. |
| `--stats` | Count the bounds and division checks that were emitted and elided. |
| `--no-bce` | Keep every bounds check (turns off front-end elimination). |
| `--unchecked` | Omit all runtime checks (for measuring what they cost). |

## Language tour

### Basics

```nova
let pi = 3.14159;              // immutable; type inferred as float
var count = 0;                 // mutable int
count += 1;
let label: string = "nova";
let half = count as float / 2.0;   // explicit casts, no implicit conversion
```

Types are `int`, `float`, `bool`, `string`, structs, and arrays `[T]`.

Control flow is `if` / `else if` / `else`, `while`, `for i in a..b` (end exclusive),
`for x in array`, `break`, `continue` and `return`.

Calls can be chained left to right with the pipeline operator: `x |> f(y)` means `f(x, y)`.

### Units

```nova
unit m;
unit s;
unit kg;
unit N = <kg*m/s^2>;           // alias, expanded to base units

let g = 9.81<m/s^2>;
let w: float<N> = 70.0<kg> * g;
print(w);                      // 686.7 kg*m/s^2
let raw = w as float;          // strip the unit (explicit)
```

### Unit polymorphism

```nova
fn mean(xs: [float<'u>]) -> float<'u> { ... }
fn ratio(a: float<'a>, b: float<'b>) -> float<'a/'b> { return a / b; }

mean([1.0<s>, 2.0<s>])         // float<s>
ratio(10.0<m>, 2.0<s>)         // float<m/s>
sqrt(16.0<m^2>)                // float<m>   (builtin: float<'u^2> -> float<'u>)
```

### Structs and arrays

```nova
struct Body { pos: Vec2, vel: float<m/s>, mass: float<kg> }

var bodies = [Body(pos: Vec2(x: 0.0<m>, y: 0.0<m>), vel: 1.0<m/s>, mass: 2.0<kg>); 10];
bodies[3].pos.x += 5.0<m>;     // nested field update through a GEP path
print(len(bodies));
for b in bodies { print(b.mass); }

fn scale(var xs: [float<m>], k: float) {    // `var` parameter: the function may write elements
    for i in 0..len(xs) { xs[i] *= k; }     // bounds check proven redundant and removed
}
```

### Errors

```text
prog.nov:6:16: error: argument 1 of 'sqrt' expects float<'u^2>, got float<m> (m has no exact square root)
  6 |     print(sqrt(9.0<m>));
    |                ^
runtime error at sort.nov:37:15: index 7 out of bounds for array of length 7
```

## Benchmarks

Produced by `bench/run.sh`. C is built with clang `-O2 -march=native`, so both compilers use
the same LLVM backend; the times are the best of three runs. They were measured on a shared
4-core cloud VM, where differences under about 10% are noise.

| | clang -O2 | Nova -O2 | Nova -O2 unchecked | Nova -O0 | -O0 `--no-bce` | -O0 unchecked |
| --- | --- | --- | --- | --- | --- | --- |
| nbody | 2.35s | 2.53s | 2.49s | 8.03s | 12.76s | 7.65s |
| sum | 0.65s | 0.65s | 0.62s | 2.66s | 3.22s | 2.92s |
| pairs | 0.42s | 0.33s | 0.37s | 2.39s | 4.50s | 2.74s |

- At `-O2`, the runtime safety checks cost about 0–2%.
- At `-O0` (and in the JIT), front-end bounds-check elimination makes programs 1.6–1.9×
  faster and brings them to within noise of having no checks at all.
- The N-body version uses units throughout (positions in AU, time in years). It prints the
  same energies as the C version, and CI checks that the outputs match.

[docs/DESIGN.md](docs/DESIGN.md) explains what these numbers mean, including the two LLVM
pipeline bugs the benchmarks exposed.

## Tests

```bash
python3 tests/run_tests.py build/novac tests/cases                    # AOT, -O0
NOVAC_FLAGS=-O2 python3 tests/run_tests.py build/novac tests/cases    # AOT, -O2
NOVAC_MODE=jit python3 tests/run_tests.py build/novac tests/cases     # JIT
```

Each test program states what it expects in comments: `// expect:` for output, `// error:` for
compile errors, `// runtime-error:` for runtime errors, and `// stats:` for check counts.
