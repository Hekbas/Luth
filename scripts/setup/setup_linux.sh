#!/usr/bin/env bash

pushd ../.. > /dev/null
extern/premake/linux/premake5 --file=premake5.lua gmake2
popd > /dev/null
#read -p "Press [Enter] to continue..."