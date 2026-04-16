#!/bin/bash
set -euo pipefail

[ -z "${THEOS_SOURCE_ROOT:-}" ] && THEOS_SOURCE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${THEOS_EMBEDDEDDOOM_SUBMODULE:-}" ]; then
	if git -C "${THEOS_SOURCE_ROOT}/Userland/Apps/TheEmbeddedDOOM" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
		THEOS_EMBEDDEDDOOM_SUBMODULE="${THEOS_SOURCE_ROOT}/Userland/Apps/TheEmbeddedDOOM"
	else
		THEOS_EMBEDDEDDOOM_SUBMODULE="${THEOS_SOURCE_ROOT}/Userland/Apps/embeddedDOOM"
	fi
fi
[ -z "${THEOS_EMBEDDEDDOOM_PATCH_DIR:-}" ] && THEOS_EMBEDDEDDOOM_PATCH_DIR="${THEOS_SOURCE_ROOT}/Meta/patches/embeddedDOOM"

if [ ! -d "${THEOS_EMBEDDEDDOOM_PATCH_DIR}" ]; then
	echo "[embeddeddoom] no patch directory at '${THEOS_EMBEDDEDDOOM_PATCH_DIR}', skipping"
	exit 0
fi

if ! git -C "${THEOS_EMBEDDEDDOOM_SUBMODULE}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	echo "[embeddeddoom] '${THEOS_EMBEDDEDDOOM_SUBMODULE}' is not a git work tree; did you init submodules?" >&2
	exit 1
fi

patches=("${THEOS_EMBEDDEDDOOM_PATCH_DIR}"/*.patch)
if [ ! -e "${patches[0]}" ]; then
	echo "[embeddeddoom] no patches found in '${THEOS_EMBEDDEDDOOM_PATCH_DIR}', skipping"
	exit 0
fi

applied_count=0
already_count=0
skipped_count=0

is_dirty() {
	git -C "${THEOS_EMBEDDEDDOOM_SUBMODULE}" status --porcelain | grep -q .
}

for patch in "${patches[@]}"; do
	patch_name="$(basename "${patch}")"

	if git -C "${THEOS_EMBEDDEDDOOM_SUBMODULE}" apply --reverse --check "${patch}" >/dev/null 2>&1; then
		echo "[embeddeddoom] already applied: ${patch_name}"
		already_count=$((already_count + 1))
		continue
	fi

	if git -C "${THEOS_EMBEDDEDDOOM_SUBMODULE}" apply --check "${patch}" >/dev/null 2>&1; then
		git -C "${THEOS_EMBEDDEDDOOM_SUBMODULE}" apply "${patch}"
		echo "[embeddeddoom] applied: ${patch_name}"
		applied_count=$((applied_count + 1))
		continue
	fi

	if is_dirty; then
		echo "[embeddeddoom] patch skipped (dirty worktree): ${patch_name}" >&2
		skipped_count=$((skipped_count + 1))
		continue
	fi

	echo "[embeddeddoom] failed to apply patch '${patch_name}'." >&2
	echo "[embeddeddoom] submodule may have diverged; inspect with:" >&2
	echo "  git -C ${THEOS_EMBEDDEDDOOM_SUBMODULE} status --short" >&2
	echo "  git -C ${THEOS_EMBEDDEDDOOM_SUBMODULE} apply --check ${patch}" >&2
	exit 1
done

echo "[embeddeddoom] patch sync done (applied=${applied_count}, already=${already_count}, skipped=${skipped_count})"
