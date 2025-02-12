#! /usr/bin/env bash

echo 'CompileFlags:                    
  Add:
    [
      # -mlong-calls,
      -DSSIZE_MAX,
      -DLWIP_NO_UNISTD_H=1,
      -Dssize_t=long,
      -D_SSIZE_T_DECLARED,
    ]
  Remove:
    [
      -fno-tree-switch-conversion,
      -mtext-section-literals,
      -mlongcalls,
      -fstrict-volatile-bitfields,
      -free,
      -fipa-pta,
    ]' >.clangd

echo "Checks: '-*, -misc-definitions-in-headers' " >.clangd-tidy

echo 'import os
Import("env")
# include toolchain paths
env.Replace(COMPILATIONDB_INCLUDE_TOOLCHAIN=True)
# override compilation DB path
env.Replace(COMPILATIONDB_PATH="compile_commands.json")' >gen_compile_commands.py
