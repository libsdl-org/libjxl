// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "jxl/loop_filter.h"

#include "jxl/aux_out.h"
#include "jxl/fields.h"

namespace jxl {

LoopFilter::LoopFilter() { Bundle::Init(this); }
Status LoopFilter::VisitFields(Visitor* JXL_RESTRICT visitor) {
  // Must come before AllDefault.

  if (visitor->AllDefault(*this, &all_default)) {
    // Overwrite all serialized fields, but not any nonserialized_*.
    visitor->SetDefault(this);
    return true;
  }

  JXL_QUIET_RETURN_IF_ERROR(visitor->Bool(true, &gab));
  if (visitor->Conditional(gab)) {
    JXL_QUIET_RETURN_IF_ERROR(visitor->Bool(false, &gab_custom));
    if (visitor->Conditional(gab_custom)) {
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->F16(1.1 * 0.104699568f, &gab_x_weight1));
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->F16(1.1 * 0.055680538f, &gab_x_weight2));
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->F16(1.1 * 0.104699568f, &gab_y_weight1));
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->F16(1.1 * 0.055680538f, &gab_y_weight2));
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->F16(1.1 * 0.104699568f, &gab_b_weight1));
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->F16(1.1 * 0.055680538f, &gab_b_weight2));
    }
  }

  JXL_QUIET_RETURN_IF_ERROR(visitor->Bits(2, 2, &epf_iters));
  if (visitor->Conditional(epf_iters > 0)) {
    JXL_QUIET_RETURN_IF_ERROR(visitor->Bool(false, &epf_sharp_custom));
    if (visitor->Conditional(epf_sharp_custom)) {
      for (size_t i = 0; i < kEpfSharpEntries; ++i) {
        JXL_QUIET_RETURN_IF_ERROR(visitor->F16(
            float(i) / float(kEpfSharpEntries - 1), &epf_sharp_lut[i]));
      }
    }

    JXL_QUIET_RETURN_IF_ERROR(visitor->Bool(false, &epf_weight_custom));
    if (visitor->Conditional(epf_weight_custom)) {
      JXL_QUIET_RETURN_IF_ERROR(visitor->F16(40.0f, &epf_channel_scale[0]));
      JXL_QUIET_RETURN_IF_ERROR(visitor->F16(5.0f, &epf_channel_scale[1]));
      JXL_QUIET_RETURN_IF_ERROR(visitor->F16(3.5f, &epf_channel_scale[2]));
      JXL_QUIET_RETURN_IF_ERROR(visitor->F16(0.45f, &epf_pass1_zeroflush));
      JXL_QUIET_RETURN_IF_ERROR(visitor->F16(0.6f, &epf_pass2_zeroflush));
    }

    JXL_QUIET_RETURN_IF_ERROR(visitor->Bool(false, &epf_sigma_custom));
    if (visitor->Conditional(epf_sigma_custom)) {
      JXL_QUIET_RETURN_IF_ERROR(visitor->F16(0.46f, &epf_quant_mul));
      JXL_QUIET_RETURN_IF_ERROR(visitor->F16(6.5f, &epf_pass2_sigma_scale));
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->F16(0.6666666666666666f, &epf_border_sad_mul));
    }
  }

  JXL_QUIET_RETURN_IF_ERROR(visitor->BeginExtensions(&extensions));
  // Extensions: in chronological order of being added to the format.
  return visitor->EndExtensions();
}

Status ReadLoopFilter(BitReader* JXL_RESTRICT reader,
                      LoopFilter* JXL_RESTRICT loop_filter) {
  return Bundle::Read(reader, loop_filter);
}

Status WriteLoopFilter(const LoopFilter& loop_filter,
                       BitWriter* JXL_RESTRICT writer, AuxOut* aux_out) {
  return Bundle::Write(loop_filter, writer, kLayerHeader, aux_out);
}

}  // namespace jxl
