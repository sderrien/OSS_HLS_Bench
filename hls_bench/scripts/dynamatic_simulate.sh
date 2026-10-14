#!/bin/bash

# Repo-local Dynamatic simulation wrapper.
#
# Role:
# Run Dynamatic's C/RTL co-simulation stage while fixing the local Vivado-FPU
# Verilog/XSim integration gap observed with Vivado 2023.2.
#
# Goal:
# Preserve Dynamatic's generated kernel HDL and hls-verifier flow, but make exact
# RTL simulation usable when the Verilog backend instantiates Vivado floating
# point operators.  This wrapper does not alter benchmark sources, skip failed
# computations, or replace simulation with an estimate; it only supplies the HDL
# simulator with the current Vivado IP simulation models required by the design.
#
# How:
# The script mirrors Dynamatic's installed tools/dynamatic/scripts/simulate.sh.
# For non-XSim or non-Vivado-FPU runs it behaves the same as the installed script.
# For Vivado-FPU Verilog runs under XSim, it generates current Vivado floating
# point IP simulation wrappers in the benchmark simulation directory, removes the
# stale static *_vivado_support.vhd files from the copied simulation HDL, and
# shadows xelab with a tiny wrapper that adds the Vivado precompiled libraries
# needed by floating_point_v7_1_16 and unisim primitives.

set -euo pipefail

source "$1"/tools/dynamatic/scripts/utils.sh

# ============================================================================ #
# Variable definitions
# ============================================================================ #

# Script arguments; kept positional-compatible with Dynamatic's installed script.
DYNAMATIC_DIR=$1
SRC_DIR=$2
OUTPUT_DIR=$3
KERNEL_NAME=$4
VIVADO_PATH=$5
VIVADO_FPU=$6
SIMULATOR_NAME=$7
HDL_TYPE=$8
TIMEOUT=$9

# Generated directories/files.
SIM_DIR="$(realpath "$OUTPUT_DIR/sim")"
C_SRC_DIR="$SIM_DIR/C_SRC"
C_OUT_DIR="$SIM_DIR/C_OUT"
COSIM_HDL_SRC_DIR="$SIM_DIR/HDL_SRC"
COSIM_HDL_OUT_DIR="$SIM_DIR/HDL_OUT"
INPUT_VECTORS_DIR="$SIM_DIR/INPUT_VECTORS"
HLS_VERIFY_DIR="$SIM_DIR/HLS_VERIFY"
IO_GEN_BIN="$SIM_DIR/C_SRC/$KERNEL_NAME-io-gen"

# Shortcuts.
HDL_DIR="$OUTPUT_DIR/hdl"
CLANGXX_BIN="$DYNAMATIC_DIR/bin/clang++"
HLS_VERIFIER_BIN="$DYNAMATIC_DIR/bin/hls-verifier"
RESOURCE_DIR="$DYNAMATIC_DIR/tools/hls-verifier/resources"

# ============================================================================ #
# Helper functions
# ============================================================================ #

needs_generated_vivado_fpu_sim_models() {
  # Role:
  # Identify the exact local configuration that needs additional simulator HDL.
  #
  # Goal:
  # Keep all other Dynamatic simulator paths byte-for-byte close to the upstream
  # script, while enabling the Vivado-FPU Verilog/XSim combination that otherwise
  # fails on stale floating_point_v7_1_8 support wrappers.
  [ "$VIVADO_FPU" = "true" ] && [ "$SIMULATOR_NAME" = "xsim" ] && [ "$HDL_TYPE" = "verilog" ]
}

vivado_bin() {
  # Role:
  # Resolve the Vivado executable from the installation root passed by the Python
  # runner, falling back to PATH for installations exposed through the shell.
  #
  # Goal:
  # Let the wrapper work both with /opt/.../Vivado/<ver> paths and with a caller's
  # environment-provided vivado command.
  if [ -x "$VIVADO_PATH/bin/vivado" ]; then
    printf '%s\n' "$VIVADO_PATH/bin/vivado"
  else
    printf '%s\n' "vivado"
  fi
}

