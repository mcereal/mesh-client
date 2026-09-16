# Sourced, not run. One helper, shared by every script in here that configures a build tree.
#
# A CMake build tree remembers the generator it was created with and refuses to be reconfigured
# under another one. This project moved from Unix Makefiles to Ninja, so the first build after
# pulling that change would otherwise stop on an error about a tree the caller did not know they
# had - on a host, in the dev container, and in the fuzz and cross trees alike.
#
# A build tree is an artifact: `make clean` deletes it and nothing in it is authored. So migrate
# it rather than stopping to ask.

mesh_reset_stale_tree() {
    local dir="$1"
    local generator="${2:-Ninja}"
    local cache="${dir}/CMakeCache.txt"

    if [[ -f "${cache}" ]] && ! grep -q "^CMAKE_GENERATOR:INTERNAL=${generator}\$" "${cache}"; then
        echo "${dir} was configured with another generator; rebuilding it under ${generator}." >&2
        rm -rf "${dir}"
    fi
}
