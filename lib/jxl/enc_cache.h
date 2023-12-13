// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef LIB_JXL_ENC_CACHE_H_
#define LIB_JXL_ENC_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "lib/jxl/ac_strategy.h"
#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/coeff_order.h"
#include "lib/jxl/coeff_order_fwd.h"
#include "lib/jxl/dct_util.h"
#include "lib/jxl/enc_ans.h"
#include "lib/jxl/enc_params.h"
#include "lib/jxl/enc_progressive_split.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/passes_state.h"
#include "lib/jxl/quant_weights.h"
#include "lib/jxl/quantizer.h"

namespace jxl {

struct AuxOut;

// Contains encoder state.
struct PassesEncoderState {
  PassesSharedState shared;

  bool streaming_mode = false;
  bool initialize_global_state = true;
  size_t dc_group_index = 0;

  ImageF initial_quant_field;    // Invalid in Falcon mode.
  ImageF initial_quant_masking;  // Invalid in Falcon mode.
  ImageF initial_quant_masking1x1;  // Invalid in Falcon mode.

  // Per-pass DCT coefficients for the image. One row per group.
  std::vector<std::unique_ptr<ACImage>> coeffs;

  // Raw data for special (reference+DC) frames.
  std::vector<std::unique_ptr<BitWriter>> special_frames;

  // For splitting into passes.
  ProgressiveSplitter progressive_splitter;

  CompressParams cparams;

  struct PassData {
    std::vector<std::vector<Token>> ac_tokens;
    std::vector<uint8_t> context_map;
    EntropyEncodingData codes;
  };

  std::vector<PassData> passes;
  std::vector<uint8_t> histogram_idx;

  // Block sizes seen so far.
  uint32_t used_acs = 0;
  // Coefficient orders that are non-default.
  std::vector<uint32_t> used_orders;

  // Multiplier to be applied to the quant matrices of the x channel.
  float x_qm_multiplier = 1.0f;
  float b_qm_multiplier = 1.0f;
};

// Initialize per-frame information.
class ModularFrameEncoder;
Status InitializePassesEncoder(const FrameHeader& frame_header,
                               const Image3F& opsin, const JxlCmsInterface& cms,
                               ThreadPool* pool,
                               PassesEncoderState* passes_enc_state,
                               ModularFrameEncoder* modular_frame_encoder,
                               AuxOut* aux_out);

}  // namespace jxl

#endif  // LIB_JXL_ENC_CACHE_H_
