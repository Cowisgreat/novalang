# NovaLang

A statically typed programming language with **units of measure checked at compile time**.
It is compiled to native code by a full pipeline written in C++: a **Flex** lexer, a **Bison**
parser, a semantic analyzer with type and dimension inference, and an **LLVM** backend that turns
`.nov` source files into `.out` executables.

```nova
unit m;
unit s;

fn speed(distance: float<m>, time: float<s>) -> float<m/s> {
    return distance / time;
}

fn main() {
    let v = speed(100.0<m>, 9.58<s>);
    print(v);                        // 10.4384 m/s
    // print(v + 1.0<m>);            // compile error: units do not match
}
```

## What makes it different

- **Dimensional analysis in the type system.** Physical units are part of a number's type.
  `+`, `-` and comparisons require matching units; `*` and `/` combine them
  (`float<m/s> * float<s>` is `float<m>`), and units that cancel out disappear. Mixing up meters
  and seconds is a compile error, like the bug that destroyed the Mars Climate Orbiter.
  Units are erased before code generation, so they cost nothing at runtime.
- **Runtime safety with source locations.** Array accesses are bounds-checked and integer
  division by zero is caught. Either one stops the program with a precise location and exit
  code 101, instead of undefined behavior:
  `runtime error at sort.nov:37:15: index 7 out of bounds for array of length 7`.
  Division by a literal zero is caught at compile time.
- **Pipeline operator.** `data |> normalize |> sum(0) |> print;` reads left to right and
  desugars to `print(sum(normalize(data), 0))`.

## Building

Requirements: a C++17 compiler, CMake ≥ 3.16, LLVM 18 (dev headers), Flex and Bison.

```bash
# Ubuntu / Debian
sudo apt-get install -y llvm-18-dev flex bison cmake

cmake -S . -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build -j
```

This produces the compiler at `build/novac`.

## Usage

```bash
./build/novac examples/physics.nov             # -> physics.out
./build/novac examples/physics.nov -o phys -O2 # optimized, custom output name
./physics.out
```

| Flag | Effect |
| --- | --- |
| `-o <file>` | output executable (default: `<input>.out`) |
| `-O0` … `-O3` | LLVM optimization level (default `-O0`) |
| `--emit-llvm` | print the generated LLVM IR instead of compiling |
| `--emit-ast` | print the parsed syntax tree |
| `--check` | parse and type-check only |

## The language

### Types and variables

`int` (64-bit), `float` (64-bit), `bool`, `string`, `void`, and arrays `[T]`. `let` bindings are
immutable and `var` bindings are mutable. Type annotations are optional; when one is left out,
the type is inferred from the initializer.

```nova
let pi = 3.14159;          // float
let name: string = "nova";
var count = 0;             // int, mutable
count += 1;
```

There are no implicit conversions; `as` converts between `int`, `float` and `bool`.

### Units of measure

Declare base units, or aliases that expand into base units, at the top level:

```nova
unit m;
unit s;
unit kg;
unit N = <kg*m/s^2>;
unit J = <N*m>;
```

Attach a unit to a literal or a type with `<...>`, written without spaces: `9.81<m/s^2>`,
`float<J>`, `int<s>`, `[float<m>]`. A unit expression is a product of units with optional integer
exponents: `kg*m^2/s^2`, `1/s`, `m*s^-1`.

| Expression | Rule |
| --- | --- |
| `a + b`, `a - b`, `a < b`, `a == b` | units must be identical |
| `a * b`, `a / b` | units multiply / divide (`m / s` → `m/s`, `m / m` → dimensionless) |
| `a % b` | `int` only, identical units |
| `-a` | unit preserved |
| `x as float` | strips the unit (explicit escape hatch) |
| `x as float<m>` | attaches a unit |

Values print with their unit: `print(2.0<N>)` → `2 kg*m/s^2`. Aliases are always shown in
base units.

### Arrays

```nova
let primes = [2, 3, 5, 7];
var grid = [0.0; n];          // n copies of 0.0; n can be computed at runtime
grid[2] = 1.5;
print(len(primes));           // 4
for p in primes { print(p); }
print(primes);                // [2, 3, 5, 7]
```

Arrays are reference values (a pointer and a length), so passing one to a function does not copy
it. Writing to an element requires a `var` binding. For a parameter, that means declaring it
`var`:

