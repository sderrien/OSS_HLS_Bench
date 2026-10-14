# Early-Exit CFG Stress Example

This tutorial case is a stress test for hyperblock construction on loop bodies that combine:

- loop-header `switch` dispatch,
- multiple early loop exits (`break`),
- and a `continue` path that jumps directly to the latch.

The kernel (`earlyexit_kernel.c`) intentionally avoids `goto` and keeps all control flow structured.

## Files

- `earlyexit_kernel.c`: kernel with `switch` + early exits.
- `main_earlyexit.c`: self-checking host test that compares kernel vs reference on multiple input cases.
- `Makefile`: local tutorial flow entrypoint.

## Run

```bash
make run-orig
make frontend
make run-opt
make verify
```

`main_earlyexit.c` prints per-case comparisons and returns non-zero if any case mismatches.

Note: this workload is intended to expose current hyperblock limitations; depending on the current compiler state, `run-opt/verify` may fail while `run-orig` passes.
