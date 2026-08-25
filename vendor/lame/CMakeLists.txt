# Vendored LAME 3.100 -- the MP3 encoder behind src/actions/action_mp3.c.
#
# Built as ONE static library so the single-exe, no-runtime-dependency goal
# holds: there is no lame_enc.dll to ship, find or version.
#
# Licence: LGPL-2.0-or-later (COPYING, LICENSE). See PROVENANCE.md for the
# release this was taken from, the checksum, and how it was verified.
#
# WARNINGS ARE RELAXED INSIDE THIS DIRECTORY AND NOWHERE ELSE. The root
# CMakeLists sets /W4 /WX for apprecorder's own code and that stays true;
# a 1999 C89 codebase cannot meet it, and patching 25,000 lines of upstream to
# satisfy our lint would destroy the property that makes vendoring safe --
# that every file here can be diffed against the release tarball.
#
# The inherited options are CLEARED rather than overridden. Appending /W0 to a
# command line that already carries /W4 works, but MSVC then emits D9025 for
# every single translation unit, and 60 lines of "overriding /W4" in every
# build is exactly the kind of noise that trains people to stop reading build
# output. set_directory_properties scopes this to vendor/lame/.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

set(LAME_ENCODER_SRC
  libmp3lame/VbrTag.c
  libmp3lame/bitstream.c
  libmp3lame/encoder.c
  libmp3lame/fft.c
  libmp3lame/gain_analysis.c
  libmp3lame/id3tag.c
  libmp3lame/lame.c
  libmp3lame/newmdct.c
  libmp3lame/presets.c
  libmp3lame/psymodel.c
  libmp3lame/quantize.c
  libmp3lame/quantize_pvt.c
  libmp3lame/reservoir.c
  libmp3lame/set_get.c
  libmp3lame/tables.c
  libmp3lame/takehiro.c
  libmp3lame/util.c
  libmp3lame/vbrquantize.c
  libmp3lame/version.c
  libmp3lame/vector/xmm_quantize_sub.c
)

# hip_* -- the MPEG decoder. In the library, but unreferenced by every encoder
# object (config.h note 2), so the linker leaves it out of anything that does
# not call it. tests/test_action_mp3.c calls it; apprecorder.exe does not.
set(LAME_DECODER_SRC
  libmp3lame/mpglib_interface.c
  mpglib/common.c
  mpglib/dct64_i386.c
  mpglib/decode_i386.c
  mpglib/interface.c
  mpglib/layer1.c
  mpglib/layer2.c
  mpglib/layer3.c
  mpglib/tabinit.c
)

add_library(lame STATIC ${LAME_ENCODER_SRC} ${LAME_DECODER_SRC})

target_include_directories(lame
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include        # lame.h, for our code
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}                # config.h
          ${CMAKE_CURRENT_SOURCE_DIR}/libmp3lame
          ${CMAKE_CURRENT_SOURCE_DIR}/mpglib
)

target_compile_definitions(lame PRIVATE HAVE_CONFIG_H)

# The directory inherited nothing (above), so these are the whole set -- which
# means the tree-wide Release /O1 has to be put back by hand here. It is worth
# putting back: measured on this source, /O1 rather than the compiler's default
# /O2 costs the vendored library 226 KB and the final linked image 58 KB, which
# is 6% of the sub-1 MB budget the whole build is shaped around. The CPU it
# buys back is not needed -- tests/test_action_mp3.c pushes ~32 s of audio
# through the encoder and back out through the decoder in 1.1 s of Release wall
# clock and 2.5 s of Debug, and this action has a four-second write-behind in
# front of it. A vendored dependency should not quietly opt itself out of
# the project's size policy; if MP3 encoding ever does become the bottleneck,
# this one line is where to change your mind.
target_compile_options(lame PRIVATE /W0 /WX- /wd4996 $<$<CONFIG:Release>:/O1>)
