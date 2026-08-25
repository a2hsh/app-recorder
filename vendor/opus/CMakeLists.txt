# Vendored libopus 1.5.2 -- the Opus encoder behind src/actions/action_ogg.c.
#
# Built as ONE static library, like vendor/lame, so the single-exe goal holds:
# there is no opus.dll to ship, find or version.
#
# Licence: BSD-3-Clause (COPYING). Unlike vendor/lame's LGPL there is no
# relink obligation to satisfy at release time -- reproducing the copyright
# notice is the whole of it. See PROVENANCE.md and design section 8.1.
#
# WHAT IS AND IS NOT COMPILED is argued in vendor/opus/config.h; the short
# form is float (not FIXED_POINT), portable C (no runtime CPU dispatch, so no
# celt/x86, silk/x86 or arm trees), and no DNN features (so no dnn/, which is
# 18 MB of the 22 MB release and every reference to it in silk/, celt/ and
# src/ sits inside an #ifdef we leave undefined).
#
# THE DECODER IS IN THIS LIBRARY, and unlike vendor/lame's hip_* it is NOT
# claimed to be absent from the shipping image. That claim was measured and
# does not hold up: forcing a reachable opus_decoder_create into
# apprecorder.exe grew the Release image by 2 KB, and so did forcing in
# opus_multistream_encoder_create -- an experiment that cannot tell "already
# linked" from "the probe was dead-stripped" apart, so no conclusion is drawn
# from it.
#
# What IS measured: Ogg/Opus support costs 220.5 KB of the Release image
# (784,896 bytes with src/actions/action_ogg.c present, 559,104 without).
# That is the number to hold this dependency to. It is also the number to
# expect: LAME's decoder is a separate library half that shares nothing with
# the encoder, whereas Opus's decoder shares the entropy coder, the MDCT, the
# FFT, the mode tables and most of SILK with the encoder, so there is no
# encoder-only build of Opus to aim at in the first place.
#
# WARNINGS ARE RELAXED INSIDE THIS DIRECTORY AND NOWHERE ELSE. The root
# CMakeLists sets /W4 /WX for apprecorder's own code and that stays true.
# The inherited options are CLEARED rather than overridden, because appending
# /W0 to a command line that already carries /W4 makes MSVC emit D9025 for
# every one of the 137 translation units below.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

set(OPUS_CELT_SRC
  celt/bands.c
  celt/celt.c
  celt/celt_decoder.c
  celt/celt_encoder.c
  celt/celt_lpc.c
  celt/cwrs.c
  celt/entcode.c
  celt/entdec.c
  celt/entenc.c
  celt/kiss_fft.c
  celt/laplace.c
  celt/mathops.c
  celt/mdct.c
  celt/modes.c
  celt/pitch.c
  celt/quant_bands.c
  celt/rate.c
  celt/vq.c
)

