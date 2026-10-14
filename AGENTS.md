When you are about to make code changes to implement something, always describe what you plan to do, and then ask for confirmation before starting modifying code.

When fixing a problem, do not fix it by make the problem disappear because of a change in the input problem, or thourgh some fallback that skips/ignore the problem. In case of dpoubt ask forn confirmation.

When analyzing or transforming LLVM IR or MLIR, do not use regex, string splitting, or line-oriented parsing  use xDSL APIs orr llvm bindings

Always add detailled comments & explanation the role/goal of each function in source code, also a summary of the role of the pythin/C++ module in teh flow (why/what/how)

Always make separate commits, when committing, use a detailled commit messages, explaining precisyley the changes (why/what/how)

For cyclecount, you have to rely on the tool ablilty to provide acccurate/exact cycle count for the execution, or by running RTL simulation and count cycle from there. All tools should test with same inputs to ne be biased.


All tools should operate from kernels in ./benchmarks. All tools should use the same testbench input so that #cyclecount is not apple to orange comparison

When you are about to make code changes to implement something, always describe what you plan to do, and then ask for confirmation before starting modifying code.
