#!/bin/bash
#===============================================================================
# build.sh - Batch-build every device listed in document/support-devices.md
#
# Usage: ./build.sh <PLATFORM...> [OPTIONS]
#
# Builds each device of the selected platforms by calling that platform's own
# build script, then reports how many configurations succeeded and which ones
# failed.
#
# Platforms (they are also the build scripts of this tree):
#   airoha     Airoha ARM         -> ./airoha.sh   (en7523 en7529 en7562 an7563 an7581 an7583)
#   econet     EcoNet MIPS        -> ./econet.sh   (en751221 en751627 en7528 en7580)
#   mediatek   MediaTek Filogic   -> ./mediatek.sh (mt7981 mt7986 mt7987 mt7988)
#   mtmips     MediaTek MTMIPS    -> ./mtmips.sh   (mt7620 mt7621 mt7628 mt7688)
#   all        Every platform above
#
# At least one platform is required: a bare ./build.sh is a usage error, so a
# full-tree build is never started by accident.  Ask for it with "all".
# (--list is the exception: without a platform it lists every device.)
#
# The device list comes from document/support-devices.md (override with
# DEVICES=<path>), so adding a board to that document is all it takes for the
# board to be built here too.
#
# Configurations run strictly one after another: every platform script
# reconfigures and rebuilds the same source tree (make clean + .config), so
# they must never run in parallel.
#
# Options (environment variables; the switches below override them):
#   DEVICES=<path>      Device list to read (default: document/support-devices.md)
#   JOBS=<n>            Parallel make jobs, handed to the platform scripts
#   TOOLCHAIN=<prefix>  Cross-compiler prefix, handed to the platform scripts
#   STAGING_DIR=<path>  Passed through to the platform scripts
#   STAGE=<stage>       Stage for mediatek.sh (all|uboot|atf) and
#                       econet.sh (all|uboot|dramc)
#   AUTO_DL=<0|1>       Let the platform scripts download a missing toolchain
#                       without prompting (default: 1; mtmips/econet only)
#   LOGS_DIR=<path>     Per-configuration logs and configurations
#                       (default: build-logs)
#   MTK_ATF_DIR=<path>  TF-A tree of the mediatek platform; its build/.config is
#                       copied next to the logs (default: ../atf-mtksoc, passed
#                       through to mediatek.sh as ATF_DIR).  A bare ATF_DIR is
#                       still accepted as a fallback.
#   AIROHA_ATF_DIR=<path>
#                       ATF source tree of the airoha platform, passed through
#                       to airoha.sh as ATF_DIR (default: ../atf-airoha); airoha
#                       builds BL1/BL2/BL31 from it instead of using the
#                       checked-in blobs
#
# Switches:
#   -h, --help              Show this help
#   -l, --list              List the configurations that would be built
#   -j, --jobs <n>          Same as JOBS=<n>
#   -d, --auto-download     Same as AUTO_DL=1
#       --no-auto-download  Same as AUTO_DL=0
#       --logs-dir <dir>    Same as LOGS_DIR=<dir>
#       --fail-fast         Stop at the first failing configuration
#
# Output:
#   build-logs/<platform>_<soc>_<board>.log   full log of one configuration
#   build-logs/<platform>_<soc>_<board>.config
#                                             the .config that configuration
#                                             actually built, plus
#                                             ...-atf.config when a TF-A tree
#                                             was involved (ATF_DIR exported)
#   build-logs/summary.txt                    result of every configuration
#
# Exit status: 0 when every configuration built, 1 when at least one failed.
#
# Examples:
#   ./build.sh all
#   ./build.sh mediatek
#   ./build.sh airoha econet
#   ./build.sh mtmips --list
#   JOBS=8 STAGE=uboot ./build.sh mediatek
#===============================================================================

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }
step()  { echo -e "\n${BLUE}=== $* ===${NC}"; }
die()   { error "$*"; exit 1; }

#------------------------------------------------------------------------------
# --help / -h: print usage extracted from the header comment block
#------------------------------------------------------------------------------
usage() {
	sed -n '2,/^[^#]/p' "$0" | grep -E '^#( |$)' | sed 's/^# \?//'
	exit 0
}

