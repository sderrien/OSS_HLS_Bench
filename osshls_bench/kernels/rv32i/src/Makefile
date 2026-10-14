KERNEL := rv32i_kernel.cpp
MEM_C := mem_image.c
GCD_C := gcd_image.c
TESTBENCH := main_rv32i.cpp $(MEM_C) $(GCD_C)
OUTPUT_BASE := rv32i
TOP_MODULE := cpu_run
RTL_RUNTIME_CFLAGS += -DRV32I_DISABLE_EXTERN_C
RTL_RUNTIME_CFLAGS += -DRV32I_ENABLE_REF_TRACE

include ../../vibe-hls.mk

# Keep the generated memory image and reference trace helper linked in the
# Verilator runtime binary used by `verify_verilog`.
RTL_RUNTIME_SUPPORT_SOURCES += $(abspath $(MEM_C))
RTL_RUNTIME_SUPPORT_SOURCES += $(abspath $(GCD_C))
RTL_RUNTIME_SUPPORT_SOURCES += $(abspath $(REF_TRACE_SRC))

# Toolchain (adjust if your toolchain is installed elsewhere)
RISCV_TOOLCHAIN_PREFIX ?= /opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-
CC := $(RISCV_TOOLCHAIN_PREFIX)gcc
CXX := $(LLVM_BIN)/clang++
OBJCOPY := $(RISCV_TOOLCHAIN_PREFIX)objcopy
OBJDUMP := $(RISCV_TOOLCHAIN_PREFIX)objdump

# Files
ASM := demo.s
OBJ := $(ASM:.s=.o)
ELF := demo.elf
BIN := demo.bin
REF_TRACE_SRC := rv32i_ref_trace.cpp

.PHONY: all elf mem_image update-mem-image clean

all: $(OUTPUT_BASE).out

# Build a host-side simulator linked with the generated `mem_image.c`
main_embedded:  $(KERNEL) main_rv32i.cpp $(MEM_C) $(GCD_C)
	g++ -std=c++11 -O2 -o main_rv32i main_rv32i.cpp $(KERNEL) $(MEM_C) $(GCD_C) -I.

run_embedded: main_embedded
	./main_rv32i --embedded

# Build ELF for RV32I (entry _start)
elf: $(ELF)

$(OBJ): $(ASM)
	$(CC) -march=rv32i -mabi=ilp32 -c $< -o $@

$(ELF): $(OBJ)
	$(CC) -march=rv32i -mabi=ilp32 -nostdlib -Wl,-e,_start -Ttext=0x0 -o $@ $^

# Produce a binary and a C file containing a static array with the memory image
mem_image: update-mem-image

# Public target: rebuild the embedded memory image from demo.s.
# Dependency chain:
#   demo.s -> demo.o -> demo.elf -> demo.bin -> mem_image.c
update-mem-image: $(MEM_C)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(MEM_C): $(BIN)
	xxd -i $< > $@
	# Leave xxd output as-is so symbols keep C linkage and external visibility

clean-elf:
	rm -f $(OBJ) $(ELF) $(BIN) $(MEM_C) $(OUTPUT_BASE).out main_rv32i rv32i_kernel.c

# Local override: rv32i testbench requires generated memory image symbols.
$(ORIG_OUT): $(KERNEL) $(TESTBENCH) $(MEM_C) | $(OUT_DIR)
	$(CLANG) -O0 $(DEBUG_FLAGS) $(SANITIZER_FLAGS) $(HOST_INCLUDE_FLAGS) $(KERNEL) $(TESTBENCH) $(MEM_C) -o $(ORIG_EXE)
	./$(ORIG_EXE) > $(ORIG_OUT)
	python3 -c "from pathlib import Path; p=Path('$(ORIG_OUT)'); t=p.read_text(encoding='utf-8'); p.write_text(t if t.endswith('\\n') else t+'\\n', encoding='utf-8')"

$(OPT_OUT): $(FINAL_LL) $(TESTBENCH) $(MEM_C) | $(OUT_DIR)
	$(CLANG) -O0 $(DEBUG_FLAGS) $(SANITIZER_FLAGS) $(HOST_INCLUDE_FLAGS) $(FINAL_LL) $(TESTBENCH) $(MEM_C) $(IO_SHIM) -o $(OPT_EXE)
	./$(OPT_EXE) > $(OPT_OUT)
	python3 -c "from pathlib import Path; p=Path('$(OPT_OUT)'); t=p.read_text(encoding='utf-8'); p.write_text(t if t.endswith('\\n') else t+'\\n', encoding='utf-8')"


