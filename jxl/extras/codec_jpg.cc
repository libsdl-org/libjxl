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

#include "jxl/extras/codec_jpg.h"

#include <stddef.h>
#include <stdio.h>
// After stddef/stdio
#include <jpeglib.h>
#include <setjmp.h>
#include <stdint.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

// Brunsli JPEG en/decoder
#include <brunsli/jpeg_data_reader.h>
#include <brunsli/jpeg_data_writer.h>

#include "jxl/base/compiler_specific.h"
#include "jxl/color_encoding.h"
#include "jxl/color_management.h"
#include "jxl/common.h"
#include "jxl/image.h"
#include "jxl/image_bundle.h"
#include "jxl/image_ops.h"
#include "jxl/luminance.h"
#if JPEGXL_ENABLE_SJPEG
#include "sjpeg.h"
#endif

#ifdef MEMORY_SANITIZER
#include "sanitizer/msan_interface.h"
#endif

namespace jxl {

namespace {

constexpr float kJPEGSampleMultiplier = 1 << (BITS_IN_JSAMPLE - 8);
constexpr unsigned char kICCSignature[12] = {
    0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00};
constexpr int kICCMarker = JPEG_APP0 + 2;
constexpr size_t kMaxBytesInMarker = 65533;

constexpr float kJPEGSampleMin = std::numeric_limits<JSAMPLE>::min();
constexpr float kJPEGSampleMax = std::numeric_limits<JSAMPLE>::max();

bool MarkerIsICC(const jpeg_saved_marker_ptr marker) {
  return marker->marker == kICCMarker &&
         marker->data_length >= sizeof kICCSignature + 2 &&
         std::equal(std::begin(kICCSignature), std::end(kICCSignature),
                    marker->data);
}

Status ReadICCProfile(jpeg_decompress_struct* const cinfo,
                      PaddedBytes* const icc) {
  // Markers are 1-indexed, and we keep them that way in this vector to get a
  // convenient 0 at the front for when we compute the offsets later.
  std::vector<size_t> marker_lengths;
  int num_markers = 0;
  bool has_num_markers = false;
  for (jpeg_saved_marker_ptr marker = cinfo->marker_list; marker != nullptr;
       marker = marker->next) {
#ifdef MEMORY_SANITIZER
    // marker is initialized by libjpeg, which we are not instrumenting with
    // msan.
    __msan_unpoison(marker, sizeof(*marker));
    __msan_unpoison(marker->data, marker->data_length);
#endif
    if (!MarkerIsICC(marker)) continue;

    const int current_marker = marker->data[sizeof kICCSignature];
    const int current_num_markers = marker->data[sizeof kICCSignature + 1];
    if (current_marker > current_num_markers) {
      return JXL_FAILURE("inconsistent JPEG ICC marker numbering");
    }
    if (has_num_markers) {
      if (current_num_markers != num_markers) {
        return JXL_FAILURE("inconsistent numbers of JPEG ICC markers");
      }
    } else {
      num_markers = current_num_markers;
      has_num_markers = true;
      marker_lengths.resize(num_markers + 1);
    }

    if (marker_lengths[current_marker] != 0) {
      return JXL_FAILURE("duplicate JPEG ICC marker number");
    }
    marker_lengths[current_marker] =
        marker->data_length - sizeof kICCSignature - 2;
  }

  if (marker_lengths.empty()) {
    // Not an error.
    return false;
  }

  std::vector<size_t> offsets = std::move(marker_lengths);
  std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
  icc->resize(offsets.back());

  for (jpeg_saved_marker_ptr marker = cinfo->marker_list; marker != nullptr;
       marker = marker->next) {
    if (!MarkerIsICC(marker)) continue;
    const uint8_t* first = marker->data + sizeof kICCSignature + 2;
    size_t count = marker->data_length - sizeof kICCSignature - 2;
    size_t offset = offsets[marker->data[sizeof kICCSignature] - 1];
    if (offset + count > icc->size()) {
      // TODO(lode): catch this issue earlier at the root cause of this.
      return JXL_FAILURE("ICC out of bounds");
    }
    std::copy_n(first, count, icc->data() + offset);
  }

  return true;
}

void WriteICCProfile(jpeg_compress_struct* const cinfo,
                     const PaddedBytes& icc) {
  constexpr size_t kMaxIccBytesInMarker =
      kMaxBytesInMarker - sizeof kICCSignature - 2;
  const int num_markers =
      static_cast<int>(DivCeil(icc.size(), kMaxIccBytesInMarker));
  size_t begin = 0;
  for (int current_marker = 0; current_marker < num_markers; ++current_marker) {
    const auto length = std::min(kMaxIccBytesInMarker, icc.size() - begin);
    jpeg_write_m_header(
        cinfo, kICCMarker,
        static_cast<unsigned int>(length + sizeof kICCSignature + 2));
    for (const unsigned char c : kICCSignature) {
      jpeg_write_m_byte(cinfo, c);
    }
    jpeg_write_m_byte(cinfo, current_marker + 1);
    jpeg_write_m_byte(cinfo, num_markers);
    for (int i = 0; i < length; ++i) {
      jpeg_write_m_byte(cinfo, icc[begin]);
      ++begin;
    }
  }
}

Status SetChromaSubsampling(const YCbCrChromaSubsampling& chroma_subsampling,
                            jpeg_compress_struct* const cinfo) {
  for (size_t i = 0; i < 3; i++) {
    cinfo->comp_info[i].h_samp_factor =
        1 << (chroma_subsampling.MaxHShift() -
              chroma_subsampling.HShift(i < 2 ? i ^ 1 : i));
    cinfo->comp_info[i].v_samp_factor =
        1 << (chroma_subsampling.MaxVShift() -
              chroma_subsampling.VShift(i < 2 ? i ^ 1 : i));
  }
  return true;
}

void MyErrorExit(j_common_ptr cinfo) {
  jmp_buf* env = static_cast<jmp_buf*>(cinfo->client_data);
  (*cinfo->err->output_message)(cinfo);
  jpeg_destroy_decompress(reinterpret_cast<j_decompress_ptr>(cinfo));
  longjmp(*env, 1);
}

void MyOutputMessage(j_common_ptr cinfo) {
#if JXL_DEBUG_WARNING == 1
  char buf[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buf);
  JXL_WARNING("%s", buf);
#endif
}

using ByteSpan = Span<const uint8_t>;
bool GetMarkerPayload(const uint8_t* data, size_t size, ByteSpan* payload) {
  if (size < 3) {
    return false;
  }
  size_t hi = data[1];
  size_t lo = data[2];
  size_t internal_size = (hi << 8u) | lo;
  // Second byte of marker is not counted towards size.
  if (internal_size != size - 1) {
    return false;
  }
  // cut second marker byte and "length" from payload.
  *payload = ByteSpan(data, size);
  payload->remove_prefix(3);
  return true;
}

constexpr uint8_t kApp2 = 0xE2;
const uint8_t kIccProfileTag[] = {'I', 'C', 'C', '_', 'P', 'R',
                                  'O', 'F', 'I', 'L', 'E', 0x00};
Status ParseChunkedMarker(const brunsli::JPEGData& src, uint8_t marker_type,
                          const ByteSpan& tag, PaddedBytes* output) {
  output->clear();

  std::vector<ByteSpan> chunks;
  std::vector<bool> presence;
  size_t expected_number_of_parts = 0;
  bool is_first_chunk = true;
  for (const auto& marker : src.app_data) {
    if (marker.empty() || marker[0] != marker_type) {
      continue;
    }
    ByteSpan payload;
    if (!GetMarkerPayload(marker.data(), marker.size(), &payload)) {
      // Something is wrong with this marker; does not care.
      continue;
    }
    if ((payload.size() < tag.size()) ||
        memcmp(payload.data(), tag.data(), tag.size()) != 0) {
      continue;
    }
    payload.remove_prefix(tag.size());
    if (payload.size() < 2) {
      return JXL_FAILURE("Chunk is too small.");
    }
    uint8_t index = payload[0];
    uint8_t total = payload[1];
    payload.remove_prefix(2);

    JXL_RETURN_IF_ERROR(total != 0);
    if (is_first_chunk) {
      is_first_chunk = false;
      expected_number_of_parts = total;
      // 1-based indices; 0-th element is added for convenience.
      chunks.resize(total + 1);
      presence.resize(total + 1);
    } else {
      JXL_RETURN_IF_ERROR(expected_number_of_parts == total);
    }

    if (index == 0 || index > total) {
      return JXL_FAILURE("Invalid chunk index.");
    }

    if (presence[index]) {
      return JXL_FAILURE("Duplicate chunk.");
    }
    presence[index] = true;
    chunks[index] = payload;
  }

  for (size_t i = 0; i < expected_number_of_parts; ++i) {
    // 0-th element is not used.
    size_t index = i + 1;
    if (!presence[index]) {
      return JXL_FAILURE("Missing chunk.");
    }
    output->append(chunks[index]);
  }

  return true;
}
void SetColorEncodingFromJpegData(const brunsli::JPEGData& jpg,
                                  ColorEncoding* color_encoding) {
  PaddedBytes icc_profile;
  if (!ParseChunkedMarker(jpg, kApp2, ByteSpan(kIccProfileTag), &icc_profile)) {
    JXL_WARNING("ReJPEG: corrupted ICC profile\n");
    icc_profile.clear();
  }

  if (!color_encoding->SetICC(std::move(icc_profile))) {
    bool is_gray = (jpg.components.size() == 1);
    *color_encoding = ColorEncoding::SRGB(is_gray);
  }
}

}  // namespace

Status DecodeImageJPG(const Span<const uint8_t> bytes, ThreadPool* pool,
                      CodecInOut* io) {
  // Don't do anything for non-JPEG files (no need to report an error)
  if (!IsJPG(bytes)) return false;
  const DecodeTarget target = io->dec_target;

  // Use brunsli JPEG decoder to read quantized coefficients.
  if (target == DecodeTarget::kQuantizedCoeffs) {
    io->frames.clear();
    io->frames.reserve(1);
    io->frames.push_back(ImageBundle(&io->metadata));
    io->Main().jpeg_data = make_unique<brunsli::JPEGData>();
    brunsli::JPEGData* jpeg_data = io->Main().jpeg_data.get();
    if (!ReadJpeg(bytes.data(), bytes.size(), brunsli::JPEG_READ_ALL,
                  jpeg_data)) {
      return JXL_FAILURE("Error reading JPEG");
    }
    SetColorEncodingFromJpegData(*jpeg_data, &io->metadata.color_encoding);
    size_t nbcomp = jpeg_data->components.size();
    if (nbcomp != 1 && nbcomp != 3) {
      return JXL_FAILURE(
          "Cannot recompress JPEGs with neither 1 nor 3 channels");
    }
    YCbCrChromaSubsampling cs;
    if (nbcomp == 3) {
      uint8_t hsample[3], vsample[3];
      for (size_t i = 0; i < nbcomp; i++) {
        hsample[i] = jpeg_data->components[i].h_samp_factor;
        vsample[i] = jpeg_data->components[i].v_samp_factor;
      }
      JXL_RETURN_IF_ERROR(cs.Set(hsample, vsample));
    }
    // TODO(veluca): This is just a guess, but it's similar to what libjpeg
    // does.
    bool is_rgb = nbcomp == 3 && jpeg_data->components[0].id == 'R' &&
                  jpeg_data->components[1].id == 'G' &&
                  jpeg_data->components[2].id == 'B';

    io->Main().chroma_subsampling = cs;
    io->Main().color_transform =
        !is_rgb ? ColorTransform::kYCbCr : ColorTransform::kNone;

    io->metadata.SetIntensityTarget(
        io->target_nits != 0 ? io->target_nits : kDefaultIntensityTarget);
    io->metadata.SetUintSamples(BITS_IN_JSAMPLE);
    io->SetFromImage(Image3F(jpeg_data->width, jpeg_data->height),
                     io->metadata.color_encoding);
    return true;
  }

  // TODO(veluca): use JPEGData also for pixels?

  // We need to declare all the non-trivial destructor local variables before
  // the call to setjmp().
  ColorEncoding color_encoding;
  PaddedBytes icc;
  Image3F coeffs;
  Image3F image;
  std::unique_ptr<JSAMPLE[]> row;
  ImageBundle bundle(&io->metadata);

  const auto try_catch_block = [&]() -> bool {
    jpeg_decompress_struct cinfo;
#ifdef MEMORY_SANITIZER
    // cinfo is initialized by libjpeg, which we are not instrumenting with
    // msan, therefore we need to initialize cinfo here.
    memset(&cinfo, 0, sizeof(cinfo));
#endif
    // Setup error handling in jpeg library so we can deal with broken jpegs in
    // the fuzzer.
    jpeg_error_mgr jerr;
    jmp_buf env;
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = &MyErrorExit;
    jerr.output_message = &MyOutputMessage;
    if (setjmp(env)) {
      return false;
    }
    cinfo.client_data = static_cast<void*>(&env);

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(bytes.data()),
                 bytes.size());
    jpeg_save_markers(&cinfo, kICCMarker, 0xFFFF);
    jpeg_read_header(&cinfo, TRUE);
    if (ReadICCProfile(&cinfo, &icc)) {
      if (!color_encoding.SetICC(std::move(icc))) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return JXL_FAILURE("read an invalid ICC profile");
      }
    } else {
      color_encoding = ColorEncoding::SRGB(cinfo.output_components == 1);
    }
    io->metadata.SetUintSamples(BITS_IN_JSAMPLE);
    io->metadata.color_encoding = color_encoding;
    io->enc_size = bytes.size();
    int nbcomp = cinfo.num_components;
    if (nbcomp != 1 && nbcomp != 3) {
      jpeg_abort_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return JXL_FAILURE("unsupported number of components (%d) in JPEG",
                         cinfo.output_components);
    }
    (void)io->dec_hints.Foreach(
        [](const std::string& key, const std::string& /*value*/) {
          JXL_WARNING("JPEG decoder ignoring %s hint", key.c_str());
          return true;
        });

