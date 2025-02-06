#!/bin/bash

make coverage

target=89.0
p=$(lcov --summary coverage.info | grep "lines" | grep -Eo "[0-9]+\.[0-9]+")
if (( $(echo "$p < $target" | bc -l) )); then
    echo "Error: Coverage $p% is less than $target%"
    exit 1 
fi
