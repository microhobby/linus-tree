#!/usr/bin/bash

# get my gid
_GID=$(id -g)

# get my uid
_UID=$(id -u)

# print it to the user
echo "Running with UID: $_UID and GID: $_GID"
