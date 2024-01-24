#!/bin/bash

make format
git diff --quiet

if [ $? -ne 0 ]; then
    echo "Error: Code style check failed."
    git diff
    exit 1
fi
