#!/usr/bin/env bash
# build mpv with vf_vsr neural filter overlay
# usage: ./neural/mpv/build_mpv.sh [/tmp/mpv-build] [--prefix $HOME/.local]
# overlays neural/mpv/vf_vsr.c into mpv tree and registers it, then builds

set -u

# resolve script locations
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"
mpv_src="${1:-/tmp/mpv-build}"
prefix="${2:-$HOME/.local}"

# validate mpv tree present
if [ ! -f "${mpv_src}/meson.build" ]; then
    echo "mpv source not found at ${mpv_src}" >&2
    exit 1
fi

# copy filter source into tree
cp "${repo_dir}/neural/mpv/vf_vsr.c" "${mpv_src}/video/filter/vf_vsr.c"
echo "overlay vf_vsr.c installed"

# register source file in meson when missing
if ! grep -q "video/filter/vf_vsr.c" "${mpv_src}/meson.build"; then
    # insert after vf_format entry
    python3 - "${mpv_src}/meson.build" << 'EOF'
import sys
path = sys.argv[1]
text = open(path).read()
anchor = "    'video/filter/vf_format.c',\n"
assert anchor in text, "meson anchor not found"
text = text.replace(anchor, anchor + "    'video/filter/vf_vsr.c',\n", 1)
open(path, "w").write(text)
EOF
    echo "meson.build patched"
fi

# declare extern entry when missing
header="${mpv_src}/filters/user_filters.h"
if ! grep -q "vf_vsr;" "${header}"; then
    # insert after vf_format declaration
    python3 - "${header}" << 'EOF'
import sys
path = sys.argv[1]
text = open(path).read()
anchor = "extern const struct mp_user_filter_entry vf_format;\n"
assert anchor in text, "header anchor not found"
text = text.replace(anchor, anchor + "extern const struct mp_user_filter_entry vf_vsr;\n", 1)
open(path, "w").write(text)
EOF
    echo "user_filters.h patched"
fi

# register filter in vf list when missing
list="${mpv_src}/filters/user_filters.c"
if ! grep -q "&vf_vsr," "${list}"; then
    # insert after vf_format entry
    python3 - "${list}" << 'EOF'
import sys
path = sys.argv[1]
text = open(path).read()
anchor = "    &vf_format,\n"
assert anchor in text, "list anchor not found"
text = text.replace(anchor, anchor + "    &vf_vsr,\n", 1)
open(path, "w").write(text)
EOF
    echo "user_filters.c patched"
fi

# configure build directory on first run
build_dir="${mpv_src}/build"
if [ ! -f "${build_dir}/build.ninja" ]; then
    meson setup "${build_dir}" "${mpv_src}" --prefix "${prefix}" 2>&1 | tail -3
fi

# compile mpv with filter
ninja -C "${build_dir}" 2>&1 | tail -3
echo "mpv binary: ${build_dir}/mpv"
echo "install with: ninja -C ${build_dir} install"
