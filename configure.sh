# Architecture specific code is now selected automatically by the Makefile
# based on `uname -m` (override with `make ARCH=x86_64`). Remove any stale
# symlink left behind by older versions of this script.
rm -f src/arch.c
