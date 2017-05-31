Caliper Compiler Suite (alpha)
==================================

This is an alpha release of a set of compiler tools that lives alongside Caliper.

Currently it is not fit for production use and is mainly here as a starting point 
for other tool developers, feel free to email me at poliakoff1@llnl.gov with 
questions though

Released under a BSD license, `LLNL-CODE-678900`. 
See `LICENSE` file for details.

Building
--------

See the included Makefile for fuller docs, but for many

```
make LLVM_SRC_DIR=<your clang root dir>
```

Will work if the Clang plugin system is installed. This will create an executable
in <your clone directory>/build called caliper_instrumenter

Usage
-------

That executable, caliper_instrumenter, can be run against a file to instrument all functions in it with Caliper. Work is being done to make this accept TAU or Score-P selective instrumentation files. There is also a file, caliper_instrumenter.cmake, which allows for use in cmake build systems like so

include(caliper-instrumenter.cmake)
add_[executable|library](mytarget <my source files>)
instrument_target(mytarget)


Attribution
-----------------------

Authors

David Poliakoff
David Boehme

This work was inspired by Eli Bendersky's repo (https://github.com/eliben/llvm-clang-samples) showing how to write basic Clang plugins and build them, I highly recommend that if you're interested in developing these kinds of tools
