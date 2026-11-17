#!/bin/bash
# Runs the GPU regression tests on Mesa lavapipe, one selector of shader_recompiler_compute_tests
# per process, and compares the result with tests/software_gpu_layer/expected_failures.txt.
#
#   cmake --build <build dir> --target shader_recompiler_compute_tests kyty_software_gpu_layer
#   tests/run_gpu_tests.sh <build dir> [--update]
#
# Needs xvfb-run and the lavapipe ICD (Debian/Ubuntu: xvfb mesa-vulkan-drivers). lavapipe lacks
# depthBounds, VK_EXT_image_view_min_lod and VK_KHR_fragment_shader_barycentric, which the
# kyty_software_gpu_layer target hides from the tests; see softwareGpuLayer.cpp. The selectors a
# software rasterizer cannot pass are listed in expected_failures.txt, so that a change to the
# renderer shows up as a difference from that list. --update rewrites the list.
set -u

build="$(cd "${1:?usage: run_gpu_tests.sh <build dir> [--update]}" && pwd)" || exit 2
update="${2:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
tests="$build/shader_recompiler_compute_tests"
expected="$root/tests/software_gpu_layer/expected_failures.txt"
icd="${KYTY_LAVAPIPE_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"

for required in "$tests" "$build/software_gpu_layer/VkLayer_kyty_software_gpu_caps.json" "$icd"; do
	if [ ! -e "$required" ]; then
		echo "run_gpu_tests: missing $required" >&2
		exit 2
	fi
done

export VK_ICD_FILENAMES="$icd"
export VK_LAYER_PATH="$build/software_gpu_layer"
export VK_INSTANCE_LAYERS=VK_LAYER_KYTY_software_gpu_caps
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
	XDG_RUNTIME_DIR="$(mktemp -d)"
	export XDG_RUNTIME_DIR
fi

results="$(mktemp)"
trap 'rm -f "$results"' EXIT

run_selector() {
	local log
	log="$(mktemp)"
	timeout 900 xvfb-run -a "$tests" "$1" > "$log" 2>&1
	local status=$?
	if grep -q 'unknown test selector' "$log"; then
		status=skip
	fi
	echo "$1 $status $(grep -c ' ok$' "$log")"
	rm -f "$log"
}
export -f run_selector
export tests

grep -o -E '"--[a-z0-9-]+-only"' "$root/tests/ShaderRecompilerComputeTests.cpp" | tr -d '"' |
	sort -u | xargs -P "${KYTY_TEST_JOBS:-$(nproc)}" -I{} bash -c 'run_selector {}' > "$results"

failed="$(awk '$2 != "0" && $2 != "skip" {print $1}' "$results" | sort)"
passed="$(awk '$2 == "0" {print $1}' "$results" | sort)"
cases="$(awk '$2 == "0" {sum += $3} END {print sum + 0}' "$results")"

if [ "$update" = "--update" ]; then
	{
		echo "# Selectors of shader_recompiler_compute_tests that fail on lavapipe."
		echo "# Written by tests/run_gpu_tests.sh --update with:"
		echo "# $(vulkaninfo --summary 2> /dev/null | grep -m1 driverInfo | sed 's/^[[:space:]]*//')"
		echo "$failed"
	} > "$expected"
	echo "run_gpu_tests: wrote $expected"
fi

known="$(grep -v '^#' "$expected" 2> /dev/null | sort)"
new_failures="$(comm -23 <(echo "$failed") <(echo "$known") | sed '/^$/d')"
new_passes="$(comm -12 <(echo "$passed") <(echo "$known") | sed '/^$/d')"

echo "run_gpu_tests: $(echo "$passed" | sed '/^$/d' | wc -l) selectors passed ($cases cases)," \
	"$(echo "$failed" | sed '/^$/d' | wc -l) failed"
if [ -n "$new_passes" ]; then
	echo "run_gpu_tests: passed although listed in expected_failures.txt:"
	echo "$new_passes" | sed 's/^/  /'
fi
if [ -n "$new_failures" ]; then
	echo "run_gpu_tests: NEW FAILURES:"
	echo "$new_failures" | sed 's/^/  /'
	exit 1
fi
