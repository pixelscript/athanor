#!/bin/sh
# Fetch the pinned Alchemy SDK + libDaisy into lib/ and build libDaisy once.
# In a git checkout this uses the submodules; in a plain copy it clones the
# same pinned commits.
set -e
cd "$(dirname "$0")"
if [ -d .git ] || git rev-parse --git-dir >/dev/null 2>&1; then
  git submodule update --init --recursive
else
  mkdir -p lib
  if [ ! -d lib/alchemy-sdk/.git ]; then
    git clone https://github.com/hermetic-modular/alchemy-sdk.git lib/alchemy-sdk
    git -C lib/alchemy-sdk checkout c8b8925   # v0.13.0
  fi
  if [ ! -d lib/libDaisy/.git ]; then
    git clone https://github.com/electro-smith/libDaisy.git lib/libDaisy
    git -C lib/libDaisy checkout 08f29653610c08cdf1ca7b5cfcac3bb4922e60c4
    git -C lib/libDaisy submodule update --init
  fi
fi
make libdaisy
echo "Ready: run 'make' to build build/athanor.bin"