    jpeg_start_decompress(&cinfo);
    if (!io->VerifyDimensions(cinfo.image_width, cinfo.image_height)) {
      jpeg_abort_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return JXL_FAILURE("image too big");
    }
    JXL_ASSERT(cinfo.output_components == nbcomp);
    image = Image3F(cinfo.image_width, cinfo.image_height);
    row.reset(new JSAMPLE[cinfo.output_components * cinfo.image_width]);
    for (size_t y = 0; y < image.ysize(); ++y) {
      JSAMPROW rows[] = {row.get()};
      jpeg_read_scanlines(&cinfo, rows, 1);
#ifdef MEMORY_SANITIZER
      __msan_unpoison(row.get(), sizeof(JSAMPLE) * cinfo.output_components *
                                     cinfo.image_width);
#endif
      float* const JXL_RESTRICT output_row[] = {
          image.PlaneRow(0, y), image.PlaneRow(1, y), image.PlaneRow(2, y)};
      if (cinfo.output_components == 1) {
        for (size_t x = 0; x < image.xsize(); ++x) {
          output_row[0][x] = output_row[1][x] = output_row[2][x] =
              row[x] * (1.f / kJPEGSampleMultiplier);
        }
      } else {  // 3 components
        for (size_t x = 0; x < image.xsize(); ++x) {
          for (size_t c = 0; c < 3; ++c) {
            output_row[c][x] = row[3 * x + c] * (1.f / kJPEGSampleMultiplier);
          }
        }
      }
    }
    io->SetFromImage(std::move(image), color_encoding);
    if (!Map255ToTargetNits(io, pool)) {
      jpeg_abort_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return JXL_FAILURE("failed to map 255 to tatget nits");
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    io->dec_pixels = io->xsize() * io->ysize();
    return true;
  };