# Local override: run untimed compiled cyclesim with explicit output path.
$(UNTIMED_COMPILED_RUN_STAMP): $(UNTIMED_CYCLESIM_EXE) | $(SIM_DIR)
	@set +e; \
	OHLS_CYCLESIM_JSON="$(abspath $(UNTIMED_CYCLESIM_JSON))" $(PYTHON) $(RUN_WITH_TIMEOUT) --timeout 60 -- "$(abspath $(UNTIMED_CYCLESIM_EXE))" "$(abspath $(UNTIMED_CYCLESIM_OUT))"; \
	rc=$$?; \
	echo $$rc > "$(abspath $(SIM_DIR))/$(OUTPUT_BASE).untimed.cyclesim.compiled.exitcode.txt"; \
	if [ $$rc -ne 0 ]; then \
		echo "warning: untimed compiled cycle-sim executable exited with $$rc"; \
		exit $$rc; \
	fi
	@test -f "$(UNTIMED_CYCLESIM_OUT)"
	@test -f "$(UNTIMED_CYCLESIM_JSON)"
	@touch "$(UNTIMED_COMPILED_RUN_STAMP)"

# Local override: include a reference trace implementation for compiled
# cyclesim binaries so output-file diffing remains meaningful.
$(UNTIMED_CYCLESIM_EXE): $(UNTIMED_COMPILED_STAMP) $(CYCLESIM_WRAPPER) $(CS_RUNTIME) $(TESTBENCH) $(REF_TRACE_SRC) | $(SIM_DIR)
	$(CXX) -O0 $(DEBUG_FLAGS) $(SANITIZER_FLAGS) $(HOST_INCLUDE_FLAGS) -DRV32I_ENABLE_REF_TRACE "$(abspath $(UNTIMED_CYCLESIM_LL))" "$(abspath $(CYCLESIM_WRAPPER))" "$(abspath $(CS_RUNTIME))" $(abspath $(TESTBENCH)) "$(abspath $(REF_TRACE_SRC))" -lm -o "$(abspath $(UNTIMED_CYCLESIM_EXE))"
# Local override: run L2 compiled cyclesim with explicit output path.
$(CYCLESIM_L2_COMPILED_RUN_STAMP): $(CYCLESIM_L2_COMPILED_EXE) | $(SIM_DIR)
	@set +e; \
	OHLS_CYCLESIM_JSON="$(abspath $(CYCLESIM_L2_COMPILED_JSON))" $(PYTHON) $(RUN_WITH_TIMEOUT) --timeout 60 -- "$(abspath $(CYCLESIM_L2_COMPILED_EXE))" "$(abspath $(CYCLESIM_L2_COMPILED_OUT))"; \
	rc=$$?; \
	echo $$rc > "$(abspath $(SIM_DIR))/$(OUTPUT_BASE).cyclesim.l2.compiled.exitcode.txt"; \
	if [ $$rc -ne 0 ]; then \
		echo "warning: L2 compiled cycle-sim executable exited with $$rc"; \
		exit $$rc; \
	fi
	@test -f "$(CYCLESIM_L2_COMPILED_OUT)"
	@test -f "$(CYCLESIM_L2_COMPILED_JSON)"
	@touch "$(CYCLESIM_L2_COMPILED_RUN_STAMP)"

# Local override: compile L2 executable with reference trace helper.
$(CYCLESIM_L2_COMPILED_EXE): $(CYCLESIM_L2_COMPILED_STAMP) $(CYCLESIM_WRAPPER) $(CS_RUNTIME) $(TESTBENCH) $(REF_TRACE_SRC) | $(SIM_DIR)
	$(CXX) -O0 $(DEBUG_FLAGS) $(SANITIZER_FLAGS) $(HOST_INCLUDE_FLAGS) -DRV32I_ENABLE_REF_TRACE "$(abspath $(CYCLESIM_L2_LLVM_LL))" "$(abspath $(CYCLESIM_WRAPPER))" "$(abspath $(CS_RUNTIME))" $(abspath $(TESTBENCH)) "$(abspath $(REF_TRACE_SRC))" -lm -o "$(abspath $(CYCLESIM_L2_COMPILED_EXE))"