#------------------------------------------------------------------------------
# Command line
#------------------------------------------------------------------------------
AUTO_DL="${AUTO_DL:-1}"
FAIL_FAST=0
LIST_ONLY=0
PLATFORM_ARGS=()

while [ $# -gt 0 ]; do
	case "$1" in
		--help|-h|help)
			usage
			;;
		--list|-l)
			LIST_ONLY=1
			;;
		--fail-fast)
			FAIL_FAST=1
			;;
		--auto-download|-d)
			AUTO_DL=1
			;;
		--no-auto-download)
			AUTO_DL=0
			;;
		--jobs|-j)
			[ -n "${2:-}" ] || die "$1 needs an argument"
			JOBS="$2"
			shift
			;;
		--jobs=*)
			JOBS="${1#*=}"
			;;
		--logs-dir)
			[ -n "${2:-}" ] || die "$1 needs an argument"
			LOGS_DIR="$2"
			shift
			;;
		--logs-dir=*)
			LOGS_DIR="${1#*=}"
			;;
		-*)
			error "Unknown option: $1"
			echo "Try '$0 --help' for more information."
			exit 1
			;;
		*)
			PLATFORM_ARGS+=("$1")
			;;
	esac
	shift
done

#------------------------------------------------------------------------------
# Hand over the options the platform scripts read from the environment
#
# The platform scripts run as children and look AUTO_DL / JOBS up in their
# environment; "AUTO_DL=1 ./build.sh mtmips" exports it for free, but the
# switches (-j/--jobs, --no-auto-download) and the AUTO_DL default below only
# set a shell variable, which a child never sees.  Without this, a plain
# "./build.sh mtmips" reaches mtmips.sh with AUTO_DL unset: it then asks on
# stdin "Download it now? [Y/n]" - a prompt written into the configuration's
# log file, where nobody can see or answer it, so the batch looks hung.
#------------------------------------------------------------------------------
export AUTO_DL JOBS

#------------------------------------------------------------------------------
# Paths
#------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UBOOT_DIR="${SCRIPT_DIR}"
DEVICES_FILE="${DEVICES:-${UBOOT_DIR}/document/support-devices.md}"
LOGS_DIR="${LOGS_DIR:-${UBOOT_DIR}/build-logs}"
MTK_ATF_DIR="${MTK_ATF_DIR:-${ATF_DIR:-${UBOOT_DIR}/../atf-mtksoc}}"
AIROHA_ATF_DIR="${AIROHA_ATF_DIR:-${UBOOT_DIR}/../atf-airoha}"

# The second repository is platform specific (mediatek reads atf-mtksoc, airoha
# reads atf-airoha); the platform scripts all take their tree from ATF_DIR.
second_repo_dir() {
	case "$1" in
		airoha)   echo "${AIROHA_ATF_DIR}" ;;
		mediatek) echo "${MTK_ATF_DIR}" ;;
		*)        echo "" ;;
	esac
}

[ -f "${DEVICES_FILE}" ] || die "Device list not found: ${DEVICES_FILE}"

#------------------------------------------------------------------------------
# Platform selection
#------------------------------------------------------------------------------
normalize_platform() {
	case "${1,,}" in
		all)                  echo "all" ;;
		airoha)               echo "airoha" ;;
		econet|en)            echo "econet" ;;
		mediatek|mtk|filogic) echo "mediatek" ;;
		mtmips|ramips)        echo "mtmips" ;;
		*)                    echo "" ;;
	esac
}

