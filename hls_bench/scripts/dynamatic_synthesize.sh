#!/bin/bash

# Repo-local Dynamatic synthesis wrapper.
#
# Role:
# Run the Vivado synthesis stage for the legacy Dynamatic v2.0.0 output layout
# without modifying the installed Dynamatic tree under ~/.local.
#
# Goal:
# Keep the benchmark CLI able to target the same FPGA part and clock period as
# the LegUp/Bambu flows, while preserving Dynamatic's generated HDL and support
# files exactly as produced by compile.sh/write-hdl.sh.
#
# How:
# The script mirrors the installed Dynamatic v2.0.0 synthesize.sh structure, but
# accepts the Vivado part and clock as explicit arguments:
#   $1: Dynamatic root directory
#   $2: Dynamatic benchmark output directory
#   $3: kernel/top name
#   $4: Vivado part name, e.g. xc7a100tcsg324-1
#   $5: full clock period in ns
#   $6: half clock period in ns
#   $7: Vivado installation root or command directory, optional
#   $8: whether Dynamatic emitted Vivado floating-point IP wrappers, optional
#   $9: HDL type emitted by Dynamatic, optional

set -euo pipefail

DYNAMATIC_DIR=$1
OUTPUT_DIR=$2
KERNEL_NAME=$3
VIVADO_PART=${4:-xc7a100tcsg324-1}
FULL_CLOCK=${5:-4.000}
HALF_CLOCK=${6:-2.000}
VIVADO_PATH=${7:-}
VIVADO_FPU=${8:-false}
HDL_TYPE=${9:-verilog}

source "$DYNAMATIC_DIR"/tools/dynamatic/scripts/utils.sh

SYNTH_DIR="$OUTPUT_DIR/synth"
SYNTH_HDL_DIR="$SYNTH_DIR/hdl"
F_REPORT="$SYNTH_DIR/report.txt"
F_SCRIPT="$SYNTH_DIR/synthesize.tcl"
F_PERIOD="$SYNTH_DIR/period_${FULL_CLOCK}.xdc"
F_UTILIZATION_SYN="$SYNTH_DIR/utilization_post_syn.rpt"
F_TIMING_SYN="$SYNTH_DIR/timing_post_syn.rpt"
F_UTILIZATION_PR="$SYNTH_DIR/utilization_post_pr.rpt"
F_TIMING_PR="$SYNTH_DIR/timing_post_pr.rpt"
HDL_DIR="$OUTPUT_DIR/hdl"
SYNTH_IP_DIR="$SYNTH_DIR/VIVADO_FPU_IP"

vivado_bin() {
  # Role:
  # Resolve the Vivado executable from the optional runner-provided installation
  # path, falling back to PATH for environments where Vivado is already sourced.
  #
  # Goal:
  # Keep this synthesis wrapper usable from both the common Python runner and
  # direct shell invocations while requiring the same Vivado binary for IP
  # generation and design synthesis.
  if [ -n "$VIVADO_PATH" ] && [ -x "$VIVADO_PATH/bin/vivado" ]; then
    printf '%s\n' "$VIVADO_PATH/bin/vivado"
  else
    printf '%s\n' "vivado"
  fi
}

needs_generated_vivado_fpu_synthesis_ip() {
  # Role:
  # Identify when Dynamatic's HDL contains Vivado floating-point wrapper modules
  # that must be resolved through generated Xilinx IP during synthesis.
  #
  # Goal:
  # Avoid reporting misleading zero-DSP utilization caused by unresolved
  # floating_point_v7_* black boxes, while leaving non-Vivado-FPU flows unchanged.
  [ "$VIVADO_FPU" = "true" ] && [ "$HDL_TYPE" = "verilog" ]
}

generate_vivado_fpu_synthesis_ip() {
  # Role:
  # Generate the same named Xilinx Floating Point IP cores that Dynamatic's
  # Verilog arithmetic components instantiate for single-precision operations.
  #
  # Goal:
  # Let Vivado synthesize the actual FP operator implementations and account for
  # their LUT/FF/DSP usage in post-synthesis reports, instead of treating the
  # static *_vivado_support.vhd wrappers as unresolved black boxes.
  local ip_project_name="dynamatic_vivado_fpu_ip"
  local ip_project_dir="$SYNTH_IP_DIR/project"
  local ip_script="$SYNTH_IP_DIR/generate_vivado_fpu_ip.tcl"

  rm -rf "$SYNTH_IP_DIR"
  mkdir -p "$SYNTH_IP_DIR"

  cat > "$ip_script" <<EOF
set_param general.maxThreads 8
create_project $ip_project_name $ip_project_dir -part $VIVADO_PART -force
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
generate_target {synthesis} [get_files *.xci]
export_ip_user_files -of_objects [get_ips] -no_script -sync -force -quiet
exit
EOF

  "$(vivado_bin)" -mode batch -source "$ip_script" > "$SYNTH_IP_DIR/vivado_ip_generation.log"
}