# SILK. The float path only -- silk/fixed/ is not vendored (config.h note 1).
set(OPUS_SILK_SRC
  silk/A2NLSF.c
  silk/ana_filt_bank_1.c
  silk/biquad_alt.c
  silk/bwexpander.c
  silk/bwexpander_32.c
  silk/check_control_input.c
  silk/CNG.c
  silk/code_signs.c
  silk/control_audio_bandwidth.c
  silk/control_codec.c
  silk/control_SNR.c
  silk/debug.c
  silk/dec_API.c
  silk/decode_core.c
  silk/decode_frame.c
  silk/decode_indices.c
  silk/decode_parameters.c
  silk/decode_pitch.c
  silk/decode_pulses.c
  silk/decoder_set_fs.c
  silk/enc_API.c
  silk/encode_indices.c
  silk/encode_pulses.c
  silk/gain_quant.c
  silk/HP_variable_cutoff.c
  silk/init_decoder.c
  silk/init_encoder.c
  silk/inner_prod_aligned.c
  silk/interpolate.c
  silk/lin2log.c
  silk/log2lin.c
  silk/LP_variable_cutoff.c
  silk/LPC_analysis_filter.c
  silk/LPC_fit.c
  silk/LPC_inv_pred_gain.c
  silk/NLSF_decode.c
  silk/NLSF_del_dec_quant.c
  silk/NLSF_encode.c
  silk/NLSF_stabilize.c
  silk/NLSF_unpack.c
  silk/NLSF_VQ.c
  silk/NLSF_VQ_weights_laroia.c
  silk/NLSF2A.c
  silk/NSQ.c
  silk/NSQ_del_dec.c
  silk/pitch_est_tables.c
  silk/PLC.c
  silk/process_NLSFs.c
  silk/quant_LTP_gains.c
  silk/resampler.c
  silk/resampler_down2.c
  silk/resampler_down2_3.c
  silk/resampler_private_AR2.c
  silk/resampler_private_down_FIR.c
  silk/resampler_private_IIR_FIR.c
  silk/resampler_private_up2_HQ.c
  silk/resampler_rom.c
  silk/shell_coder.c
  silk/sigm_Q15.c
  silk/sort.c
  silk/stereo_decode_pred.c
  silk/stereo_encode_pred.c
  silk/stereo_find_predictor.c
  silk/stereo_LR_to_MS.c
  silk/stereo_MS_to_LR.c
  silk/stereo_quant_pred.c
  silk/sum_sqr_shift.c
  silk/table_LSF_cos.c
  silk/tables_gain.c
  silk/tables_LTP.c
  silk/tables_NLSF_CB_NB_MB.c
  silk/tables_NLSF_CB_WB.c
  silk/tables_other.c
  silk/tables_pitch_lag.c
  silk/tables_pulses_per_block.c
  silk/VAD.c
  silk/VQ_WMat_EC.c
  silk/float/apply_sine_window_FLP.c
  silk/float/autocorrelation_FLP.c
  silk/float/burg_modified_FLP.c
  silk/float/bwexpander_FLP.c
  silk/float/corrMatrix_FLP.c
  silk/float/encode_frame_FLP.c
  silk/float/energy_FLP.c
  silk/float/find_LPC_FLP.c
  silk/float/find_LTP_FLP.c
  silk/float/find_pitch_lags_FLP.c
  silk/float/find_pred_coefs_FLP.c
  silk/float/inner_product_FLP.c
  silk/float/k2a_FLP.c
  silk/float/LPC_analysis_filter_FLP.c
  silk/float/LPC_inv_pred_gain_FLP.c
  silk/float/LTP_analysis_filter_FLP.c
  silk/float/LTP_scale_ctrl_FLP.c
  silk/float/noise_shape_analysis_FLP.c
  silk/float/pitch_analysis_core_FLP.c
  silk/float/process_gains_FLP.c
  silk/float/regularize_correlations_FLP.c
  silk/float/residual_energy_FLP.c
  silk/float/scale_copy_vector_FLP.c
  silk/float/scale_vector_FLP.c
  silk/float/schur_FLP.c
  silk/float/sort_FLP.c
  silk/float/warped_autocorrelation_FLP.c
  silk/float/wrappers_FLP.c
)

# The Opus layer itself. analysis.c / mlp.c / mlp_data.c are upstream's
# OPUS_SOURCES_FLOAT: the music-vs-speech classifier that picks SILK, CELT or
# hybrid per frame. A recorder gets a mixture of both and wants that decision
# made well.
set(OPUS_API_SRC
  src/analysis.c
  src/extensions.c
  src/mapping_matrix.c
  src/mlp.c
  src/mlp_data.c
  src/opus.c
  src/opus_decoder.c
  src/opus_encoder.c
  src/opus_multistream.c
  src/opus_multistream_decoder.c
  src/opus_multistream_encoder.c
  src/opus_projection_decoder.c
  src/opus_projection_encoder.c
  src/repacketizer.c
)

add_library(opus STATIC ${OPUS_CELT_SRC} ${OPUS_SILK_SRC} ${OPUS_API_SRC})

target_include_directories(opus
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include     # <opus.h>, for our code
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}             # config.h
          ${CMAKE_CURRENT_SOURCE_DIR}/celt
          ${CMAKE_CURRENT_SOURCE_DIR}/silk
          ${CMAKE_CURRENT_SOURCE_DIR}/silk/float
          ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# OPUS_BUILD and USE_ALLOCA are where upstream's own CMake puts them rather
# than in config.h. USE_ALLOCA because MSVC has no variable-length arrays and
# celt/stack_alloc.h #errors out if no allocation mode is chosen.
target_compile_definitions(opus PRIVATE HAVE_CONFIG_H OPUS_BUILD USE_ALLOCA)

# The directory inherited nothing (above), so this is the whole command line --
# which means the tree-wide Release /O1 has to be put back by hand. This is
# the trap vendor/lame/CMakeLists.txt documents, and it costs the same thing
# here: without the line this vendored library would silently compile at the
# compiler's default /O2 and grow the image the whole build is shaped around
# keeping small.
#
# /wd4996 is for the CRT deprecation warnings upstream trips; /wd4244 /wd4267
# for the narrowing conversions a codebase written to C89 rules is full of.
target_compile_options(opus PRIVATE
  /W0 /WX- /wd4996 /wd4244 /wd4267 $<$<CONFIG:Release>:/O1>)