declare -A WANT=()
if [ ${#PLATFORM_ARGS[@]} -eq 0 ]; then
	# No platform: never start a full-tree build by accident.  Listing is the
	# only thing a bare invocation is allowed to do.
	if [ "${LIST_ONLY}" != "1" ]; then
		error "Platform must be specified (use \"all\" to build every platform)."
		echo "Usage: $0 <airoha|econet|mediatek|mtmips|all> [...] [OPTIONS]"
		echo "Try '$0 --help' for more information."
		exit 1
	fi
	WANT[all]=1
else
	for arg in "${PLATFORM_ARGS[@]}"; do
		p=$(normalize_platform "${arg}")
		[ -n "${p}" ] || die "Unsupported platform '${arg}' (use airoha, econet, mediatek, mtmips or all)"
		WANT[$p]=1
	done
fi
if [ -n "${WANT[all]:-}" ]; then
	WANT=([airoha]=1 [econet]=1 [mediatek]=1 [mtmips]=1)
fi

PLATFORMS=()
for p in airoha econet mediatek mtmips; do
	[ -n "${WANT[$p]:-}" ] && PLATFORMS+=("$p")
done
PLATFORM_LIST="${PLATFORMS[*]}"

#------------------------------------------------------------------------------
# Device list
#
# One "platform soc board" triple per device, read from the markdown tables of
# document/support-devices.md: a "## <section>" heading selects the platform,
# every following "| SOC | Board |" row is one configuration.
#------------------------------------------------------------------------------
parse_devices() {
	awk '
		function trim(s) {
			gsub(/^[ \t]+/, "", s)
			gsub(/[ \t]+$/, "", s)
			return s
		}
		/^##[ \t]/ {
			low = tolower(trim(substr($0, 3)))
			plat = ""
			if (low ~ /airoha/)       plat = "airoha"
			else if (low ~ /econet/)  plat = "econet"
			else if (low ~ /filogic/) plat = "mediatek"
			else if (low ~ /mtmips/)  plat = "mtmips"
			next
		}
		plat != "" && /^[ \t]*\|/ {
			if (split($0, col, "|") < 4) next
			soc   = trim(col[2])
			board = trim(col[3])
			if (soc == "" || board == "") next
			if (tolower(soc) == "soc")    next   # header row
			if (soc ~ /^[-:]+$/)          next   # separator row
			print plat, soc, board
		}
	' "${DEVICES_FILE}"
}

SELECTED=()
while read -r plat soc board; do
	[ -n "${plat}" ] || continue
	[ -n "${WANT[$plat]:-}" ] || continue
	SELECTED+=("${plat} ${soc} ${board}")
done < <(parse_devices)

[ ${#SELECTED[@]} -gt 0 ] || die "No device found for platform(s): ${PLATFORM_LIST}"

if [ "${LIST_ONLY}" = "1" ]; then
	echo ""
	echo "Configurations selected from ${DEVICES_FILE}:"
	echo ""
	printf "  %-10s %-10s %s\n" "PLATFORM" "SOC" "BOARD"
	printf "  %-10s %-10s %s\n" "--------" "--------" "-----"
	for entry in "${SELECTED[@]}"; do
		read -r plat soc board <<< "${entry}"
		printf "  %-10s %-10s %s\n" "${plat}" "${soc}" "${board}"
	done
	echo ""
	echo "  Total: ${#SELECTED[@]}"
	exit 0
fi

#------------------------------------------------------------------------------
# Pre-flight helpers
#------------------------------------------------------------------------------

# Same defconfig matching as mediatek.sh: "${SOC}_${BOARD}_defconfig", with a
# single "${SOC}*_${BOARD}_defconfig" fallback for boards whose defconfig
# carries an extra SoC suffix (mt7986a_bpir3_emmc).
defconfig_found() {
	local soc="$1" board="$2"
	local matches=()

	if [ -f "${UBOOT_DIR}/configs/${soc}_${board}_defconfig" ]; then
		return 0
	fi
	shopt -s nullglob
	matches=( "${UBOOT_DIR}/configs/${soc}"*_"${board}"_defconfig )
	shopt -u nullglob
	[ ${#matches[@]} -gt 0 ]
}

# Number of flashable artifacts the platform script collected into its output
# directory: airoha/mediatek/econet print "<file> (md5: ...) -> <dir>/<file>",
# mtmips prints "Output: <dir>/<file>".  The "Output directory: <dir>/" line of
# the summaries must not count, hence the anchored "Output: " form.
count_artifacts() {
	local n
	n=$(grep -cE -- '(-> |^Output: ).*output_(airoha|mediatek|econet|mtmips)/' "$1" 2>/dev/null || true)
	echo "${n:-0}"
}

#------------------------------------------------------------------------------
# State
#------------------------------------------------------------------------------
declare -A P_TOTAL=() P_PASS=() P_FAIL=()
FAILED=()
TOTAL=0
PASS=0
FAIL=0
ARTIFACTS=0

mkdir -p "${LOGS_DIR}"
SUMMARY_FILE="${LOGS_DIR}/summary.txt"
: > "${SUMMARY_FILE}"

#------------------------------------------------------------------------------
# Failure bookkeeping
#
#   record_failure <plat> <soc> <board> <detail> [<log>]
#
# Counts the configuration as failed, records it for the summary and honours
# --fail-fast.
#------------------------------------------------------------------------------
record_failure() {
	local plat="$1" soc="$2" board="$3" detail="$4" log="${5:-}"
	local label="${plat} ${soc} ${board}"

	P_FAIL[$plat]=$(( ${P_FAIL[$plat]:-0} + 1 ))
	FAIL=$(( FAIL + 1 ))
	if [ -n "${log}" ]; then
		FAILED+=("${label}  (${detail})  log: ${log}")
	else
		FAILED+=("${label}  (${detail})")
	fi
	printf 'FAIL\t%s\t%s\t%s\t%s\n' "${plat}" "${soc}" "${board}" "${detail}" >> "${SUMMARY_FILE}"

	if [ "${FAIL_FAST}" = "1" ]; then
		warn "--fail-fast: stopping after the first failure"
		finalize_summary
		print_summary
		exit 1
	fi
}

finalize_summary() {
	printf '# passed: %s  failed: %s\n' "${PASS}" "${FAIL}" >> "${SUMMARY_FILE}"
}

#------------------------------------------------------------------------------
# Run one configuration
#------------------------------------------------------------------------------
run_config() {
	local plat="$1" soc="$2" board="$3" idx="$4"
	local script="${UBOOT_DIR}/${plat}.sh"
	local label="${plat} ${soc} ${board}"
	local log="${LOGS_DIR}/${plat}_${soc}_${board}.log"
	local rc=0 t0 t1 secs arts cfg second_dir
	second_dir=$(second_repo_dir "${plat}")

	soc="${soc,,}"   # SOC is lowercase for every platform script

	P_TOTAL[$plat]=$(( ${P_TOTAL[$plat]:-0} + 1 ))
	TOTAL=$(( TOTAL + 1 ))

	step "[${idx}/${#SELECTED[@]}] ${label}"

	if [ ! -x "${script}" ]; then
		error "FAIL  ${label}: platform script missing or not executable: ${script}"
		record_failure "${plat}" "${soc}" "${board}" "no ${plat}.sh"
		return 0
	fi

	if ! defconfig_found "${soc}" "${board}"; then
		error "FAIL  ${label}: defconfig not found (configs/${soc}_${board}_defconfig)"
		record_failure "${plat}" "${soc}" "${board}" "defconfig not found"
		return 0
	fi

	info "Command: SOC=${soc} BOARD=${board} ./${plat}.sh"
	info "Log:     ${log}"

	t0=$(date +%s)
	# Each configuration runs in its own subshell: the platform scripts cd
	# around and export ARCH/STAGING_DIR/... which must not leak into the next
	# one.  JOBS / TOOLCHAIN / STAGE / AUTO_DL / STAGING_DIR come from the
	# environment, so they are simply inherited; the second-repository tree is
	# handed over explicitly because its location differs per platform.
	(
		cd "${UBOOT_DIR}" || exit 1
		if [ -n "${second_dir}" ]; then
			export ATF_DIR="${second_dir}"
		fi
		SOC="${soc}" BOARD="${board}" "${script}"
	) > "${log}" 2>&1 || rc=$?
	t1=$(date +%s)
	secs=$(( t1 - t0 ))

	cfg="${log%.log}"    # <plat>_<soc>_<board>, exactly like its log
	if [ -f "${UBOOT_DIR}/.config" ] && \
	   [ "$(stat -c %Y "${UBOOT_DIR}/.config")" -ge "${t0}" ]; then
		cp -f "${UBOOT_DIR}/.config" "${cfg}.config"
	fi
	if [ -n "${second_dir}" ] && [ -f "${second_dir}/build/.config" ] && \
	   [ "$(stat -c %Y "${second_dir}/build/.config")" -ge "${t0}" ]; then
		cp -f "${second_dir}/build/.config" "${cfg}-atf.config"
	fi

	if [ "${rc}" -eq 0 ]; then
		arts=$(count_artifacts "${log}")
		ARTIFACTS=$(( ARTIFACTS + arts ))
		P_PASS[$plat]=$(( ${P_PASS[$plat]:-0} + 1 ))
		PASS=$(( PASS + 1 ))
		info "PASS  ${label}  (${secs}s, ${arts} artifact(s))"
		printf 'PASS\t%s\t%s\t%s\t%s\n' "${plat}" "${soc}" "${board}" "${arts} artifact(s)" >> "${SUMMARY_FILE}"
	else
		error "FAIL  ${label}  (exit ${rc}, ${secs}s)"
		echo "  --- last 20 log lines (${log}) ---"
		tail -n 20 "${log}" | sed 's/\x1b\[[0-9;]*m//g' | sed 's/^/  /'
		echo "  ----------------------------------"
		record_failure "${plat}" "${soc}" "${board}" "exit ${rc}" "${log}"
	fi

	return 0
}

#------------------------------------------------------------------------------
# Summary
#------------------------------------------------------------------------------
print_summary() {
	local p f

	echo ""
	echo "==========================================================================="
	echo -e "  ${BOLD}Batch build summary${NC}"
	echo "==========================================================================="
	echo ""
	printf "  %-10s %6s %6s %6s\n" "PLATFORM" "TOTAL" "PASSED" "FAILED"
	printf "  %-10s %6s %6s %6s\n" "----------" "-----" "------" "------"
	for p in "${PLATFORMS[@]}"; do
		printf "  %-10s %6d %6d %6d\n" "${p}" "${P_TOTAL[$p]:-0}" "${P_PASS[$p]:-0}" "${P_FAIL[$p]:-0}"
	done
	printf "  %-10s %6d %6d %6d\n" "TOTAL" "${TOTAL}" "${PASS}" "${FAIL}"

	echo ""
	if [ "${FAIL}" -eq 0 ]; then
		echo -e "  ${GREEN}All ${TOTAL} configuration(s) built successfully.${NC}"
	else
		echo -e "  ${GREEN}Succeeded: ${PASS}${NC}   ${RED}Failed: ${FAIL}${NC}   (of ${TOTAL})"
		echo ""
		echo -e "  ${RED}Failed configurations:${NC}"
		for f in "${FAILED[@]}"; do
			printf "    - %s\n" "${f}"
		done
	fi

	echo ""
	echo "  Flashable artifacts collected: ${ARTIFACTS}"
	echo "  Logs:     ${LOGS_DIR}/"
	echo "  Results:  ${SUMMARY_FILE}"
	echo "==========================================================================="
	echo ""
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------
main() {
	echo ""
	echo "==========================================================================="
	echo "	U-Boot Batch Build"
	echo "  Platforms: ${PLATFORM_LIST}"
	echo "  Devices:   ${#SELECTED[@]} configuration(s) from ${DEVICES_FILE}"
	echo "  Source:    ${UBOOT_DIR}"
	echo "  Jobs:      ${JOBS:-$(nproc 2>/dev/null || echo 1)}"
	echo "  Logs:      ${LOGS_DIR}"
	if [ "${AUTO_DL}" = "1" ]; then
		echo "  Toolchain: missing ones are downloaded automatically (AUTO_DL=1)"
	else
		echo "  Toolchain: AUTO_DL=0, mtmips.sh/econet.sh may prompt on stdin"
	fi
	echo "==========================================================================="

	{
		printf '# build.sh  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
		printf '# platforms: %s\n' "${PLATFORM_LIST}"
		printf '# configurations: %s\n' "${#SELECTED[@]}"
		printf '# result: platform\tsoc\tboard\tdetail\n'
	} >> "${SUMMARY_FILE}"

	local idx=0 entry plat soc board
	for entry in "${SELECTED[@]}"; do
		read -r plat soc board <<< "${entry}"
		idx=$(( idx + 1 ))
		run_config "${plat}" "${soc}" "${board}" "${idx}"
	done

	finalize_summary

	print_summary

	[ "${FAIL}" -eq 0 ]
}

main "$@"
