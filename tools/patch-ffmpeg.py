#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Trims FFmpeg's h264 and AAC decoders for the Palm. Idempotent.

- 8-bit H.264 only: other bit depths are rejected, and each DSP init
  function sees a constant depth of 8, so the compiler drops the 9- to
  14-bit code (about 0.7 MB).
- Integer transforms of up to 4096 points: no float or double codelets and
  no large tables (about 1.1 MB of code and BSS).
- AAC core only: SBR and parametric stereo (HE-AAC) data is skipped, and
  their state is never allocated. That saves 430 KB of heap per channel
  element. HE-AAC streams decode at half bandwidth.

  patch-ffmpeg.py FFMPEG_SOURCE_DIR
"""

import os
import sys

EDITS = [
    ("libavcodec/h264_slice.c",
     "    if (sps->bit_depth_luma < 8 || sps->bit_depth_luma > 14 ||\n"
     "        sps->bit_depth_luma == 11 || sps->bit_depth_luma == 13\n"
     "    ) {",
     "    if (sps->bit_depth_luma != 8 || sps->bit_depth_chroma != 8) {"),
    ("libavcodec/h264_mb.c",
     "        if (is_complex || h->pixel_shift)\n",
     "        if (is_complex)\n"),
    ("libavcodec/h264_mb.c",
     "    } else if (h->pixel_shift) {\n"
     "        hl_decode_mb_simple_16(h, sl);\n"
     "    } else\n",
     "    } else\n"),
    ("libavcodec/h264qpel.c",
     "av_cold void ff_h264qpel_init(H264QpelContext *c, int bit_depth)\n{\n",
     "av_cold void ff_h264qpel_init(H264QpelContext *c, int bit_depth)\n{\n"
     "    bit_depth = 8;\n"),
    ("libavcodec/h264chroma.c",
     "av_cold void ff_h264chroma_init(H264ChromaContext *c, int bit_depth)\n{\n",
     "av_cold void ff_h264chroma_init(H264ChromaContext *c, int bit_depth)\n{\n"
     "    bit_depth = 8;\n"),
    ("libavcodec/videodsp.c",
     "av_cold void ff_videodsp_init(VideoDSPContext *ctx, int bpc)\n{\n",
     "av_cold void ff_videodsp_init(VideoDSPContext *ctx, int bpc)\n{\n"
     "    bpc = 8;\n"),
    ("libavcodec/h264dsp.c",
     "av_cold void ff_h264dsp_init(H264DSPContext *c, const int bit_depth,\n"
     "                             const int chroma_format_idc)\n{\n",
     "av_cold void ff_h264dsp_init(H264DSPContext *c, const int bit_depth_in,\n"
     "                             const int chroma_format_idc)\n{\n"
     "    const int bit_depth = 8;\n"
     "    (void)bit_depth_in;\n"),
    ("libavcodec/h264pred.c",
     "av_cold void ff_h264_pred_init(H264PredContext *h, int codec_id,\n"
     "                               const int bit_depth,\n"
     "                               int chroma_format_idc)\n{\n",
     "av_cold void ff_h264_pred_init(H264PredContext *h, int codec_id,\n"
     "                               const int bit_depth_in,\n"
     "                               int chroma_format_idc)\n{\n"
     "    const int bit_depth = 8;\n"
     "    (void)bit_depth_in;\n"),
    ("libavutil/tx.c",
     "    ff_tx_codelet_list_float_c,\n"
     "    ff_tx_codelet_list_double_c,\n"
     "    ff_tx_codelet_list_int32_c,\n",
     "    ff_tx_codelet_list_int32_c,\n"),
    ("libavutil/tx_template.c",
     "    SR_TABLE(4096)     \\\n"
     "    SR_TABLE(8192)     \\\n"
     "    SR_TABLE(16384)    \\\n"
     "    SR_TABLE(32768)    \\\n"
     "    SR_TABLE(65536)    \\\n"
     "    SR_TABLE(131072)   \\\n",
     "    SR_TABLE(4096)     \\\n"),
    ("libavutil/tx_template.c",
     "        int idx = factor_2 - 3;\n",
     "        int idx = FFMIN(factor_2 - 3,\n"
     "                        (int)FF_ARRAY_ELEMS(sr_tabs_init_funcs) - 1);\n"),
    ("libavutil/tx_template.c",
     "DECL_SR_CODELET(8192,4096,2048)\n"
     "DECL_SR_CODELET(16384,8192,4096)\n"
     "DECL_SR_CODELET(32768,16384,8192)\n"
     "DECL_SR_CODELET(65536,32768,16384)\n"
     "DECL_SR_CODELET(131072,65536,32768)\n",
     ""),
    ("libavutil/tx_template.c",
     "    &TX_NAME(ff_tx_fft8192_ns_def),\n"
     "    &TX_NAME(ff_tx_fft16384_ns_def),\n"
     "    &TX_NAME(ff_tx_fft32768_ns_def),\n"
     "    &TX_NAME(ff_tx_fft65536_ns_def),\n"
     "    &TX_NAME(ff_tx_fft131072_ns_def),\n",
     ""),
    ("libavcodec/aac/aacdec.c",
     "    if (count_channels(layout_map, tags) > 1) {\n"
     "        m4ac->ps = 0;\n",
     "    m4ac->sbr = 0;\n"
     "    m4ac->ps = 0;\n"
     "    if (count_channels(layout_map, tags) > 1) {\n"
     "        m4ac->ps = 0;\n"),
    ("libavcodec/aac/aacdec.c",
     "    case EXT_SBR_DATA:\n"
     "        if (!che) {\n"
     "            av_log(ac->avctx, AV_LOG_ERROR, \"SBR was found before the first channel element.\\n\");\n"
     "            return res;\n"
     "        } else if (!ac->oc[1].m4ac.sbr) {\n"
     "            av_log(ac->avctx, AV_LOG_ERROR, \"SBR signaled to be not-present but was found in the bitstream.\\n\");\n"
     "            skip_bits_long(gb, 8 * cnt - 4);\n"
     "            return res;\n"
     "        } else if (ac->oc[1].m4ac.sbr == -1 && ac->oc[1].status == OC_LOCKED) {\n"
     "            av_log(ac->avctx, AV_LOG_ERROR, \"Implicit SBR was found with a first occurrence after the first frame.\\n\");\n"
     "            skip_bits_long(gb, 8 * cnt - 4);\n"
     "            return res;\n"
     "        } else if (ac->oc[1].m4ac.ps == -1 && ac->oc[1].status < OC_LOCKED &&\n"
     "                   ac->avctx->ch_layout.nb_channels == 1) {\n"
     "            ac->oc[1].m4ac.sbr = 1;\n"
     "            ac->oc[1].m4ac.ps = 1;\n"
     "            ac->avctx->profile = AV_PROFILE_AAC_HE_V2;\n"
     "            ff_aac_output_configure(ac, ac->oc[1].layout_map, ac->oc[1].layout_map_tags,\n"
     "                                    ac->oc[1].status, 1);\n"
     "        } else {\n"
     "            ac->oc[1].m4ac.sbr = 1;\n"
     "            ac->avctx->profile = AV_PROFILE_AAC_HE;\n"
     "        }\n"
     "\n"
     "        ac->proc.sbr_decode_extension(ac, che, gb, crc_flag, cnt, elem_type,\n"
     "                                      ac->oc[1].m4ac.frame_length_short);\n"
     "\n"
     "        if (ac->oc[1].m4ac.ps == 1 && !ac->warned_he_aac_mono) {\n"
     "            av_log(ac->avctx, AV_LOG_VERBOSE, \"Treating HE-AAC mono as stereo.\\n\");\n"
     "            ac->warned_he_aac_mono = 1;\n"
     "        }\n"
     "        break;\n",
     "    case EXT_SBR_DATA:\n"
     "        (void)crc_flag;\n"
     "        skip_bits_long(gb, 8 * cnt - 4);\n"
     "        return res;\n"),
    ("libavcodec/aacsbr_template.c",
     "    ExtChannelElement *ext = av_mallocz(sizeof(*ext));\n",
     "    ExtChannelElement *ext = av_mallocz(offsetof(ExtChannelElement, sbr));\n"),
    ("libavcodec/aacsbr_template.c",
     "    ext->ch.ch[1].AAC_RENAME(predictor_state) = ext->predictor_state[1];\n",
     "    ext->ch.ch[1].AAC_RENAME(predictor_state) = ext->predictor_state[1];\n"
     "    return 0;\n"),
    ("libavcodec/aacsbr_template.c",
     "    SpectralBandReplication *sbr = get_sbr(che);\n"
     "    av_tx_uninit(&sbr->mdct);\n"
     "    av_tx_uninit(&sbr->mdct_ana);\n",
     "    (void)che;\n"),
]


def main(source):
    applied = skipped = 0
    for path, old, new in EDITS:
        full = os.path.join(source, path)
        with open(full) as handle:
            text = handle.read()
        # Insertions keep the old text, so only the new text shows they ran.
        # For replacements and removals, the old text means "not yet".
        inserting = old in new
        if inserting and new in text:
            skipped += 1
        elif old in text:
            text = text.replace(old, new, 1)
            with open(full, "w") as handle:
                handle.write(text)
            applied += 1
        elif not inserting and (not new or new in text):
            skipped += 1
        else:
            sys.exit(f"patch-ffmpeg: {path}: expected text not found")
    print(f"patch-ffmpeg: {applied} applied, {skipped} already present")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
