#!/bin/bash

# CaaL - Container as a Login
# presetup: clones the git repository, and executes setup.sh
# https://github.com/douxxtech/caal

command -v git >/dev/null 2>&1 || { echo "git is not installed. Install it first"; exit 1; }

cd /tmp
git clone https://github.com/douxxtech/caal
cd caal
bash scripts/setup.sh
rm -r caal