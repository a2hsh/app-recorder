# Vendored libogg 1.3.6 -- the Ogg container behind src/actions/action_ogg.c.
#
# Two files. libogg is the smallest dependency in this tree by an order of
# magnitude: bitwise.c is the bit packer, framing.c is the page builder, and
# together they are what turns a sequence of Opus packets into a file that
# resynchronises after a truncation. Everything else in the release tarball is
# build scaffolding, tests or documentation.
#
# Licence: BSD-3-Clause (COPYING), which is a materially easier release
# position than vendor/lame's LGPL -- see vendor/ogg/PROVENANCE.md and design
# section 8.1. Nothing has to be relinkable; the notice has to be reproduced.
#
# WARNINGS ARE RELAXED INSIDE THIS DIRECTORY AND NOWHERE ELSE, exactly as
# vendor/lame does it and for the same reason: patching upstream to satisfy
# our lint would destroy the property that every file here diffs clean against
# the release. The inherited options are CLEARED rather than overridden,
# because appending /W0 to a command line that already carries /W4 makes MSVC
# emit D9025 for every translation unit.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

add_library(ogg STATIC
  src/bitwise.c
  src/framing.c
)

target_include_directories(ogg
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include     # <ogg/ogg.h>, for our code
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src         # crctable.h
)

# THE TRAP vendor/lame/CMakeLists.txt DOCUMENTS: clearing the directory's
# inherited options above also dropped the tree-wide Release /O1, so it has to
# be put back by hand. A vendored dependency must not quietly opt itself out
# of the project's size policy.
target_compile_options(ogg PRIVATE /W0 /WX- $<$<CONFIG:Release>:/O1>)