```nova
fn fill(var xs: [int], value: int) {
    for i in 0..len(xs) { xs[i] = value; }
}
```

### Functions

Functions declare their parameter types and an optional return type (the default is `void`).
They can be called before they are defined, and they can be recursive or mutually recursive.

```nova
fn area(w: float<m>, h: float<m>) -> float<m^2> { return w * h; }
fn greet() { print("hi"); }
```

`x |> f(a, b)` is `f(x, a, b)`, and `x |> f` is `f(x)`. The pipeline has the lowest precedence of
any operator.

### Control flow and operators

- Control flow: `if` / `else if` / `else`, `while`, `for i in start..end` (end exclusive),
  `for x in array`, `break`, `continue`, `return`.
- Operators: `+ - * / %`, `< <= > >= == !=`, short-circuiting `&& ||`, unary `-` and `!`, and
  compound assignment `+= -= *= /= %=`.
- Builtins: `print(x)` prints any value followed by a newline. `len(a)` returns an array's length.
- Program entry: `fn main() -> int` (the return value becomes the exit code) or `fn main()`.

### Compile-time errors

The semantic analyzer reports every error it finds, with file, line and column:

```text
prog.nov:6:18: error: operator '+' cannot be applied to float<m> and float<s> (units do not match)
prog.nov:9:5: error: cannot assign to immutable variable 'x' (declare it with 'var' to make it mutable)
prog.nov:12:12: error: returning float<m*s> from a function declared to return float<m/s>
3 error(s) generated.
```

It checks for:

- undeclared names and unknown units
- type and unit mismatches
- assignment to immutable bindings
- wrong argument counts or types
- `break` or `continue` outside a loop
- compound assignments that would change a variable's unit (`d *= 2.0<m>` on a `float<m>`)
- functions that can finish without returning a value

## Architecture

```text
 .nov source
     │
     ▼
 lexer.l   (Flex)    tokens with line/column; unit annotations are parsed into factor maps
     │
     ▼
 parser.y  (Bison)   LALR(1) grammar (no conflicts) → AST (ast.h); `|>` is desugared here
     │
     ▼
 sema.cpp            scopes, type checking & inference, unit resolution and dimensional
     │               analysis, return-path analysis, constant division-by-zero detection
     ▼
 codegen.cpp (LLVM)  AST → LLVM IR (units erased) with bounds/zero checks
     │               → optional O1–O3 pipeline → native object file
     ▼
 cc (system linker)  → executable .out
```

How the backend works:

- A unit is stored as a map from base-unit name to an integer exponent (`kg*m/s^2` =
  `{kg:1, m:1, s:-2}`). Multiplying units adds exponents and dividing subtracts them; an exponent
  that reaches zero is dropped. Aliases are expanded at declaration, so `N` and `kg*m/s^2` are the
  same type.
- Locals live in `alloca`s in the entry block, so LLVM's `mem2reg` promotes them to SSA registers.
- Arrays lower to an `{ ptr, i64 }` fat pointer. The element storage is `malloc`ed.
- Each runtime check branches to a cold `panic` block (marked with branch weights) that calls
  `dprintf(2, …)` and `exit(101)`, so the hot path stays a single compare and branch.
- `&&` and `||` are lowered to branches with a `phi`, which gives true short-circuit evaluation.
- User functions get a `nova.` prefix and internal linkage, so a NovaLang function can be called
  `printf` or `main` without clashing with libc. A generated C `main` calls `nova.main`.

## Tests

`tests/cases/` holds 289 test programs covering:

- literals, type inference, operators, casts, scoping and control flow
- functions and recursion
- units of measure (derivation, aliases, cancellation, and every unit error)
- arrays, the pipeline operator and compound assignment
- runtime panics and their source locations
- every compile-time diagnostic

Each file states what it expects in comments:

```nova
// expect: 10.4384 m/s
// runtime-error: index 3 out of bounds for array of length 3
// error: operator '+' cannot be applied to float<m> and float<s>
```

Run the suite:

```bash
python3 tests/run_tests.py build/novac tests/cases
NOVAC_FLAGS=-O2 python3 tests/run_tests.py build/novac tests/cases   # with optimizations
# or: cd build && ctest --output-on-failure
```

On every push and pull request, CI (`.github/workflows/ci.yml`) builds the compiler, runs the
whole suite at `-O0` and at `-O2`, and compiles and runs every example.