# Reset only this benchmark's synthesis directory.  Compile and HDL artifacts are
# intentionally left intact because they are inputs to this stage and useful for
# debugging synthesis failures.
rm -rf "$SYNTH_DIR" && mkdir -p "$SYNTH_HDL_DIR"

# Dynamatic's current write-hdl.sh emits a self-contained benchmark HDL directory
# containing the top-level kernel, wrapper, generated component instances, and
# benchmark-specific support packages.  Copy that generated directory verbatim
# instead of assuming the legacy flat $DYNAMATIC_DIR/data/vhdl/*.vhd layout.
if ! ls "$HDL_DIR"/*.vhd 1> /dev/null 2>&1 && ! ls "$HDL_DIR"/*.v 1> /dev/null 2>&1; then
  echo "error: no generated HDL files found in $HDL_DIR" >&2
  exit 1
fi
cp "$HDL_DIR"/*.vhd "$SYNTH_HDL_DIR" 2> /dev/null || true
cp "$HDL_DIR"/*.v "$SYNTH_HDL_DIR" 2> /dev/null || true

READ_IP=""
if needs_generated_vivado_fpu_synthesis_ip; then
  generate_vivado_fpu_synthesis_ip
  # These static wrappers instantiate an old floating_point_v7_1_* component by
  # name.  When left in the design, Vivado reports the FP cores as black boxes.
  # Removing only the staged synthesis copies keeps the original HDL artifacts
  # intact and allows the generated .xci IP definitions to provide the operators.
  rm -f "$SYNTH_HDL_DIR"/*_vivado_support.vhd
  READ_IP="read_ip [glob $SYNTH_IP_DIR/project/dynamatic_vivado_fpu_ip.srcs/sources_1/ip/*/*.xci]"
fi

READ_VHDL=""
if ls "$SYNTH_HDL_DIR"/*.vhd 1> /dev/null 2>&1; then
  READ_VHDL="read_vhdl -vhdl2008 [glob $SYNTH_DIR/hdl/*.vhd]"
fi

READ_VERILOG=""
if ls "$SYNTH_HDL_DIR"/*.v 1> /dev/null 2>&1; then
  READ_VERILOG="read_verilog [glob $SYNTH_DIR/hdl/*.v]"
fi

# Generate a self-contained Vivado script.  The part and clock are parameters of
# this local wrapper, unlike the installed Dynamatic script where they are
# hard-coded for a Kintex-7 target at 4 ns.
cat > "$F_SCRIPT" <<EOF
set_param general.maxThreads 8
create_project -in_memory -part $VIVADO_PART
$READ_IP
$READ_VHDL
$READ_VERILOG
if {[llength [get_ips -quiet]] > 0} {
  generate_target synthesis [get_ips]
  read_verilog [glob $SYNTH_IP_DIR/project/dynamatic_vivado_fpu_ip.gen/sources_1/ip/*/synth/*.v]
}
read_xdc "$F_PERIOD"
synth_design -top $KERNEL_NAME -part $VIVADO_PART -no_iobuf -mode out_of_context
report_utilization > $F_UTILIZATION_SYN
report_timing > $F_TIMING_SYN
opt_design
place_design
phys_opt_design
route_design
phys_opt_design
report_utilization > $F_UTILIZATION_PR
report_timing > $F_TIMING_PR
exit
EOF

cat > "$F_PERIOD" <<EOF
create_clock -name clk -period $FULL_CLOCK -waveform {0.000 $HALF_CLOCK} [get_ports clk]
set_property HD.CLK_SRC BUFGCTRL_X0Y0 [get_ports clk]

#set_input_delay 0 -clock CLK  [all_inputs]
#set_output_delay 0 -clock CLK [all_outputs]
EOF

echo_info "Created synthesis scripts for part $VIVADO_PART at ${FULL_CLOCK} ns"
echo_info "Launching Vivado synthesis"
cd "$SYNTH_DIR"
vivado -mode tcl -source "$F_SCRIPT" > "$F_REPORT"
if [ ! -s "$F_UTILIZATION_SYN" ] || [ ! -s "$F_TIMING_SYN" ]; then
  echo "error: Vivado did not produce post-synthesis reports" >&2
  exit 1
fi
exit_on_fail "Logic synthesis failed" "Logic synthesis succeeded"