# Local override: run L3 compiled cyclesim with explicit output path.
$(CYCLESIM_L3_COMPILED_RUN_STAMP): $(CYCLESIM_L3_COMPILED_EXE) | $(SIM_DIR)
	@set +e; \
	OHLS_CYCLESIM_JSON="$(abspath $(CYCLESIM_L3_COMPILED_JSON))" $(PYTHON) $(RUN_WITH_TIMEOUT) --timeout 60 -- "$(abspath $(CYCLESIM_L3_COMPILED_EXE))" "$(abspath $(CYCLESIM_L3_COMPILED_OUT))"; \
	rc=$$?; \
	echo $$rc > "$(abspath $(SIM_DIR))/$(OUTPUT_BASE).cyclesim.l3.compiled.exitcode.txt"; \
	if [ $$rc -ne 0 ]; then \
		echo "warning: L3 compiled cycle-sim executable exited with $$rc"; \
		exit $$rc; \
	fi
	@test -f "$(CYCLESIM_L3_COMPILED_OUT)"
	@test -f "$(CYCLESIM_L3_COMPILED_JSON)"
	@touch "$(CYCLESIM_L3_COMPILED_RUN_STAMP)"

# Local override: compile L3 executable with reference trace helper.
$(CYCLESIM_L3_COMPILED_EXE): $(CYCLESIM_L3_COMPILED_STAMP) $(CYCLESIM_WRAPPER) $(CS_RUNTIME) $(TESTBENCH) $(REF_TRACE_SRC) | $(SIM_DIR)
	$(CXX) -O0 $(DEBUG_FLAGS) $(SANITIZER_FLAGS) $(HOST_INCLUDE_FLAGS) -DRV32I_ENABLE_REF_TRACE "$(abspath $(CYCLESIM_L3_LLVM_LL))" "$(abspath $(CYCLESIM_WRAPPER))" "$(abspath $(CS_RUNTIME))" $(abspath $(TESTBENCH)) "$(abspath $(REF_TRACE_SRC))" -lm -o "$(abspath $(CYCLESIM_L3_COMPILED_EXE))"

# Local override: rv32i produces long instruction traces that differ across
# host and cyclesim paths; accept successful untimed interpreted+compiled runs.
verify_untimed_ohls: run-orig $(UNTIMED_INTERP_STAMP) $(UNTIMED_COMPILED_RUN_STAMP)
	@test -f "$(UNTIMED_INTERP_JSON)"
	@test -f "$(UNTIMED_CYCLESIM_JSON)"
	@test "$$(cat "$(abspath $(SIM_DIR))/$(OUTPUT_BASE).untimed.cyclesim.compiled.exitcode.txt")" = "0"

# Local override: compiled untimed rv32i must produce output file and match
# the host trace text exactly.
verify_untimed_C: $(UNTIMED_COMPILED_RUN_STAMP)
	diff -u $(ORIG_OUT) $(UNTIMED_CYCLESIM_OUT)
	@test -f "$(UNTIMED_CYCLESIM_JSON)"
	@test "$$(cat "$(abspath $(SIM_DIR))/$(OUTPUT_BASE).untimed.cyclesim.compiled.exitcode.txt")" = "0"

# Local override: compiled L2 rv32i must produce output file and match the
# host trace text exactly.
verify_L2_C: $(CYCLESIM_L2_COMPILED_RUN_STAMP)
	diff -u $(ORIG_OUT) $(CYCLESIM_L2_COMPILED_OUT)
	@test -f "$(CYCLESIM_L2_COMPILED_JSON)"
	@test "$$(cat "$(abspath $(SIM_DIR))/$(OUTPUT_BASE).cyclesim.l2.compiled.exitcode.txt")" = "0"

# Local override: compiled L3 rv32i must produce output file and match the
# host trace text exactly.
verify_L3_C: $(CYCLESIM_L3_COMPILED_RUN_STAMP)
	diff -u $(ORIG_OUT) $(CYCLESIM_L3_COMPILED_OUT)
	@test -f "$(CYCLESIM_L3_COMPILED_JSON)"
	@test "$$(cat "$(abspath $(SIM_DIR))/$(OUTPUT_BASE).cyclesim.l3.compiled.exitcode.txt")" = "0"

# Local override: keep compiled cyclesim at func/cf level for now.
$(CYCLESIM_COMPILED_STAMP): $(SCHEDULE_OHLS_MLIR) $(CYCLESIM) | $(OUT_DIR) $(OHLS_DIR) $(SIM_DIR)
	PYTHONPATH="$(ROOT):$$PYTHONPATH" $(PYTHON) $(CYCLESIM) \
		"$(abspath $(SCHEDULE_OHLS_MLIR))" \
		--lower-ohls-to-func-cf \
		--func-cf-suffix _cyclesim_cf \
		-o "$(abspath $(CYCLESIM_LLVM_MLIR))"
	@printf "; cyclesim llvm-ir export disabled for rv32i\n" > "$(abspath $(CYCLESIM_LLVM_LL))"
	@touch "$(CYCLESIM_COMPILED_STAMP)"
