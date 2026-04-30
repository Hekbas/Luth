#!/usr/bin/env bash

if ! command -v make &> /dev/null; then
    echo "ERROR: make is not installed."
    exit 1
fi

pushd ../.. > /dev/null
make "$@"
popd > /dev/null

#read -p "Press enter to continue..."