xelab_bin() {
  # Role:
  # Resolve the real XSim elaboration executable before we shadow xelab in PATH.
  #
  # Goal:
  # Ensure hls-verifier still runs AMD/Xilinx xelab, but with the library flags
  # required by generated Vivado floating-point IP and unisim primitives.
  if [ -x "$VIVADO_PATH/bin/xelab" ]; then
    printf '%s\n' "$VIVADO_PATH/bin/xelab"
  else
    command -v xelab
  fi
}

generate_vivado_fpu_ip_models() {
  # Role:
  # Generate current-version Verilog simulation wrappers for the Vivado floating
  # point IP modules instantiated by Dynamatic's Verilog arithmetic components.
  #
  # Goal:
  # Replace Dynamatic's stale static VHDL wrappers, which reference
  # floating_point_v7_1_8, with Vivado 2023.2-generated Verilog wrappers that bind
  # to the installed floating_point_v7_1_16 simulation library.
  local ip_work_dir="$SIM_DIR/VIVADO_FPU_IP"
  local ip_project_name="dynamatic_vivado_fpu_ip"
  local ip_project_dir="$ip_work_dir/project"
  local ip_script="$ip_work_dir/generate_vivado_fpu_ip.tcl"
  local vivado_part="${VIVADO_PART:-xc7a100tcsg324-1}"

  rm -rf "$ip_work_dir"
  mkdir -p "$ip_work_dir"

  cat > "$ip_script" <<EOF
set_param general.maxThreads 8
create_project $ip_project_name $ip_project_dir -part $vivado_part -force
set vivado_ver [version -short]
set fpo_ver 7.1
if {[regexp -nocase {2015\.1.*} \$vivado_ver match]} {
  set fpo_ver 7.0
}
source $DYNAMATIC_DIR/tools/backend/synth-resources/mulf_vitis_hls_single_precision_lat_4.tcl
source $DYNAMATIC_DIR/tools/backend/synth-resources/subf_vitis_hls_single_precision_lat_8.tcl
source $DYNAMATIC_DIR/tools/backend/synth-resources/divf_vitis_hls_single_precision_lat_28.tcl
source $DYNAMATIC_DIR/tools/backend/synth-resources/cmpf_vitis_hls_single_precision_lat_0.tcl
source $DYNAMATIC_DIR/tools/backend/synth-resources/addf_vitis_hls_single_precision_lat_8.tcl
create_ip -name floating_point -version \$fpo_ver -vendor xilinx.com -library ip -module_name fadd_32ns_32ns_32_13_full_dsp_1_ip
set_property -dict [list CONFIG.a_precision_type Single \
                          CONFIG.a_tuser_width 1 \
                          CONFIG.add_sub_value Add \
                          CONFIG.b_tuser_width 1 \
                          CONFIG.c_a_exponent_width 8 \
                          CONFIG.c_a_fraction_width 24 \
                          CONFIG.c_compare_operation Programmable \
                          CONFIG.c_has_divide_by_zero false \
                          CONFIG.c_has_invalid_op false \
                          CONFIG.c_has_overflow false \
                          CONFIG.c_has_underflow false \
                          CONFIG.c_latency 8 \
                          CONFIG.c_mult_usage Full_Usage \
                          CONFIG.c_optimization Speed_Optimized \
                          CONFIG.c_rate 1 \
                          CONFIG.c_result_exponent_width 8 \
                          CONFIG.c_result_fraction_width 24 \
                          CONFIG.component_name fadd_32ns_32ns_32_13_full_dsp_1_ip \
                          CONFIG.flow_control NonBlocking \
                          CONFIG.has_a_tlast false \
                          CONFIG.has_a_tuser false \
                          CONFIG.has_aclken true \
                          CONFIG.has_aresetn false \
                          CONFIG.has_b_tlast false \
                          CONFIG.has_b_tuser false \
                          CONFIG.has_operation_tlast false \
                          CONFIG.has_operation_tuser false \
                          CONFIG.has_result_tready false \
                          CONFIG.maximum_latency false \
                          CONFIG.operation_tuser_width 1 \
                          CONFIG.operation_type Add_Subtract \
                          CONFIG.result_precision_type Single \
                          CONFIG.result_tlast_behv Null] -objects [get_ips fadd_32ns_32ns_32_13_full_dsp_1_ip] -quiet
set_property generate_synth_checkpoint false [get_files fadd_32ns_32ns_32_13_full_dsp_1_ip.xci]
generate_target {synthesis simulation} [get_files fadd_32ns_32ns_32_13_full_dsp_1_ip.xci]
export_ip_user_files -of_objects [get_ips] -no_script -sync -force -quiet
exit
EOF

  "$(vivado_bin)" -mode batch -source "$ip_script" > "$ip_work_dir/vivado_ip_generation.log"

  # The static VHDL support wrappers target an old Vivado IP revision.  Removing
  # only the copied simulation files keeps OUTPUT_DIR/hdl intact for debugging and
  # synthesis while letting hls-verifier discover the generated Verilog wrappers.
  rm -f "$COSIM_HDL_SRC_DIR"/*_vivado_support.vhd
  cp "$ip_project_dir/$ip_project_name.gen/sources_1/ip"/*/sim/*.v "$COSIM_HDL_SRC_DIR"
}

install_xelab_library_wrapper() {
  # Role:
  # Put a local xelab shim ahead of Vivado's bin directory for the hls-verifier
  # subprocess only.
  #
  # Goal:
  # Add the precompiled Vivado simulation libraries that the generated floating
  # point wrappers depend on, without patching Dynamatic's hls-verifier binary.
  local wrapper_dir="$HLS_VERIFY_DIR/xsim_wrapper_bin"
  local real_xelab
  real_xelab="$(xelab_bin)"

  mkdir -p "$wrapper_dir"
  cat > "$wrapper_dir/xelab" <<EOF
#!/bin/bash
exec "$real_xelab" -L floating_point_v7_1_16 -L unisims_ver "\$@"
EOF
  chmod +x "$wrapper_dir/xelab"
  export PATH="$wrapper_dir:$PATH"
}

# ============================================================================ #
# Simulation flow
# ============================================================================ #

# Reset simulation directory.
rm -rf "$SIM_DIR" && mkdir -p "$SIM_DIR"

# Create simulation directories.
mkdir -p "$C_SRC_DIR" "$C_OUT_DIR" "$COSIM_HDL_SRC_DIR" "$COSIM_HDL_OUT_DIR" \
    "$INPUT_VECTORS_DIR" "$HLS_VERIFY_DIR"

# Copy integration header to a C source subdirectory so that its relative path is
# correct with respect to the source file containing the kernel.
DYN_INCLUDE_DIR="$C_SRC_DIR/dynamatic"
mkdir "$DYN_INCLUDE_DIR"
cp "$DYNAMATIC_DIR/include/dynamatic/Integration.h" "$DYN_INCLUDE_DIR"

# Copy generated HDL module and components to the simulator source folder.
cp "$HDL_DIR/"*.vhd "$COSIM_HDL_SRC_DIR" 2> /dev/null || true
cp "$HDL_DIR/"*.v "$COSIM_HDL_SRC_DIR" 2> /dev/null || true

if [ "$VIVADO_FPU" = "true" ]; then
  # Copy glbl.v when Vivado IP support is enabled; XSim/ModelSim use it for
  # Xilinx global set/reset wiring and hls-verifier can include it in the project.
  cp "$VIVADO_PATH/data/verilog/src/glbl.v" "$COSIM_HDL_SRC_DIR/glbl.v"
fi

if needs_generated_vivado_fpu_sim_models; then
  generate_vivado_fpu_ip_models
  install_xelab_library_wrapper
fi

# Copy sources to a dedicated folder.
cp "$SRC_DIR/$KERNEL_NAME.c" "$C_SRC_DIR"
# Suppress the error if the header file does not exist; it is optional.
cp "$SRC_DIR/$KERNEL_NAME.h" "$C_SRC_DIR" 2> /dev/null || true

# Copy TB supplementary files: memory model, argument wrappers, and simulator
# support files expected by hls-verifier.
if [ "$HDL_TYPE" = "verilog" ]; then
  cp "$RESOURCE_DIR/templates_verilog/template_tb_join.v" "$COSIM_HDL_SRC_DIR/tb_join.v"
  cp "$RESOURCE_DIR/templates_verilog/template_two_port_RAM.sv" "$COSIM_HDL_SRC_DIR/two_port_RAM.sv"
  cp "$RESOURCE_DIR/templates_verilog/template_single_argument.sv" "$COSIM_HDL_SRC_DIR/single_argument.sv"
  cp "$RESOURCE_DIR/modelsim.ini" "$HLS_VERIFY_DIR/modelsim.ini"
  cp "$RESOURCE_DIR/verilator_main.cpp" "$HLS_VERIFY_DIR/verilator_main.cpp"
else
  cp "$RESOURCE_DIR/templates_vhdl/template_tb_join.vhd" "$COSIM_HDL_SRC_DIR/tb_join.vhd"
  cp "$RESOURCE_DIR/templates_vhdl/template_two_port_RAM.vhd" "$COSIM_HDL_SRC_DIR/two_port_RAM.vhd"
  cp "$RESOURCE_DIR/templates_vhdl/template_single_argument.vhd" "$COSIM_HDL_SRC_DIR/single_argument.vhd"
  cp "$RESOURCE_DIR/templates_vhdl/template_simpackage.vhd" "$COSIM_HDL_SRC_DIR/simpackage.vhd"
  cp "$RESOURCE_DIR/modelsim.ini" "$HLS_VERIFY_DIR/modelsim.ini"
fi

# Compile the kernel's main function to generate inputs and golden outputs for
# the simulator comparison.
"$CLANGXX_BIN" "$SRC_DIR/$KERNEL_NAME.c" -D HLS_VERIFICATION \
  -DHLS_VERIFICATION_PATH="$SIM_DIR" -I "$DYNAMATIC_DIR/include" \
  -Wno-deprecated -o "$IO_GEN_BIN"
exit_on_fail "Failed to build kernel for IO gen." "Built kernel for IO gen."

# Generate IO vectors and expected outputs.
"$IO_GEN_BIN"
exit_on_fail "Failed to run kernel for IO gen." "Ran kernel for IO gen."

EXTRA_ARGS=""
if [ -n "$TIMEOUT" ]; then
  EXTRA_ARGS="--timeout=$TIMEOUT"
fi

# Simulate and verify design.
echo_info "Launching simulation ($SIMULATOR_NAME)"
cd "$HLS_VERIFY_DIR"
if [ "$VIVADO_FPU" = "true" ]; then
  "$HLS_VERIFIER_BIN" \
  --sim-path="$SIM_DIR" \
  --kernel-name="$KERNEL_NAME" \
  --handshake-mlir="$OUTPUT_DIR/comp/handshake_export.mlir" \
  --simulator="$SIMULATOR_NAME" \
  --hdl="$HDL_TYPE" \
  --vivado-fpu \
  $EXTRA_ARGS \
  > "../report.txt" 2>&1
else
  "$HLS_VERIFIER_BIN" \
  --sim-path="$SIM_DIR" \
  --kernel-name="$KERNEL_NAME" \
  --handshake-mlir="$OUTPUT_DIR/comp/handshake_export.mlir" \
  --simulator="$SIMULATOR_NAME" \
  --hdl="$HDL_TYPE" \
  $EXTRA_ARGS \
  > "../report.txt" 2>&1
fi
exit_on_fail "Simulation failed" "Simulation succeeded"
