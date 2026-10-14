# GCD Compiled CycleSim Bug

## Summary

The compiled cyclesim returns the wrong value for the GCD function when one operand is smaller than the other and requires multiple iterations.

## Symptom

Test case `(17, 51)` fails:
- Expected: `gcd(17, 51) = 17`
- Actual: `gcd(17, 51) = 34`

The debug trace shows:
```
[gcd] a=17 b=51
[gcd] a=17 b=34
```
The loop exits after `b` becomes 17 (equal to `a`), but returns 34 instead of 17.

## Root Cause

The loop exit block in the CF MLIR returns the wrong phi argument.

### In the CF MLIR (`gcd.untimed.cyclesim.cf.mlir`)

The loop is defined with iter_args in order `(b, a)`:
```mlir
%hyperblock_r0, %hyperblock_r1 = ohls.untimed.loop iter_args(%phi_hyperblock_b_addr_03_hb = %phi_hyperblock_b_addr_03_hb : i32, %phi_hyperblock_a_addr_04_hb = %phi_hyperblock_a_addr_04_hb : i32) successors(^cfgpre_exit_dispatch) -> (i32, i32)
```

When the loop exits, it goes to `^cfgpre_exit_dispatch` which should return `a` (the GCD result):
```mlir
^cfgpre_exit_dispatch(%hyperblock_out0 : i32, %hyperblock_out1 : i32):
  %cfgpre_exit_dispatch_r0 = ohls.untimed.bb successors(^while_end) -> (i32) {
  ^cfgpre_exit_dispatch_body(%cfgpre_exit_dispatch_bb_arg0 : i32, %cfgpre_exit_dispatch_bb_arg1 : i32):
    ohls.seq.yield %cfgpre_exit_dispatch_bb_arg0 : i32  # <-- BUG: should be arg1
  } (%hyperblock_out0, %hyperblock_out1)
```

**The bug**: `ohls.seq.yield %cfgpre_exit_dispatch_bb_arg0` should be `%cfgpre_exit_dispatch_bb_arg1`.

The block arguments are `(b, a)` (matching the loop iter_args order), but the yield returns the first argument (`b`) instead of the second (`a`).

### In the Generated LLVM IR (`gcd.untimed.cyclesim.ll`)

The bug is visible in the phi nodes at block 29:
```llvm
29:                                               ; preds = %32
  %30 = phi i32 [ %33, %32 ]    ; this is 'b' - wrong!
  %31 = phi i32 [ %34, %32 ]    ; this is 'a' - correct but unused
  br label %22

22:                                               ; preds = %29, %2
  %23 = phi i32 [ %30, %29 ], [ %0, %2 ]  ; returns %30 which is 'b', not 'a'
  ret i32 %23
```

The phi node `%30` receives `%33` (the updated b value) instead of `%31` (the updated a value).

## Where to Spot

| File | Line | What to Look For |
|------|------|------------------|
| `output/sim/gcd.untimed.cyclesim.cf.mlir` | ~79 | `ohls.seq.yield %cfgpre_exit_dispatch_bb_arg0` - should be `arg1` |
| `output/sim/gcd.untimed.cyclesim.ll` | ~165 | `%30 = phi i32 [ %33, %32 ]` - wrong input to phi |

## Why Interpreted Works

The interpreted cyclesim directly executes the OHLS MLIR and correctly tracks the while loop termination, returning the proper value. The bug is specifically in the CF MLIR → LLVM IR lowering step where the loop exit phi is constructed incorrectly.

## Verification

- `make verify` **PASSES** - original C vs optimized C match
- Interpreted cyclesim **PASSES** - `gcd.ohls.cyclesim.l2.interp.out.txt` shows correct result
- Compiled cyclesim **FAILS** - returns wrong value due to phi node bug