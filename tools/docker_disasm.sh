#!/bin/sh
# Builds pret's disasm (disasm/build/pokestadium-us.elf) inside a Debian
# container, for Windows hosts without a MIPS toolchain. Run from the repo root:
#   docker run --rm -v "%CD%:/src" -w /src/disasm debian:bookworm sh /src/tools/docker_disasm.sh
set -e
apt-get update -qq
apt-get install -y -qq make git build-essential binutils-mips-linux-gnu \
    python3 python3-pip python3-venv >/dev/null
# Keep the venv outside the bind mount so it doesn't land in the Windows tree.
python3 -m venv /opt/venv
. /opt/venv/bin/activate
pip install -q -r requirements.txt
git config --global --add safe.directory '*'
make init
make
md5sum build/pokestadium-us.z64
ls -la build/pokestadium-us.elf
