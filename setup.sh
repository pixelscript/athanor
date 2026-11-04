#!/bin/sh
# Fetch the pinned Alchemy SDK + libDaisy into lib/ and build libDaisy once.
set -e
cd "$(dirname "$0")"
mkdir -p lib
if [ ! -d lib/alchemy-sdk/.git ]; then
  git clone https://github.com/hermetic-modular/alchemy-sdk.git lib/alchemy-sdk
  git -C lib/alchemy-sdk checkout 3f40b663297708c987c28bf36065c4d1047de375   # v0.12.0
fi
if [ ! -d lib/libDaisy/.git ]; then
  git clone https://github.com/electro-smith/libDaisy.git lib/libDaisy
  git -C lib/libDaisy checkout 08f29653610c08cdf1ca7b5cfcac3bb4922e60c4
  git -C lib/libDaisy submodule update --init
fi
make libdaisy
echo "Ready: run 'make' to build build/athanor.bin"
