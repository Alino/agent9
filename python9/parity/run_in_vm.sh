#!/bin/sh
# Run the parity suite INSIDE the 9front VM against the ported interpreter and
# copy the manifest back to the host.
#
# STATUS: scaffold. The port binary does not exist yet, so this documents the
# intended flow and the knobs. It will be wired up once step 1 (a python that
# starts in the VM) lands. The host-side reference manifest does not depend on
# this -- it is captured purely on the host.
#
# Intended flow (9front side, driven over the VM's exported namespace / drawterm
# or the existing scripts/vm-rc plumbing used by the rest of agent9):
#   1. mount the host dir holding python9/cpython/src (Lib/test) into the VM
#   2. run:  python9/parity/run_suite.py --python /path/to/ported/python \
#              --out /tmp/9front-port.json --jobs 1
#      (jobs=1: the VM is single-cpu and regrtest -mp assumptions are POSIX-y)
#   3. copy /tmp/9front-port.json back to python9/parity/manifests/
#
# Then on the host:
#   score.py --reference manifests/host-reference.json \
#            --port      manifests/9front-port.json \
#            --skiplist  skiplist.txt
set -e
echo "run_in_vm.sh is a scaffold; see comments. No port binary exists yet." >&2
exit 2
