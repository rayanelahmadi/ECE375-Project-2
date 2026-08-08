# RISC-V Cycle-Accurate Pipeline Simulator

A cycle-accurate simulator for a classic five-stage pipelined RISC-V (RV64I subset)
processor, written in C++. It models not just *what* a program computes, but *how long*
the hardware takes to compute it — tracking every stall, forward, squash, cache miss and
exception, cycle by cycle.

Built for **COS/ECE 375 (Computer Architecture and Organization), Fall 2025** — Project 2 /
final project. Project 1 was a purely functional simulator (correct results, no notion of
time); this project adds the microarchitecture.

> **Which branch is which:** the finished simulator lives on **`attempt-2`**. The default
> branch, `master`, is close to the original course-provided starter template and is *not*
> the completed work.

---

## What it does

Given a RISC-V binary and a cache configuration file, the simulator runs the program
through a simulated IF → ID → EX → MEM → WB pipeline and emits:

- a **cycle-by-cycle pipeline trace** (what instruction sat in each stage, and why a stage
  was empty — bubble, squash, idle, or speculative),
- the **final architectural state** (all 32 registers and a region of memory), which is
  guaranteed to match a correct functional simulator, and
- **performance statistics**: dynamic instruction count, total cycles, I-cache and D-cache
  hits/misses, and load stalls.

## Microarchitecture modeled

**Pipeline and hazards**
- Five stages: Fetch, Decode, Execute, Memory, Writeback.
- **Full forwarding** — MEM→EX, WB→EX, and EX/MEM/WB→ID for branch operands. Includes the
  WB→MEM special case that forwards a loaded value straight into a store's data input, so
  `ld` immediately followed by `sd` of that value costs no stall.
- **Stalls**: one-cycle load-use, one-cycle arithmetic-branch, two-cycle load-branch. A
  stall injects a bubble that holds the blocked instruction while older instructions keep
  draining down the pipe.
- Dependencies on `x0` are correctly ignored — writing to the zero register never triggers
  a stall.

**Control**
- Branches resolve in **ID**, with an **always-not-taken** prediction policy. IF
  speculatively fetches PC+4; when the branch turns out to be taken, that speculative
  instruction is squashed before it ever enters ID.

**Caches** (`src/cache.cpp`, `src/cache.h`)
- Separate L1 instruction and data caches, each configurable at runtime for size, block
  size, associativity, and miss penalty.
- **True LRU** replacement (a monotonically increasing recency counter per line, not an
  approximation), write-allocate, write-through, and stall-on-miss for both reads and
  writes.
- Set index / tag / block-offset geometry is derived from the config at construction time.
- Miss timing is integrated into the pipeline: an I-cache miss holds the instruction in IF
  for `missLatency` extra cycles while older instructions continue and NOPs fill in behind
  them; a D-cache miss latches the instruction in MEM and stalls everything upstream.

**Exceptions**
- **Illegal instructions** are detected in ID and squashed before EX.
- **Memory exceptions** (out-of-range access) are detected in MEM and squashed before WB.
- Either way the core traps to the handler at **`0x8000`**: the faulting instruction never
  updates architectural state, every older instruction in the pipe completes normally,
  every younger one is squashed, and IF redirects on the next cycle.

**Halting**
- RISC-V has no HALT, so the sentinel word `0xfeedfeed` ends the program — the simulator
  stops once it retires from WB.

## Repository layout

```
src/
  cycle.cpp        ← the heart of the project: pipeline timing, hazards,
                     forwarding, squashes, cache-miss stalls, exception redirect
  cache.cpp/.h     ← LRU set-associative cache model
  simulator.cpp/.h ← per-stage simulation (simIF/simID/simEX/simMEM/simWB) built
                     on the Project 1 functional primitives
  funct.cpp        ← functional-only driver (Project 1)
  sim_cycle.cpp    ← main() for the cycle-accurate simulator
  sim_funct.cpp    ← main() for the functional simulator
  MemoryStore.*, RegisterInfo.h, Utilities.*   ← course-provided infrastructure
test/              ← assembly test programs (*.s) and reference outputs (*.ref)
bin/               ← bundled riscv64-elf assembler / objcopy / objdump
Makefile           ← build rules for both simulators and all assembly tests
```

Of these, `cycle.cpp`, `cache.{cpp,h}`, and `simulator.{cpp,h}` are the files we wrote;
the rest is the course-provided harness.

## Building

```sh
make sim_cycle    # cycle-accurate simulator
make sim_funct    # functional simulator (Project 1 reference)
make tests        # assemble every test/*.s into a .bin
make all          # all of the above
make clean
```

> **Note:** the provided Makefile guards on hostname and only builds on the Princeton
> Nobel machines (`davisson`/`compton`), since that's where the project is graded. To build
> elsewhere, drop that check — everything else is portable C++14 with no dependencies
> beyond a compiler.

## Running

```sh
./sim_cycle <file.bin> <cache_config.txt>
./sim_funct <file.bin>
```

For example:

```sh
make tests
./sim_cycle test/fib.bin test/cache_config.txt
```

This writes four files named after the binary:

| File | Contents |
| --- | --- |
| `fib_cycle_pipe_state.out` | per-cycle contents of all five pipeline stages |
| `fib_cycle_sim_stats.out`  | instructions, cycles, cache hits/misses, load stalls |
| `fib_cycle_reg_state.out`  | final register file |
| `fib_cycle_mem_state.out`  | final memory contents |

Diff those against the matching `test/*.ref` files to check correctness.

### Cache configuration

The config file is eight numbers — four for the I-cache, then four for the D-cache
(size in bytes, block size in bytes, ways, miss penalty in cycles). Text after each number
is ignored. `test/cache_config.txt`:

```
2048   # [ICache]  2K instruction cache
16     #           16-byte blocks
2      #           2-way set associative
5      #           5-cycle miss penalty
4096   # [DCache]  4K data cache
16     #           16-byte blocks
4      #           4-way set associative
8      #           8-cycle miss penalty
```

## Tests

Beyond the two programs supplied with the assignment (`fib.s`, `illegal.s`), we wrote
targeted tests that each isolate one microarchitectural behavior:

| Test | What it exercises |
| --- | --- |
| `stall_load_use.s` | one-cycle load-use stall |
| `stall_arith_branch.s` | one-cycle arithmetic-to-branch stall |
| `stall_load_branch.s` | two-cycle load-to-branch stall |
| `stall_two_load_branch.s` | a branch depending on two loads — should count as *two* load stalls |
| `zero_load_use.s` | load into `x0` must **not** stall |
| `zero_arith_branch.s` | arithmetic into `x0` before a branch must **not** stall |
| `fwd_load_store.s` | WB→MEM forwarding from a load into a store's data input |
| `fwd_arith_store.s` | forwarding an ALU result to a store at EX |
| `dcache_hit.s` | cold miss followed by repeat hits |
| `dcache_write_alloc.s` | write-allocate: store misses, subsequent load hits |
| `cache_conflict.s` | fills one D-cache set, re-touches a line, and checks that the eviction picks the true LRU way |
| `icache_conflict.s` | alternates between two code regions that map to the same I-cache set |
| `illegal_id.s` | illegal opcode detected in ID, trap to `0x8000` |
| `mem_exc.s` | out-of-range load detected in MEM, trap to `0x8000` |

## Authors

- Max Machado (mm5451)
- Rayan Elahmadi (re2099)
- Bobby Diaz (rd8921)

See `Partners.md` for the course submission notes, and `project2.pdf` for the full
assignment specification.