  return try_catch_block();
}

Status EncodeWithLibJpeg(const ImageBundle* ib, size_t quality,
                         YCbCrChromaSubsampling chroma_subsampling,
                         PaddedBytes* bytes) {
  jpeg_compress_struct cinfo;
#ifdef MEMORY_SANITIZER
  // cinfo is initialized by libjpeg, which we are not instrumenting with
  // msan.
  __msan_unpoison(&cinfo, sizeof(cinfo));
#endif
  jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  unsigned char* buffer = nullptr;
  unsigned long size = 0;
  jpeg_mem_dest(&cinfo, &buffer, &size);
  cinfo.image_width = ib->xsize();
  cinfo.image_height = ib->ysize();
  if (ib->IsGray()) {
    cinfo.input_components = 1;
    cinfo.in_color_space = JCS_GRAYSCALE;
  } else {
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
  }
  jpeg_set_defaults(&cinfo);
  cinfo.optimize_coding = TRUE;
  if (cinfo.input_components == 3) {
    JXL_RETURN_IF_ERROR(SetChromaSubsampling(chroma_subsampling, &cinfo));
  }
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  if (!ib->IsSRGB()) {
    WriteICCProfile(&cinfo, ib->c_current().ICC());
  }
  if (cinfo.input_components > 3)
    return JXL_FAILURE("invalid numbers of components");

  std::unique_ptr<JSAMPLE[]> row(
      new JSAMPLE[cinfo.input_components * cinfo.image_width]);
  for (size_t y = 0; y < ib->ysize(); ++y) {
    const float* const JXL_RESTRICT input_row[3] = {
        ib->color().ConstPlaneRow(0, y), ib->color().ConstPlaneRow(1, y),
        ib->color().ConstPlaneRow(2, y)};
    for (size_t x = 0; x < ib->xsize(); ++x) {
      for (size_t c = 0; c < cinfo.input_components; ++c) {
        JXL_RETURN_IF_ERROR(c < 3);
        row[cinfo.input_components * x + c] = static_cast<JSAMPLE>(
            std::max(std::min(kJPEGSampleMultiplier * input_row[c][x] + .5f,
                              kJPEGSampleMax),
                     kJPEGSampleMin));
      }
    }
    JSAMPROW rows[] = {row.get()};
    jpeg_write_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  bytes->resize(size);
#ifdef MEMORY_SANITIZER
  // Compressed image data is initialized by libjpeg, which we are not
  // instrumenting with msan.
  __msan_unpoison(buffer, size);
#endif
  std::copy_n(buffer, size, bytes->data());
  std::free(buffer);
  return true;
}

Status EncodeWithSJpeg(const ImageBundle* ib, size_t quality,
                       YCbCrChromaSubsampling chroma_subsampling,
                       PaddedBytes* bytes) {
#if !JPEGXL_ENABLE_SJPEG
  return JXL_FAILURE("JPEG XL was built without sjpeg support");
#else
  sjpeg::EncoderParam param(quality);
  if (!ib->IsSRGB()) {
    param.iccp.assign(ib->metadata()->color_encoding.ICC().begin(),
                      ib->metadata()->color_encoding.ICC().end());
  }
  if (chroma_subsampling.Is444()) {
    param.yuv_mode = SJPEG_YUV_444;
  } else if (chroma_subsampling.Is420()) {
    param.yuv_mode = SJPEG_YUV_SHARP;
  } else {
    return JXL_FAILURE("sjpeg does not support this chroma subsampling mode");
  }
  std::vector<uint8_t> rgb;
  rgb.reserve(ib->xsize() * ib->ysize() * 3);
  for (size_t y = 0; y < ib->ysize(); ++y) {
    const float* const rows[] = {
        ib->color().ConstPlaneRow(0, y),
        ib->color().ConstPlaneRow(1, y),
        ib->color().ConstPlaneRow(2, y),
    };
    for (size_t x = 0; x < ib->xsize(); ++x) {
      for (const float* const row : rows) {
        rgb.push_back(static_cast<uint8_t>(
            std::max(0.f, std::min(255.f, std::round(row[x])))));
      }
    }
  }
  std::string output;
  JXL_RETURN_IF_ERROR(sjpeg::Encode(rgb.data(), ib->xsize(), ib->ysize(),
                                    ib->xsize() * 3, param, &output));
  bytes->assign(
      reinterpret_cast<const uint8_t*>(output.data()),
      reinterpret_cast<const uint8_t*>(output.data() + output.size()));
  return true;
#endif
}

Status EncodeImageJPG(const CodecInOut* io, JpegEncoder encoder, size_t quality,
                      YCbCrChromaSubsampling chroma_subsampling,
                      ThreadPool* pool, PaddedBytes* bytes,
                      const DecodeTarget target) {
  if (io->Main().HasAlpha()) {
    return JXL_FAILURE("alpha is not supported");
  }
  if (quality < 0 || quality > 100) {
    return JXL_FAILURE("please specify a 0-100 JPEG quality");
  }

  if (target == DecodeTarget::kQuantizedCoeffs) {
    auto write = [](void* data, const uint8_t* buf, size_t len) {
      PaddedBytes* bytes = reinterpret_cast<PaddedBytes*>(data);
      bytes->append(buf, buf + len);
      return len;
    };
    brunsli::JPEGOutput out(static_cast<brunsli::JPEGOutputHook>(write),
                            reinterpret_cast<void*>(bytes));
    return brunsli::WriteJpeg(*io->Main().jpeg_data, out);
  }

  ImageBundle ib_0_255 = io->Main().Copy();
  JXL_RETURN_IF_ERROR(MapTargetNitsTo255(&ib_0_255, pool));
  const ImageBundle* ib;
  ImageMetadata metadata = io->metadata;
  ImageBundle ib_store(&metadata);
  JXL_RETURN_IF_ERROR(TransformIfNeeded(ib_0_255, io->metadata.color_encoding,
                                        pool, &ib_store, &ib));

  switch (encoder) {
    case JpegEncoder::kLibJpeg:
      JXL_RETURN_IF_ERROR(
          EncodeWithLibJpeg(ib, quality, chroma_subsampling, bytes));
      break;
    case JpegEncoder::kSJpeg:
      JXL_RETURN_IF_ERROR(
          EncodeWithSJpeg(ib, quality, chroma_subsampling, bytes));
      break;
    default:
      return JXL_FAILURE("tried to use an unknown JPEG encoder");
  }

  io->enc_size = bytes->size();
  return true;
}  // namespace jxl

}  // namespace jxl
