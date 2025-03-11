#!/bin/bash

if type "clang" > /dev/null 2>&1; then
  clang *.c -Wall -Werror -O0 -g -o tinyosc
else
  gcc *.c -Wall -Werror -std=c99 -O0 -g -o tinyosc
fi
