// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/jxl/enc_ans.h"

#include <jxl/memory_manager.h>
#include <jxl/types.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lib/jxl/ans_common.h"
#include "lib/jxl/ans_params.h"
#include "lib/jxl/base/bits.h"
#include "lib/jxl/base/common.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/fast_math-inl.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_ans.h"
#include "lib/jxl/enc_ans_params.h"
#include "lib/jxl/enc_aux_out.h"
#include "lib/jxl/enc_cluster.h"
#include "lib/jxl/enc_context_map.h"
#include "lib/jxl/enc_fields.h"
#include "lib/jxl/enc_huffman.h"
#include "lib/jxl/enc_params.h"
#include "lib/jxl/fields.h"

namespace jxl {

namespace {

#if (!JXL_IS_DEBUG_BUILD)
constexpr
#endif
    bool ans_fuzzer_friendly_ = false;

const int kMaxNumSymbolsForSmallCode = 2;

void ANSBuildInfoTable(const ANSHistBin* counts, const AliasTable::Entry* table,
                       size_t alphabet_size, size_t log_alpha_size,
                       ANSEncSymbolInfo* info) {
  size_t log_entry_size = ANS_LOG_TAB_SIZE - log_alpha_size;
  size_t entry_size_minus_1 = (1 << log_entry_size) - 1;
  // create valid alias table for empty streams.
  for (size_t s = 0; s < std::max<size_t>(1, alphabet_size); ++s) {
    const ANSHistBin freq = s == alphabet_size ? ANS_TAB_SIZE : counts[s];
    info[s].freq_ = static_cast<uint16_t>(freq);
#ifdef USE_MULT_BY_RECIPROCAL
    if (freq != 0) {
      info[s].ifreq_ =
          ((1ull << RECIPROCAL_PRECISION) + info[s].freq_ - 1) / info[s].freq_;
    } else {
      info[s].ifreq_ = 1;  // shouldn't matter (symbol shouldn't occur), but...
    }
#endif
    info[s].reverse_map_.resize(freq);
  }
  for (int i = 0; i < ANS_TAB_SIZE; i++) {
    AliasTable::Symbol s =
        AliasTable::Lookup(table, i, log_entry_size, entry_size_minus_1);
    info[s.value].reverse_map_[s.offset] = i;
  }
}

// Static Huffman code for encoding logcounts. The last symbol is used as RLE
// sequence.
const uint8_t kLogCountBitLengths[ANS_LOG_TAB_SIZE + 2] = {
    5, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 6, 7, 7,
};
const uint8_t kLogCountSymbols[ANS_LOG_TAB_SIZE + 2] = {
    17, 11, 15, 3, 9, 7, 4, 2, 5, 6, 0, 33, 1, 65,
};

// Returns the difference between largest count that can be represented and is
// smaller than "count" and smallest representable count larger than "count".
uint32_t SmallestIncrementLog(uint32_t count, uint32_t shift) {
  if (count == 0) return 0;
  uint32_t bits = FloorLog2Nonzero(count);
  uint32_t drop_bits = bits - GetPopulationCountPrecision(bits, shift);
  return drop_bits;
}

// fixed-point log2 LUT
const auto lg2 = [] {
  std::array<uint32_t, ANS_TAB_SIZE + 1> lg2;
  lg2[0] = 0;  // for entropy calculations it is OK
  for (size_t i = 1; i < lg2.size(); ++i) {
    lg2[i] = round(ldexp(log2(i) / ANS_LOG_TAB_SIZE, 31));
  }
  return lg2;
}();

float EstimateDataBits(const ANSHistBin* histogram, const ANSHistBin* counts,
                       size_t len) {
  int64_t sum = 0;
  int total_histogram = 0;
  int total_counts = 0;
  for (size_t i = 0; i < len; ++i) {
    total_histogram += histogram[i];
    total_counts += counts[i];
    if (histogram[i] > 0) {
      JXL_DASSERT(counts[i] > 0);
    }
    sum += histogram[i] * int64_t{lg2[counts[i]]};
  }
  if (total_histogram > 0) {
    // Used only in assert.
    (void)total_counts;
    JXL_DASSERT(total_counts == ANS_TAB_SIZE);
  }
  return (total_histogram - ldexpf(sum, -31)) * ANS_LOG_TAB_SIZE;
}

float EstimateDataBitsFlat(const ANSHistBin* histogram, size_t len) {
  int64_t flat_bits = int64_t{lg2[len]} * ANS_LOG_TAB_SIZE;
  int total_histogram = 0;
  for (size_t i = 0; i < len; ++i) {
    total_histogram += histogram[i];
  }
  return ldexpf(total_histogram * flat_bits, -31);
}

struct CountsEntropy {
  ANSHistBin count : 16;    // allowed value of counts in a histogram bin
  ANSHistBin step_log : 5;  // log2 of increase step size
  int32_t delta_lg2;  // change of log between that value and the next allowed
};

// Array is sorted by decreasing allowed counts for each possible shift.
// Exclusion of single-bin histograms before `RebalanceHistogram` allows
// to put count upper limit of 4095, and shifts of 11 and 12 produce the
// same table
const auto allowed_counts = []() {
  std::array<std::array<CountsEntropy, ANS_TAB_SIZE>, ANS_LOG_TAB_SIZE>
      allowed_counts = {};

  for (uint32_t shift = 0; shift < allowed_counts.size(); ++shift) {
    auto& ac = allowed_counts[shift];
    for (uint32_t i = 1; i < allowed_counts[0].size(); ++i) {
      int32_t cnt = i & ~((1 << SmallestIncrementLog(i, shift)) - 1);
      ac[cnt].count = cnt;
    }
    std::sort(ac.begin(), ac.end(),
              [](const CountsEntropy& a, const CountsEntropy& b) {
                return a.count > b.count;
              });
    int ind = 1;
    while (ac[ind].count > 0) {
      ac[ind].delta_lg2 =
          round(log2(double(ac[ind - 1].count) / ac[ind].count) /
                ANS_LOG_TAB_SIZE * (int64_t{1} << 31));
      ac[ind].step_log =
          FloorLog2Nonzero<uint32_t>(ac[ind - 1].count - ac[ind].count);
      ++ind;
    }
    // Guards against non-possible steps:
    // at max value [0] - 0 (by init), at min value - max
    ac[ind].delta_lg2 = std::numeric_limits<int32_t>::max();
  }
  return allowed_counts;
}();

// We are growing/reducing histogram step by step trying to maximize total
// entropy i.e. sum of `freq[n] * log[counts[n]]` with a given sum of
// `counts[n]` chosen from `allowed_counts[shift]`. This sum is balanced by
// the `counts[omit_pos]` in the highest bin of histogram. We start from close
// to correct solution and each time a step with maximum entropy increase per
// unit of bin change is chosen. This greedy scheme is not guaranteed to
// achieve the global maximum, but cannot produce invalid histogram. We use a
// fixed-point approximation for logarithms and all arithmetic is integer
// besides initial approximation. Sum of `freq` and each of `lg2[counts]` are
// supposed to be limited to `int32_t` range, so that the sum of their products
// should not exceed `int64_t`.
bool RebalanceHistogram(ANSHistBin total, int max_symbol, uint32_t shift,
                        int* omit_pos, ANSHistBin* counts) {
  constexpr ANSHistBin table_size = ANS_TAB_SIZE;
  JXL_DASSERT(max_symbol > 0);
  JXL_DASSERT(max_symbol <= table_size);
  // `shift = 12` and `shift = 11` are the same
  shift = std::min(shift, ANS_LOG_TAB_SIZE - 1);

  struct EntropyDelta {
    ANSHistBin freq;  // initial count
    int count_ind;    // index of current bin value in `allowed_counts`
    int bin_ind;      // index of current bin in `counts`
  };
  // Penalties corresponding to different step sizes - entropy decrease in
  // balancing bin, step of size (1 << ANS_LOG_TAB_SIZE - 1) is not possible
  int64_t balance_inc[ANS_LOG_TAB_SIZE - 1] = {};
  int64_t balance_dec[ANS_LOG_TAB_SIZE - 1] = {};
  const auto& ac = allowed_counts[shift];
  // TODO(ivan) separate cases of shift >= 11 - all steps are 1 there, and
  // possibly 10 - all relevant steps are 2.
  // Total entropy change by a step: increase/decrease in current bin
  // together with corresponding decrease/increase in the balancing bin.
  // Inc steps increase current bin, dec steps decrease
  const auto delta_entropy_inc = [&](const EntropyDelta& a) {
    return a.freq * int64_t{ac[a.count_ind].delta_lg2} -
           balance_inc[ac[a.count_ind].step_log];
  };
  const auto delta_entropy_dec = [&](const EntropyDelta& a) {
    return a.freq * int64_t{ac[a.count_ind + 1].delta_lg2} -
           balance_dec[ac[a.count_ind + 1].step_log];
  };
  // Compare steps by entropy increase per unit of histogram bin change.
  // Truncation is OK here, accuracy is anyway better than float
  const auto IncLess = [&](const EntropyDelta& a, const EntropyDelta& b) {
    return delta_entropy_inc(a) >> ac[a.count_ind].step_log <
           delta_entropy_inc(b) >> ac[b.count_ind].step_log;
  };
  const auto DecLess = [&](const EntropyDelta& a, const EntropyDelta& b) {
    return delta_entropy_dec(a) >> ac[a.count_ind + 1].step_log <
           delta_entropy_dec(b) >> ac[b.count_ind + 1].step_log;
  };
  // Vector of adjustable bins from `allowed_counts`
  std::vector<EntropyDelta> bins;
  bins.reserve(256);

  double norm = double(table_size) / total;

  int remainder_pos = 0;  // highest balancing bin in the histogram
  int64_t max_freq = 0;
  ANSHistBin rest = table_size;  // reserve of histogram counts to distribute
  for (int n = 0; n < max_symbol; ++n) {
    ANSHistBin freq = counts[n];
    if (freq > max_freq) {
      remainder_pos = n;
      max_freq = freq;
    }

    double target = freq * norm;  // rounding
    // Keep zeros and clamp nonzero freq counts to [1, table_size)
    ANSHistBin count = std::max<ANSHistBin>(round(target), freq > 0);
    count = std::min<ANSHistBin>(count, table_size - 1);
    uint32_t step_log = SmallestIncrementLog(count, shift);
    ANSHistBin inc = 1 << step_log;
    count &= ~(inc - 1);

    counts[n] = count;
    rest -= count;
    if (target > 1.0) {
      int count_ind = 0;
      // TODO(ivan) binary search instead of linear?
      while (ac[count_ind].count != count) ++count_ind;
      bins.push_back({freq, count_ind, n});
    }
  }

  // Delete the highest balancing bin from adjustable by `allowed_counts`
  bins.erase(std::find_if(bins.begin(), bins.end(), [&](const EntropyDelta& a) {
    return a.bin_ind == remainder_pos;
  }));
  // From now on `rest` is the height of balancing bin,
  // here it can be negative, but will be tracted into positive domain later
  rest += counts[remainder_pos];

  if (!bins.empty()) {
    const uint32_t max_log = ac[1].step_log;
    while (true) {
      // Update balancing bin penalties setting guards and tractors
      for (uint32_t log = 0; log <= max_log; ++log) {
        ANSHistBin delta = 1 << log;
        if (rest >= table_size) {
          // Tract large `rest` into allowed domain:
          balance_inc[log] = 0;  // permit all inc steps
          balance_dec[log] = 0;  // forbid all dec steps
        } else if (rest > 1) {
          // `rest` is OK, put guards against non-possible steps
          balance_inc[log] =
              rest > delta  // possible step
                  ? max_freq * int64_t{lg2[rest] - lg2[rest - delta]}
                  : std::numeric_limits<int64_t>::max();  // forbidden
          balance_dec[log] =
              rest + delta < table_size  // possible step
                  ? max_freq * int64_t{lg2[rest + delta] - lg2[rest]}
                  : 0;  // forbidden
        } else {
          // Tract negative or zero `rest` into positive:
          // forbid all inc steps
          balance_inc[log] = std::numeric_limits<int64_t>::max();
          // permit all dec steps
          balance_dec[log] = std::numeric_limits<int64_t>::max();
        }
      }
      // Try to increase entropy
      auto best_bin_inc = std::max_element(bins.begin(), bins.end(), IncLess);
      if (delta_entropy_inc(*best_bin_inc) > 0) {
        // Grow the bin with the best histogram entropy increase
        rest -= 1 << ac[best_bin_inc->count_ind--].step_log;
      } else {
        // This still implies that entropy is strictly increasing each step
        // (or `rest` is tracted into positive domain), so we cannot loop
        // infinitely
        auto best_bin_dec = std::min_element(bins.begin(), bins.end(), DecLess);
        // Break if no reverse steps can grow entropy (or valid)
        if (delta_entropy_dec(*best_bin_dec) >= 0) break;
        // Decrease the bin with the best histogram entropy increase
        rest += 1 << ac[++best_bin_dec->count_ind].step_log;
      }
    }
    // Set counts besides the balancing bin
    for (auto& a : bins) counts[a.bin_ind] = ac[a.count_ind].count;

    // The scheme works fine if we have room to grow `logcount` of balancing
    // bin, otherwise we need to put balancing bin to the first bin of 12 bit
    // width. In this case both that bin and balancing one should be close to
    // 2048 in targets, so exchange of them will not produce much worse
    // histogram
    for (int n = 0; n < remainder_pos; ++n) {
      if (counts[n] >= 2048) {
        counts[remainder_pos] = counts[n];
        remainder_pos = n;
        break;
      }
    }
  }
  // Set balancing bin
  counts[remainder_pos] = rest;
  *omit_pos = remainder_pos;

  return counts[remainder_pos] > 0;
}

Status NormalizeCounts(ANSHistBin* counts, int* omit_pos, const int length,
                       uint32_t shift, int* num_symbols, int* symbols) {
  int max_symbol = 0;
  int symbol_count = 0;
  ANSHistBin total = 0;
  for (int n = 0; n < length; ++n) {
    if (counts[n] > 0) {
      if (symbol_count < kMaxNumSymbolsForSmallCode) {
        symbols[symbol_count] = n;
      }
      ++symbol_count;
      max_symbol = n + 1;
      total += counts[n];
    }
  }
  *num_symbols = symbol_count;
  if (symbol_count == 0) {
    return true;
  }
  if (symbol_count == 1) {
    counts[symbols[0]] = ANS_TAB_SIZE;
    return true;
  }
  if (symbol_count > ANS_TAB_SIZE || max_symbol > ANS_TAB_SIZE)
    return JXL_FAILURE("Too many entries in an ANS histogram");

  if (!RebalanceHistogram(total, max_symbol, shift, omit_pos, counts)) {
    return JXL_FAILURE("Logic error: couldn't rebalance a histogram");
  }
  return true;
}

struct SizeWriter {
  size_t size = 0;
  void Write(size_t num, size_t bits) { size += num; }
};

template <typename Writer>
void StoreVarLenUint8(size_t n, Writer* writer) {
  JXL_DASSERT(n <= 255);
  if (n == 0) {
    writer->Write(1, 0);
  } else {
    writer->Write(1, 1);
    size_t nbits = FloorLog2Nonzero(n);
    writer->Write(3, nbits);
    writer->Write(nbits, n - (1ULL << nbits));
  }
}

template <typename Writer>
void StoreVarLenUint16(size_t n, Writer* writer) {
  JXL_DASSERT(n <= 65535);
  if (n == 0) {
    writer->Write(1, 0);
  } else {
    writer->Write(1, 1);
    size_t nbits = FloorLog2Nonzero(n);
    writer->Write(4, nbits);
    writer->Write(nbits, n - (1ULL << nbits));
  }
}

template <typename Writer>
bool EncodeCounts(const ANSHistBin* counts, const int alphabet_size,
                  const int omit_pos, const int num_symbols, uint32_t shift,
                  const int* symbols, Writer* writer) {
  if (num_symbols <= 2) {
    // Small tree marker to encode 1-2 symbols.
    writer->Write(1, 1);
    if (num_symbols == 0) {
      writer->Write(1, 0);
      StoreVarLenUint8(0, writer);
    } else {
      writer->Write(1, num_symbols - 1);
      for (int i = 0; i < num_symbols; ++i) {
        StoreVarLenUint8(symbols[i], writer);
      }
    }
    if (num_symbols == 2) {
      writer->Write(ANS_LOG_TAB_SIZE, counts[symbols[0]]);
    }
  } else {
    // Mark non-small tree.
    writer->Write(1, 0);
    // Mark non-flat histogram.
    writer->Write(1, 0);

    // Elias gamma-like code for shift. Only difference is that if the number
    // of bits to be encoded is equal to FloorLog2(ANS_LOG_TAB_SIZE+1), we skip
    // the terminating 0 in unary coding.
    int upper_bound_log = FloorLog2Nonzero(ANS_LOG_TAB_SIZE + 1);
    int log = FloorLog2Nonzero(shift + 1);
    writer->Write(log, (1 << log) - 1);
    if (log != upper_bound_log) writer->Write(1, 0);
    writer->Write(log, ((1 << log) - 1) & (shift + 1));

    // Precompute sequences for RLE encoding. Contains the number of identical
    // values starting at a given index. Only contains the value at the first
    // element of the series.
    std::vector<uint32_t> same(alphabet_size, 0);
    int last = 0;
    int length = 0;
    // Store the sequence length once different symbol reached, or we are
    // near the omit_pos. We don't support including the omit_pos in an RLE
    // sequence because this value may use a different amount of log2 bits
    // than standard, it is too complex to handle in the decoder.
    for (int i = 1; i < alphabet_size; i++) {
      if (i == omit_pos || i == omit_pos + 1 || counts[i] != counts[last]) {
        same[last] = i - last;
        last = i;
      }
    }
    if (counts[last] != 0) {
      // Store the last sequence length if it is a non-zero sequence
      same[last] = alphabet_size - last;
      length = alphabet_size;
    } else {
      // else exclude last zero run
      length = last;
    }

    // Since `num_symbols >= 3`, we know that `length >= 3`, therefore we encode
    // `length - 3`. The check also ensures that all `same[i] <= 255` and can be
    // encoded further after RLE symbols by `StoreVarLenUint8`
    if (length - 3 > 255) {
      return false;
    }
    StoreVarLenUint8(length - 3, writer);

    std::vector<int> logcounts(length, -1);
    // Use shortest possible Huffman code to encode `omit_pos` (see
    // `kLogCountBitLengths`). `logcounts` value at `omit_pos` should be the
    // first of maximal values in the whole `logcounts` array, so it can be
    // increased without changing that property
    int omit_log = 9;
    for (int i = 0; i < length; ++i) {
      JXL_DASSERT(counts[i] <= ANS_TAB_SIZE);
      JXL_DASSERT(counts[i] >= 0);
      if (i != omit_pos && counts[i] > 0) {
        logcounts[i] = FloorLog2Nonzero(static_cast<uint32_t>(counts[i]));
        omit_log = std::max(omit_log, logcounts[i] + (i < omit_pos));
      }
    }
    logcounts[omit_pos] = omit_log;

    // The logcount values are encoded with a static Huffman code.
    constexpr size_t kMinReps = 4;
    constexpr size_t rep = ANS_LOG_TAB_SIZE + 1;
    for (int i = 0; i < length; ++i) {
      writer->Write(kLogCountBitLengths[logcounts[i] + 1],
                    kLogCountSymbols[logcounts[i] + 1]);
      if (same[i] > kMinReps) {
        // Encode the RLE symbol and skip the repeated ones.
        writer->Write(kLogCountBitLengths[rep], kLogCountSymbols[rep]);
        StoreVarLenUint8(same[i] - kMinReps - 1, writer);
        i += same[i] - 1;
      }
    }
    if (shift != 0) {  // otherwise `bitcount = 0`
      for (int i = 0; i < length; ++i) {
        if (logcounts[i] > 0 && i != omit_pos) {
          int bitcount = GetPopulationCountPrecision(logcounts[i], shift);
          int drop_bits = logcounts[i] - bitcount;
          JXL_DASSERT((counts[i] & ((1 << drop_bits) - 1)) == 0);
          writer->Write(bitcount, (counts[i] >> drop_bits) - (1 << bitcount));
        }
        if (same[i] > kMinReps) {
          // Skip symbols encoded by RLE.
          i += same[i] - 1;
        }
      }
    }
  }
  return true;
}

void EncodeFlatHistogram(const int alphabet_size, BitWriter* writer) {
  // Mark non-small tree.
  writer->Write(1, 0);
  // Mark uniform histogram.
  writer->Write(1, 1);
  JXL_DASSERT(alphabet_size > 0);
  // Encode alphabet size.
  StoreVarLenUint8(alphabet_size - 1, writer);
}

StatusOr<float> ComputeHistoAndDataCost(const ANSHistBin* histogram,
                                        size_t alphabet_size, uint32_t method) {
  // TODO(ivan) check possible SIMD
  while (alphabet_size > 0 && histogram[alphabet_size - 1] == 0)
    --alphabet_size;
  if (method == 0) {  // Flat code
    return ANS_LOG_TAB_SIZE + 2 +
           EstimateDataBitsFlat(histogram, alphabet_size);
  }
  // Non-flat: shift = method-1.
  uint32_t shift = method - 1;
  std::vector<ANSHistBin> counts(histogram, histogram + alphabet_size);
  int omit_pos = 0;
  int num_symbols;
  int symbols[kMaxNumSymbolsForSmallCode] = {};
  JXL_RETURN_IF_ERROR(NormalizeCounts(counts.data(), &omit_pos, alphabet_size,
                                      shift, &num_symbols, symbols));
  SizeWriter writer;
  if (EncodeCounts(counts.data(), alphabet_size, omit_pos, num_symbols, shift,
                   symbols, &writer)) {
    return writer.size +
           EstimateDataBits(histogram, counts.data(), alphabet_size);
  } else {  // not possible to encode
    return std::numeric_limits<float>::max();
  }
}

StatusOr<uint32_t> ComputeBestMethod(
    const ANSHistBin* histogram, size_t alphabet_size, float* cost,
    HistogramParams::ANSHistogramStrategy ans_histogram_strategy) {
  uint32_t method = 0;
  JXL_ASSIGN_OR_RETURN(float fcost,
                       ComputeHistoAndDataCost(histogram, alphabet_size, 0));
  auto try_shift = [&](uint32_t shift) -> Status {
    JXL_ASSIGN_OR_RETURN(
        float c, ComputeHistoAndDataCost(histogram, alphabet_size, shift + 1));
    if (c < fcost) {
      method = shift + 1;
      fcost = c;
    }
    return true;
  };
  switch (ans_histogram_strategy) {
    case HistogramParams::ANSHistogramStrategy::kPrecise: {
      for (uint32_t shift = 0; shift < ANS_LOG_TAB_SIZE; shift++) {
        JXL_RETURN_IF_ERROR(try_shift(shift));
      }
      break;
    }
    case HistogramParams::ANSHistogramStrategy::kApproximate: {
      for (uint32_t shift = 0; shift <= ANS_LOG_TAB_SIZE; shift += 2) {
        JXL_RETURN_IF_ERROR(try_shift(shift));
      }
      break;
    }
    case HistogramParams::ANSHistogramStrategy::kFast: {
      JXL_RETURN_IF_ERROR(try_shift(0));
      JXL_RETURN_IF_ERROR(try_shift(ANS_LOG_TAB_SIZE / 2));
      JXL_RETURN_IF_ERROR(try_shift(ANS_LOG_TAB_SIZE));
      break;
    }
  };
  *cost = fcost;
  return method;
}

}  // namespace

// Returns an estimate of the cost of encoding this histogram and the
// corresponding data.
StatusOr<size_t> BuildAndStoreANSEncodingData(
    JxlMemoryManager* memory_manager,
    HistogramParams::ANSHistogramStrategy ans_histogram_strategy,
    const ANSHistBin* histogram, size_t alphabet_size, size_t log_alpha_size,
    bool use_prefix_code, ANSEncSymbolInfo* info, BitWriter* writer) {
  if (use_prefix_code) {
    size_t cost = 0;
    if (alphabet_size <= 1) return 0;
    std::vector<uint32_t> histo(alphabet_size);
    for (size_t i = 0; i < alphabet_size; i++) {
      histo[i] = histogram[i];
      JXL_ENSURE(histogram[i] >= 0);
    }
    {
      std::vector<uint8_t> depths(alphabet_size);
      std::vector<uint16_t> bits(alphabet_size);
      if (writer == nullptr) {
        BitWriter tmp_writer{memory_manager};
        JXL_RETURN_IF_ERROR(tmp_writer.WithMaxBits(
            8 * alphabet_size + 8,  // safe upper bound
            LayerType::Header, /*aux_out=*/nullptr, [&] {
              return BuildAndStoreHuffmanTree(histo.data(), alphabet_size,
                                              depths.data(), bits.data(),
                                              &tmp_writer);
            }));
        cost = tmp_writer.BitsWritten();
      } else {
        size_t start = writer->BitsWritten();
        JXL_RETURN_IF_ERROR(BuildAndStoreHuffmanTree(
            histo.data(), alphabet_size, depths.data(), bits.data(), writer));
        cost = writer->BitsWritten() - start;
      }
      for (size_t i = 0; i < alphabet_size; i++) {
        info[i].bits = depths[i] == 0 ? 0 : bits[i];
        info[i].depth = depths[i];
      }
    }
    // Estimate data cost.
    for (size_t i = 0; i < alphabet_size; i++) {
      cost += histogram[i] * info[i].depth;
    }
    return cost;
  }
  JXL_ENSURE(alphabet_size <= ANS_TAB_SIZE);
  float fcost;
  JXL_ASSIGN_OR_RETURN(uint32_t method,
                       ComputeBestMethod(histogram, alphabet_size, &fcost,
                                         ans_histogram_strategy));
  JXL_ENSURE(fcost >= 0);
  int num_symbols;
  int symbols[kMaxNumSymbolsForSmallCode] = {};
  std::vector<ANSHistBin> counts(histogram, histogram + alphabet_size);
  if (!counts.empty()) {
    size_t sum = 0;
    for (int count : counts) {
      sum += count;
    }
    if (sum == 0) {
      counts[0] = ANS_TAB_SIZE;
    }
  }
  int omit_pos = 0;
  uint32_t shift = method - 1;
  if (method == 0) {
    JXL_ENSURE(alphabet_size > 0);
    counts = CreateFlatHistogram(alphabet_size, ANS_TAB_SIZE);
  } else {
    JXL_RETURN_IF_ERROR(NormalizeCounts(counts.data(), &omit_pos, alphabet_size,
                                        shift, &num_symbols, symbols));
  }
  AliasTable::Entry a[ANS_MAX_ALPHABET_SIZE];
  JXL_RETURN_IF_ERROR(
      InitAliasTable(counts, ANS_LOG_TAB_SIZE, log_alpha_size, a));
  ANSBuildInfoTable(counts.data(), a, alphabet_size, log_alpha_size, info);
  if (writer != nullptr) {
    if (method == 0) {
      JXL_ENSURE(alphabet_size > 0);
      EncodeFlatHistogram(alphabet_size, writer);
    } else {
      if (!EncodeCounts(counts.data(), alphabet_size, omit_pos, num_symbols,
                        method - 1, symbols, writer)) {
        return JXL_FAILURE("EncodeCounts failed");
      }
    }
  }
  return static_cast<size_t>(fcost);
}

StatusOr<float> ANSPopulationCost(const ANSHistBin* data,
                                  size_t alphabet_size) {
  float cost = 0.0f;
  if (ANS_MAX_ALPHABET_SIZE < alphabet_size) {
    return std::numeric_limits<float>::max();
  }
  JXL_ASSIGN_OR_RETURN(
      uint32_t method,
      ComputeBestMethod(data, alphabet_size, &cost,
                        HistogramParams::ANSHistogramStrategy::kFast));
  (void)method;
  return cost;
}

template <typename Writer>
void EncodeUintConfig(const HybridUintConfig uint_config, Writer* writer,
                      size_t log_alpha_size) {
  writer->Write(CeilLog2Nonzero(log_alpha_size + 1),
                uint_config.split_exponent);
  if (uint_config.split_exponent == log_alpha_size) {
    return;  // msb/lsb don't matter.
  }
  size_t nbits = CeilLog2Nonzero(uint_config.split_exponent + 1);
  writer->Write(nbits, uint_config.msb_in_token);
  nbits = CeilLog2Nonzero(uint_config.split_exponent -
                          uint_config.msb_in_token + 1);
  writer->Write(nbits, uint_config.lsb_in_token);
}
template <typename Writer>
void EncodeUintConfigs(const std::vector<HybridUintConfig>& uint_config,
                       Writer* writer, size_t log_alpha_size) {
  // TODO(veluca): RLE?
  for (const auto& cfg : uint_config) {
    EncodeUintConfig(cfg, writer, log_alpha_size);
  }
}
template void EncodeUintConfigs(const std::vector<HybridUintConfig>&,
                                BitWriter*, size_t);

namespace {

Status ChooseUintConfigs(const HistogramParams& params,
                         const std::vector<std::vector<Token>>& tokens,
                         const std::vector<uint8_t>& context_map,
                         std::vector<Histogram>* clustered_histograms,
                         EntropyEncodingData* codes, size_t* log_alpha_size) {
  codes->uint_config.resize(clustered_histograms->size());
  if (params.uint_method == HistogramParams::HybridUintMethod::kNone) {
    return true;
  }
  if (params.uint_method == HistogramParams::HybridUintMethod::k000) {
    codes->uint_config.clear();
    codes->uint_config.resize(clustered_histograms->size(),
                              HybridUintConfig(0, 0, 0));
    return true;
  }
  if (params.uint_method == HistogramParams::HybridUintMethod::kContextMap) {
    codes->uint_config.clear();
    codes->uint_config.resize(clustered_histograms->size(),
                              HybridUintConfig(2, 0, 1));
    return true;
  }

  // If the uint config is adaptive, just stick with the default in streaming
  // mode.
  if (params.streaming_mode) {
    return true;
  }

  // Brute-force method that tries a few options.
  std::vector<HybridUintConfig> configs;
  if (params.uint_method == HistogramParams::HybridUintMethod::kBest) {
    configs = {
        HybridUintConfig(4, 2, 0),  // default
        HybridUintConfig(4, 1, 0),  // less precise
        HybridUintConfig(4, 2, 1),  // add sign
        HybridUintConfig(4, 2, 2),  // add sign+parity
        HybridUintConfig(4, 1, 2),  // add parity but less msb
        // Same as above, but more direct coding.
        HybridUintConfig(5, 2, 0), HybridUintConfig(5, 1, 0),
        HybridUintConfig(5, 2, 1), HybridUintConfig(5, 2, 2),
        HybridUintConfig(5, 1, 2),
        // Same as above, but less direct coding.
        HybridUintConfig(3, 2, 0), HybridUintConfig(3, 1, 0),
        HybridUintConfig(3, 2, 1), HybridUintConfig(3, 1, 2),
        // For near-lossless.
        HybridUintConfig(4, 1, 3), HybridUintConfig(5, 1, 4),
        HybridUintConfig(5, 2, 3), HybridUintConfig(6, 1, 5),
        HybridUintConfig(6, 2, 4), HybridUintConfig(6, 0, 0),
        // Other
        HybridUintConfig(0, 0, 0),   // varlenuint
        HybridUintConfig(2, 0, 1),   // works well for ctx map
        HybridUintConfig(7, 0, 0),   // direct coding
        HybridUintConfig(8, 0, 0),   // direct coding
        HybridUintConfig(9, 0, 0),   // direct coding
        HybridUintConfig(10, 0, 0),  // direct coding
        HybridUintConfig(11, 0, 0),  // direct coding
        HybridUintConfig(12, 0, 0),  // direct coding
    };
  } else if (params.uint_method == HistogramParams::HybridUintMethod::kFast) {
    configs = {
        HybridUintConfig(4, 2, 0),  // default
        HybridUintConfig(4, 1, 2),  // add parity but less msb
        HybridUintConfig(0, 0, 0),  // smallest histograms
        HybridUintConfig(2, 0, 1),  // works well for ctx map
    };
  }

  std::vector<float> costs(clustered_histograms->size(),
                           std::numeric_limits<float>::max());
  std::vector<uint32_t> extra_bits(clustered_histograms->size());
  std::vector<uint8_t> is_valid(clustered_histograms->size());
  // Wider histograms are assigned max cost in PopulationCost anyway
  // and therefore will not be used
  constexpr size_t max_alpha = ANS_MAX_ALPHABET_SIZE;
  for (HybridUintConfig cfg : configs) {
    std::fill(is_valid.begin(), is_valid.end(), true);
    std::fill(extra_bits.begin(), extra_bits.end(), 0);

    for (auto& histo : *clustered_histograms) {
      histo.Clear();
    }
    for (const auto& stream : tokens) {
      for (const auto& token : stream) {
        // TODO(veluca): do not ignore lz77 commands.
        if (token.is_lz77_length) continue;
        size_t histo = context_map[token.context];
        uint32_t tok, nbits, bits;
        cfg.Encode(token.value, &tok, &nbits, &bits);
        if (tok >= max_alpha ||
            (codes->lz77.enabled && tok >= codes->lz77.min_symbol)) {
          is_valid[histo] = JXL_FALSE;
          continue;
        }
        extra_bits[histo] += nbits;
        (*clustered_histograms)[histo].Add(tok);
      }
    }

    for (size_t i = 0; i < clustered_histograms->size(); i++) {
      if (!is_valid[i]) continue;
      JXL_ASSIGN_OR_RETURN(float cost,
                           (*clustered_histograms)[i].PopulationCost());
      cost += extra_bits[i];
      // add signaling cost of the hybriduintconfig itself
      cost += CeilLog2Nonzero(cfg.split_exponent + 1);
      cost += CeilLog2Nonzero(cfg.split_exponent - cfg.msb_in_token + 1);
      if (cost < costs[i]) {
        codes->uint_config[i] = cfg;
        costs[i] = cost;
      }
    }
  }

  // Rebuild histograms.
  for (auto& histo : *clustered_histograms) {
    histo.Clear();
  }
  *log_alpha_size = 5;
  for (const auto& stream : tokens) {
    for (const auto& token : stream) {
      uint32_t tok, nbits, bits;
      size_t histo = context_map[token.context];
      (token.is_lz77_length ? codes->lz77.length_uint_config
                            : codes->uint_config[histo])
          .Encode(token.value, &tok, &nbits, &bits);
      tok += token.is_lz77_length ? codes->lz77.min_symbol : 0;
      (*clustered_histograms)[histo].Add(tok);
      while (tok >= (1u << *log_alpha_size)) (*log_alpha_size)++;
    }
  }
  size_t max_log_alpha_size = codes->use_prefix_code ? PREFIX_MAX_BITS : 8;
  JXL_ENSURE(*log_alpha_size <= max_log_alpha_size);
  return true;
}

Histogram HistogramFromSymbolInfo(
    const std::vector<ANSEncSymbolInfo>& encoding_info, bool use_prefix_code) {
  Histogram histo;
  histo.data_.resize(DivCeil(encoding_info.size(), Histogram::kRounding) *
                     Histogram::kRounding);
  histo.total_count_ = 0;
  for (size_t i = 0; i < encoding_info.size(); ++i) {
    const ANSEncSymbolInfo& info = encoding_info[i];
    int count = use_prefix_code
                    ? (info.depth ? (1u << (PREFIX_MAX_BITS - info.depth)) : 0)
                    : info.freq_;
    histo.data_[i] = count;
    histo.total_count_ += count;
  }
  return histo;
}

class HistogramBuilder {
 public:
  explicit HistogramBuilder(const size_t num_contexts)
      : histograms_(num_contexts) {}

  void VisitSymbol(int symbol, size_t histo_idx) {
    JXL_DASSERT(histo_idx < histograms_.size());
    histograms_[histo_idx].Add(symbol);
  }

  // NOTE: `layer` is only for clustered_entropy; caller does ReclaimAndCharge.
  // Returns cost (in bits).
  StatusOr<size_t> BuildAndStoreEntropyCodes(
      JxlMemoryManager* memory_manager, const HistogramParams& params,
      const std::vector<std::vector<Token>>& tokens, EntropyEncodingData* codes,
      std::vector<uint8_t>* context_map, BitWriter* writer, LayerType layer,
      AuxOut* aux_out) const {
    const size_t prev_histograms = codes->encoding_info.size();
    std::vector<Histogram> clustered_histograms;
    for (size_t i = 0; i < prev_histograms; ++i) {
      clustered_histograms.push_back(HistogramFromSymbolInfo(
          codes->encoding_info[i], codes->use_prefix_code));
    }
    size_t context_offset = context_map->size();
    context_map->resize(context_offset + histograms_.size());
    if (histograms_.size() > 1) {
      if (!ans_fuzzer_friendly_) {
        std::vector<uint32_t> histogram_symbols;
        JXL_RETURN_IF_ERROR(
            ClusterHistograms(params, histograms_, kClustersLimit,
                              &clustered_histograms, &histogram_symbols));
        for (size_t c = 0; c < histograms_.size(); ++c) {
          (*context_map)[context_offset + c] =
              static_cast<uint8_t>(histogram_symbols[c]);
        }
      } else {
        JXL_ENSURE(codes->encoding_info.empty());
        fill(context_map->begin(), context_map->end(), 0);
        size_t max_symbol = 0;
        for (const Histogram& h : histograms_) {
          max_symbol = std::max(h.data_.size(), max_symbol);
        }
        size_t num_symbols = 1 << CeilLog2Nonzero(max_symbol + 1);
        clustered_histograms.resize(1);
        clustered_histograms[0].Clear();
        for (size_t i = 0; i < num_symbols; i++) {
          clustered_histograms[0].Add(i);
        }
      }
      if (writer != nullptr) {
        JXL_RETURN_IF_ERROR(EncodeContextMap(
            *context_map, clustered_histograms.size(), writer, layer, aux_out));
      }
    } else {
      JXL_ENSURE(codes->encoding_info.empty());
      clustered_histograms.push_back(histograms_[0]);
    }
    if (aux_out != nullptr) {
      for (size_t i = prev_histograms; i < clustered_histograms.size(); ++i) {
        aux_out->layer(layer).clustered_entropy +=
            clustered_histograms[i].ShannonEntropy();
      }
    }
    size_t log_alpha_size = codes->lz77.enabled ? 8 : 7;  // Sane default.
    if (ans_fuzzer_friendly_) {
      codes->uint_config.clear();
      codes->uint_config.resize(1, HybridUintConfig(7, 0, 0));
    } else {
      JXL_RETURN_IF_ERROR(ChooseUintConfigs(params, tokens, *context_map,
                                            &clustered_histograms, codes,
                                            &log_alpha_size));
    }
    if (params.streaming_mode) {
      // TODO(szabadka) Figure out if we can use lower values here.
      log_alpha_size = 8;
    }
    SizeWriter size_writer;  // Used if writer == nullptr to estimate costs.
    size_t cost = 1;
    if (writer) writer->Write(1, TO_JXL_BOOL(codes->use_prefix_code));

    if (codes->use_prefix_code) {
      log_alpha_size = PREFIX_MAX_BITS;
    } else {
      cost += 2;
    }
    if (writer == nullptr) {
      EncodeUintConfigs(codes->uint_config, &size_writer, log_alpha_size);
    } else {
      if (!codes->use_prefix_code) writer->Write(2, log_alpha_size - 5);
      EncodeUintConfigs(codes->uint_config, writer, log_alpha_size);
    }
    if (codes->use_prefix_code) {
      for (const auto& histo : clustered_histograms) {
        size_t alphabet_size = histo.alphabet_size();
        if (writer) {
          StoreVarLenUint16(alphabet_size - 1, writer);
        } else {
          StoreVarLenUint16(alphabet_size - 1, &size_writer);
        }
      }
    }
    cost += size_writer.size;
    for (size_t c = prev_histograms; c < clustered_histograms.size(); ++c) {
      size_t alphabet_size = clustered_histograms[c].alphabet_size();
      codes->encoding_info.emplace_back();
      codes->encoding_info.back().resize(alphabet_size);
      BitWriter* histo_writer = writer;
      if (params.streaming_mode) {
        codes->encoded_histograms.emplace_back(memory_manager);
        histo_writer = &codes->encoded_histograms.back();
      }
      const auto& body = [&]() -> Status {
        JXL_ASSIGN_OR_RETURN(
            size_t ans_cost,
            BuildAndStoreANSEncodingData(
                memory_manager, params.ans_histogram_strategy,
                clustered_histograms[c].data_.data(), alphabet_size,
                log_alpha_size, codes->use_prefix_code,
                codes->encoding_info.back().data(), histo_writer));
        cost += ans_cost;
        return true;
      };
      if (histo_writer) {
        JXL_RETURN_IF_ERROR(histo_writer->WithMaxBits(
            256 + alphabet_size * 24, layer, aux_out, body,
            /*finished_histogram=*/true));
      } else {
        JXL_RETURN_IF_ERROR(body());
      }
      if (params.streaming_mode) {
        JXL_RETURN_IF_ERROR(writer->AppendUnaligned(*histo_writer));
      }
    }
    return cost;
  }

  const Histogram& Histo(size_t i) const { return histograms_[i]; }

 private:
  std::vector<Histogram> histograms_;
};

class SymbolCostEstimator {
 public:
  SymbolCostEstimator(size_t num_contexts, bool force_huffman,
                      const std::vector<std::vector<Token>>& tokens,
                      const LZ77Params& lz77) {
    HistogramBuilder builder(num_contexts);
    // Build histograms for estimating lz77 savings.
    HybridUintConfig uint_config;
    for (const auto& stream : tokens) {
      for (const auto& token : stream) {
        uint32_t tok, nbits, bits;
        (token.is_lz77_length ? lz77.length_uint_config : uint_config)
            .Encode(token.value, &tok, &nbits, &bits);
        tok += token.is_lz77_length ? lz77.min_symbol : 0;
        builder.VisitSymbol(tok, token.context);
      }
    }
    max_alphabet_size_ = 0;
    for (size_t i = 0; i < num_contexts; i++) {
      max_alphabet_size_ =
          std::max(max_alphabet_size_, builder.Histo(i).data_.size());
    }
    bits_.resize(num_contexts * max_alphabet_size_);
    // TODO(veluca): SIMD?
    add_symbol_cost_.resize(num_contexts);
    for (size_t i = 0; i < num_contexts; i++) {
      float inv_total = 1.0f / (builder.Histo(i).total_count_ + 1e-8f);
      float total_cost = 0;
      for (size_t j = 0; j < builder.Histo(i).data_.size(); j++) {
        size_t cnt = builder.Histo(i).data_[j];
        float cost = 0;
        if (cnt != 0 && cnt != builder.Histo(i).total_count_) {
          cost = -FastLog2f(cnt * inv_total);
          if (force_huffman) cost = std::ceil(cost);
        } else if (cnt == 0) {
          cost = ANS_LOG_TAB_SIZE;  // Highest possible cost.
        }
        bits_[i * max_alphabet_size_ + j] = cost;
        total_cost += cost * builder.Histo(i).data_[j];
      }
      // Penalty for adding a lz77 symbol to this contest (only used for static
      // cost model). Higher penalty for contexts that have a very low
      // per-symbol entropy.
      add_symbol_cost_[i] = std::max(0.0f, 6.0f - total_cost * inv_total);
    }
  }
  float Bits(size_t ctx, size_t sym) const {
    return bits_[ctx * max_alphabet_size_ + sym];
  }
  float LenCost(size_t ctx, size_t len, const LZ77Params& lz77) const {
    uint32_t nbits, bits, tok;
    lz77.length_uint_config.Encode(len, &tok, &nbits, &bits);
    tok += lz77.min_symbol;
    return nbits + Bits(ctx, tok);
  }
  float DistCost(size_t len, const LZ77Params& lz77) const {
    uint32_t nbits, bits, tok;
    HybridUintConfig().Encode(len, &tok, &nbits, &bits);
    return nbits + Bits(lz77.nonserialized_distance_context, tok);
  }
  float AddSymbolCost(size_t idx) const { return add_symbol_cost_[idx]; }

 private:
  size_t max_alphabet_size_;
  std::vector<float> bits_;
  std::vector<float> add_symbol_cost_;
};

void ApplyLZ77_RLE(const HistogramParams& params, size_t num_contexts,
                   const std::vector<std::vector<Token>>& tokens,
                   LZ77Params& lz77,
                   std::vector<std::vector<Token>>& tokens_lz77) {
  // TODO(veluca): tune heuristics here.
  SymbolCostEstimator sce(num_contexts, params.force_huffman, tokens, lz77);
  float bit_decrease = 0;
  size_t total_symbols = 0;
  tokens_lz77.resize(tokens.size());
  std::vector<float> sym_cost;
  HybridUintConfig uint_config;
  for (size_t stream = 0; stream < tokens.size(); stream++) {
    size_t distance_multiplier =
        params.image_widths.size() > stream ? params.image_widths[stream] : 0;
    const auto& in = tokens[stream];
    auto& out = tokens_lz77[stream];
    total_symbols += in.size();
    // Cumulative sum of bit costs.
    sym_cost.resize(in.size() + 1);
    for (size_t i = 0; i < in.size(); i++) {
      uint32_t tok, nbits, unused_bits;
      uint_config.Encode(in[i].value, &tok, &nbits, &unused_bits);
      sym_cost[i + 1] = sce.Bits(in[i].context, tok) + nbits + sym_cost[i];
    }
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
      size_t num_to_copy = 0;
      size_t distance_symbol = 0;  // 1 for RLE.
      if (distance_multiplier != 0) {
        distance_symbol = 1;  // Special distance 1 if enabled.
        JXL_DASSERT(kSpecialDistances[1][0] == 1);
        JXL_DASSERT(kSpecialDistances[1][1] == 0);
      }
      if (i > 0) {
        for (; i + num_to_copy < in.size(); num_to_copy++) {
          if (in[i + num_to_copy].value != in[i - 1].value) {
            break;
          }
        }
      }
      if (num_to_copy == 0) {
        out.push_back(in[i]);
        continue;
      }
      float cost = sym_cost[i + num_to_copy] - sym_cost[i];
      // This subtraction might overflow, but that's OK.
      size_t lz77_len = num_to_copy - lz77.min_length;
      float lz77_cost = num_to_copy >= lz77.min_length
                            ? CeilLog2Nonzero(lz77_len + 1) + 1
                            : 0;
      if (num_to_copy < lz77.min_length || cost <= lz77_cost) {
        for (size_t j = 0; j < num_to_copy; j++) {
          out.push_back(in[i + j]);
        }
        i += num_to_copy - 1;
        continue;
      }
      // Output the LZ77 length
      out.emplace_back(in[i].context, lz77_len);
      out.back().is_lz77_length = true;
      i += num_to_copy - 1;
      bit_decrease += cost - lz77_cost;
      // Output the LZ77 copy distance.
      out.emplace_back(lz77.nonserialized_distance_context, distance_symbol);
    }
  }

  if (bit_decrease > total_symbols * 0.2 + 16) {
    lz77.enabled = true;
  }
}

// Hash chain for LZ77 matching
struct HashChain {
  size_t size_;
  std::vector<uint32_t> data_;

  unsigned hash_num_values_ = 32768;
  unsigned hash_mask_ = hash_num_values_ - 1;
  unsigned hash_shift_ = 5;

  std::vector<int> head;
  std::vector<uint32_t> chain;
  std::vector<int> val;

  // Speed up repetitions of zero
  std::vector<int> headz;
  std::vector<uint32_t> chainz;
  std::vector<uint32_t> zeros;
  uint32_t numzeros = 0;

  size_t window_size_;
  size_t window_mask_;
  size_t min_length_;
  size_t max_length_;

  // Map of special distance codes.
  std::unordered_map<int, int> special_dist_table_;
  size_t num_special_distances_ = 0;

  uint32_t maxchainlength = 256;  // window_size_ to allow all

  HashChain(const Token* data, size_t size, size_t window_size,
            size_t min_length, size_t max_length, size_t distance_multiplier)
      : size_(size),
        window_size_(window_size),
        window_mask_(window_size - 1),
        min_length_(min_length),
        max_length_(max_length) {
    data_.resize(size);
    for (size_t i = 0; i < size; i++) {
      data_[i] = data[i].value;
    }

    head.resize(hash_num_values_, -1);
    val.resize(window_size_, -1);
    chain.resize(window_size_);
    for (uint32_t i = 0; i < window_size_; ++i) {
      chain[i] = i;  // same value as index indicates uninitialized
    }

    zeros.resize(window_size_);
    headz.resize(window_size_ + 1, -1);
    chainz.resize(window_size_);
    for (uint32_t i = 0; i < window_size_; ++i) {
      chainz[i] = i;
    }
    // Translate distance to special distance code.
    if (distance_multiplier) {
      // Count down, so if due to small distance multiplier multiple distances
      // map to the same code, the smallest code will be used in the end.
      for (int i = kNumSpecialDistances - 1; i >= 0; --i) {
        special_dist_table_[SpecialDistance(i, distance_multiplier)] = i;
      }
      num_special_distances_ = kNumSpecialDistances;
    }
  }

  uint32_t GetHash(size_t pos) const {
    uint32_t result = 0;
    if (pos + 2 < size_) {
      // TODO(lode): take the MSB's of the uint32_t values into account as well,
      // given that the hash code itself is less than 32 bits.
      result ^= static_cast<uint32_t>(data_[pos + 0] << 0u);
      result ^= static_cast<uint32_t>(data_[pos + 1] << hash_shift_);
      result ^= static_cast<uint32_t>(data_[pos + 2] << (hash_shift_ * 2));
    } else {
      // No need to compute hash of last 2 bytes, the length 2 is too short.
      return 0;
    }
    return result & hash_mask_;
  }

  uint32_t CountZeros(size_t pos, uint32_t prevzeros) const {
    size_t end = pos + window_size_;
    if (end > size_) end = size_;
    if (prevzeros > 0) {
      if (prevzeros >= window_mask_ && data_[end - 1] == 0 &&
          end == pos + window_size_) {
        return prevzeros;
      } else {
        return prevzeros - 1;
      }
    }
    uint32_t num = 0;
    while (pos + num < end && data_[pos + num] == 0) num++;
    return num;
  }

  void Update(size_t pos) {
    uint32_t hashval = GetHash(pos);
    uint32_t wpos = pos & window_mask_;

    val[wpos] = static_cast<int>(hashval);
    if (head[hashval] != -1) chain[wpos] = head[hashval];
    head[hashval] = wpos;

    if (pos > 0 && data_[pos] != data_[pos - 1]) numzeros = 0;
    numzeros = CountZeros(pos, numzeros);

    zeros[wpos] = numzeros;
    if (headz[numzeros] != -1) chainz[wpos] = headz[numzeros];
    headz[numzeros] = wpos;
  }

  void Update(size_t pos, size_t len) {
    for (size_t i = 0; i < len; i++) {
      Update(pos + i);
    }
  }

  template <typename CB>
  void FindMatches(size_t pos, int max_dist, const CB& found_match) const {
    uint32_t wpos = pos & window_mask_;
    uint32_t hashval = GetHash(pos);
    uint32_t hashpos = chain[wpos];

    int prev_dist = 0;
    int end = std::min<int>(pos + max_length_, size_);
    uint32_t chainlength = 0;
    uint32_t best_len = 0;
    for (;;) {
      int dist = (hashpos <= wpos) ? (wpos - hashpos)
                                   : (wpos - hashpos + window_mask_ + 1);
      if (dist < prev_dist) break;
      prev_dist = dist;
      uint32_t len = 0;
      if (dist > 0) {
        int i = pos;
        int j = pos - dist;
        if (numzeros > 3) {
          int r = std::min<int>(numzeros - 1, zeros[hashpos]);
          if (i + r >= end) r = end - i - 1;
          i += r;
          j += r;
        }
        while (i < end && data_[i] == data_[j]) {
          i++;
          j++;
        }
        len = i - pos;
        // This can trigger even if the new length is slightly smaller than the
        // best length, because it is possible for a slightly cheaper distance
        // symbol to occur.
        if (len >= min_length_ && len + 2 >= best_len) {
          auto it = special_dist_table_.find(dist);
          int dist_symbol = (it == special_dist_table_.end())
                                ? (num_special_distances_ + dist - 1)
                                : it->second;
          found_match(len, dist_symbol);
          if (len > best_len) best_len = len;
        }
      }

      chainlength++;
      if (chainlength >= maxchainlength) break;

      if (numzeros >= 3 && len > numzeros) {
        if (hashpos == chainz[hashpos]) break;
        hashpos = chainz[hashpos];
        if (zeros[hashpos] != numzeros) break;
      } else {
        if (hashpos == chain[hashpos]) break;
        hashpos = chain[hashpos];
        if (val[hashpos] != static_cast<int>(hashval)) {
          // outdated hash value
          break;
        }
      }
    }
  }
  void FindMatch(size_t pos, int max_dist, size_t* result_dist_symbol,
                 size_t* result_len) const {
    *result_dist_symbol = 0;
    *result_len = 1;
    FindMatches(pos, max_dist, [&](size_t len, size_t dist_symbol) {
      if (len > *result_len ||
          (len == *result_len && *result_dist_symbol > dist_symbol)) {
        *result_len = len;
        *result_dist_symbol = dist_symbol;
      }
    });
  }
};

float LenCost(size_t len) {
  uint32_t nbits, bits, tok;
  HybridUintConfig(1, 0, 0).Encode(len, &tok, &nbits, &bits);
  constexpr float kCostTable[] = {
      2.797667318563126,  3.213177690381199,  2.5706009246743737,
      2.408392498667534,  2.829649191872326,  3.3923087753324577,
      4.029267451554331,  4.415576699706408,  4.509357574741465,
      9.21481543803004,   10.020590190114898, 11.858671627804766,
      12.45853300490526,  11.713105831990857, 12.561996324849314,
      13.775477692278367, 13.174027068768641,
  };
  size_t table_size = sizeof kCostTable / sizeof *kCostTable;
  if (tok >= table_size) tok = table_size - 1;
  return kCostTable[tok] + nbits;
}

// TODO(veluca): this does not take into account usage or non-usage of distance
// multipliers.
float DistCost(size_t dist) {
  uint32_t nbits, bits, tok;
  HybridUintConfig(7, 0, 0).Encode(dist, &tok, &nbits, &bits);
  constexpr float kCostTable[] = {
      6.368282626312716,  5.680793277090298,  8.347404197105247,
      7.641619201599141,  6.914328374119438,  7.959808291537444,
      8.70023120759855,   8.71378518934703,   9.379132523982769,
      9.110472749092708,  9.159029569270908,  9.430936766731973,
      7.278284055315169,  7.8278514904267755, 10.026641158289236,
      9.976049229827066,  9.64351607048908,   9.563403863480442,
      10.171474111762747, 10.45950155077234,  9.994813912104219,
      10.322524683741156, 8.465808729388186,  8.756254166066853,
      10.160930174662234, 10.247329273413435, 10.04090403724809,
      10.129398517544082, 9.342311691539546,  9.07608009102374,
      10.104799540677513, 10.378079384990906, 10.165828974075072,
      10.337595322341553, 7.940557464567944,  10.575665823319431,
      11.023344321751955, 10.736144698831827, 11.118277044595054,
      7.468468230648442,  10.738305230932939, 10.906980780216568,
      10.163468216353817, 10.17805759656433,  11.167283670483565,
      11.147050200274544, 10.517921919244333, 10.651764778156886,
      10.17074446448919,  11.217636876224745, 11.261630721139484,
      11.403140815247259, 10.892472096873417, 11.1859607804481,
      8.017346947551262,  7.895143720278828,  11.036577113822025,
      11.170562110315794, 10.326988722591086, 10.40872184751056,
      11.213498225466386, 11.30580635516863,  10.672272515665442,
      10.768069466228063, 11.145257364153565, 11.64668307145549,
      10.593156194627339, 11.207499484844943, 10.767517766396908,
      10.826629811407042, 10.737764794499988, 10.6200448518045,
      10.191315385198092, 8.468384171390085,  11.731295299170432,
      11.824619886654398, 10.41518844301179,  10.16310536548649,
      10.539423685097576, 10.495136599328031, 10.469112847728267,
      11.72057686174922,  10.910326337834674, 11.378921834673758,
      11.847759036098536, 11.92071647623854,  10.810628276345282,
      11.008601085273893, 11.910326337834674, 11.949212023423133,
      11.298614839104337, 11.611603659010392, 10.472930394619985,
      11.835564720850282, 11.523267392285337, 12.01055816679611,
      8.413029688994023,  11.895784139536406, 11.984679534970505,
      11.220654278717394, 11.716311684833672, 10.61036646226114,
      10.89849965960364,  10.203762898863669, 10.997560826267238,
      11.484217379438984, 11.792836176993665, 12.24310468755171,
      11.464858097919262, 12.212747017409377, 11.425595666074955,
      11.572048533398757, 12.742093965163013, 11.381874288645637,
      12.191870445817015, 11.683156920035426, 11.152442115262197,
      11.90303691580457,  11.653292787169159, 11.938615382266098,
      16.970641701570223, 16.853602280380002, 17.26240782594733,
      16.644655390108507, 17.14310889757499,  16.910935455445955,
      17.505678976959697, 17.213498225466388, 2.4162310293553024,
      3.494587244462329,  3.5258600986408344, 3.4959806589517095,
      3.098390886949687,  3.343454654302911,  3.588847442290287,
      4.14614790111827,   5.152948641990529,  7.433696808092598,
      9.716311684833672,
  };
  size_t table_size = sizeof kCostTable / sizeof *kCostTable;
  if (tok >= table_size) tok = table_size - 1;
  return kCostTable[tok] + nbits;
}

void ApplyLZ77_LZ77(const HistogramParams& params, size_t num_contexts,
                    const std::vector<std::vector<Token>>& tokens,
                    LZ77Params& lz77,
                    std::vector<std::vector<Token>>& tokens_lz77) {
  // TODO(veluca): tune heuristics here.
  SymbolCostEstimator sce(num_contexts, params.force_huffman, tokens, lz77);
  float bit_decrease = 0;
  size_t total_symbols = 0;
  tokens_lz77.resize(tokens.size());
  HybridUintConfig uint_config;
  std::vector<float> sym_cost;
  for (size_t stream = 0; stream < tokens.size(); stream++) {
    size_t distance_multiplier =
        params.image_widths.size() > stream ? params.image_widths[stream] : 0;
    const auto& in = tokens[stream];
    auto& out = tokens_lz77[stream];
    total_symbols += in.size();
    // Cumulative sum of bit costs.
    sym_cost.resize(in.size() + 1);
    for (size_t i = 0; i < in.size(); i++) {
      uint32_t tok, nbits, unused_bits;
      uint_config.Encode(in[i].value, &tok, &nbits, &unused_bits);
      sym_cost[i + 1] = sce.Bits(in[i].context, tok) + nbits + sym_cost[i];
    }

    out.reserve(in.size());
    size_t max_distance = in.size();
    size_t min_length = lz77.min_length;
    JXL_DASSERT(min_length >= 3);
    size_t max_length = in.size();

    // Use next power of two as window size.
    size_t window_size = 1;
    while (window_size < max_distance && window_size < kWindowSize) {
      window_size <<= 1;
    }

    HashChain chain(in.data(), in.size(), window_size, min_length, max_length,
                    distance_multiplier);
    size_t len;
    size_t dist_symbol;

    const size_t max_lazy_match_len = 256;  // 0 to disable lazy matching

    // Whether the next symbol was already updated (to test lazy matching)
    bool already_updated = false;
    for (size_t i = 0; i < in.size(); i++) {
      out.push_back(in[i]);
      if (!already_updated) chain.Update(i);
      already_updated = false;
      chain.FindMatch(i, max_distance, &dist_symbol, &len);
      if (len >= min_length) {
        if (len < max_lazy_match_len && i + 1 < in.size()) {
          // Try length at next symbol lazy matching
          chain.Update(i + 1);
          already_updated = true;
          size_t len2, dist_symbol2;
          chain.FindMatch(i + 1, max_distance, &dist_symbol2, &len2);
          if (len2 > len) {
            // Use the lazy match. Add literal, and use the next length starting
            // from the next byte.
            ++i;
            already_updated = false;
            len = len2;
            dist_symbol = dist_symbol2;
            out.push_back(in[i]);
          }
        }

        float cost = sym_cost[i + len] - sym_cost[i];
        size_t lz77_len = len - lz77.min_length;
        float lz77_cost = LenCost(lz77_len) + DistCost(dist_symbol) +
                          sce.AddSymbolCost(out.back().context);

        if (lz77_cost <= cost) {
          out.back().value = len - min_length;
          out.back().is_lz77_length = true;
          out.emplace_back(lz77.nonserialized_distance_context, dist_symbol);
          bit_decrease += cost - lz77_cost;
        } else {
          // LZ77 match ignored, and symbol already pushed. Push all other
          // symbols and skip.
          for (size_t j = 1; j < len; j++) {
            out.push_back(in[i + j]);
          }
        }

        if (already_updated) {
          chain.Update(i + 2, len - 2);
          already_updated = false;
        } else {
          chain.Update(i + 1, len - 1);
        }
        i += len - 1;
      } else {
        // Literal, already pushed
      }
    }
  }

  if (bit_decrease > total_symbols * 0.2 + 16) {
    lz77.enabled = true;
  }
}

void ApplyLZ77_Optimal(const HistogramParams& params, size_t num_contexts,
                       const std::vector<std::vector<Token>>& tokens,
                       LZ77Params& lz77,
                       std::vector<std::vector<Token>>& tokens_lz77) {
  std::vector<std::vector<Token>> tokens_for_cost_estimate;
  ApplyLZ77_LZ77(params, num_contexts, tokens, lz77, tokens_for_cost_estimate);
  // If greedy-LZ77 does not give better compression than no-lz77, no reason to
  // run the optimal matching.
  if (!lz77.enabled) return;
  SymbolCostEstimator sce(num_contexts + 1, params.force_huffman,
                          tokens_for_cost_estimate, lz77);
  tokens_lz77.resize(tokens.size());
  HybridUintConfig uint_config;
  std::vector<float> sym_cost;
  std::vector<uint32_t> dist_symbols;
  for (size_t stream = 0; stream < tokens.size(); stream++) {
    size_t distance_multiplier =
        params.image_widths.size() > stream ? params.image_widths[stream] : 0;
    const auto& in = tokens[stream];
    auto& out = tokens_lz77[stream];
    // Cumulative sum of bit costs.
    sym_cost.resize(in.size() + 1);
    for (size_t i = 0; i < in.size(); i++) {
      uint32_t tok, nbits, unused_bits;
      uint_config.Encode(in[i].value, &tok, &nbits, &unused_bits);
      sym_cost[i + 1] = sce.Bits(in[i].context, tok) + nbits + sym_cost[i];
    }

    out.reserve(in.size());
    size_t max_distance = in.size();
    size_t min_length = lz77.min_length;
    JXL_DASSERT(min_length >= 3);
    size_t max_length = in.size();

    // Use next power of two as window size.
    size_t window_size = 1;
    while (window_size < max_distance && window_size < kWindowSize) {
      window_size <<= 1;
    }

    HashChain chain(in.data(), in.size(), window_size, min_length, max_length,
                    distance_multiplier);

    struct MatchInfo {
      uint32_t len;
      uint32_t dist_symbol;
      uint32_t ctx;
      float total_cost = std::numeric_limits<float>::max();
    };
    // Total cost to encode the first N symbols.
    std::vector<MatchInfo> prefix_costs(in.size() + 1);
    prefix_costs[0].total_cost = 0;

    size_t rle_length = 0;
    size_t skip_lz77 = 0;
    for (size_t i = 0; i < in.size(); i++) {
      chain.Update(i);
      float lit_cost =
          prefix_costs[i].total_cost + sym_cost[i + 1] - sym_cost[i];
      if (prefix_costs[i + 1].total_cost > lit_cost) {
        prefix_costs[i + 1].dist_symbol = 0;
        prefix_costs[i + 1].len = 1;
        prefix_costs[i + 1].ctx = in[i].context;
        prefix_costs[i + 1].total_cost = lit_cost;
      }
      if (skip_lz77 > 0) {
        skip_lz77--;
        continue;
      }
      dist_symbols.clear();
      chain.FindMatches(i, max_distance,
                        [&dist_symbols](size_t len, size_t dist_symbol) {
                          if (dist_symbols.size() <= len) {
                            dist_symbols.resize(len + 1, dist_symbol);
                          }
                          if (dist_symbol < dist_symbols[len]) {
                            dist_symbols[len] = dist_symbol;
                          }
                        });
      if (dist_symbols.size() <= min_length) continue;
      {
        size_t best_cost = dist_symbols.back();
        for (size_t j = dist_symbols.size() - 1; j >= min_length; j--) {
          if (dist_symbols[j] < best_cost) {
            best_cost = dist_symbols[j];
          }
          dist_symbols[j] = best_cost;
        }
      }
      for (size_t j = min_length; j < dist_symbols.size(); j++) {
        // Cost model that uses results from lazy LZ77.
        float lz77_cost = sce.LenCost(in[i].context, j - min_length, lz77) +
                          sce.DistCost(dist_symbols[j], lz77);
        float cost = prefix_costs[i].total_cost + lz77_cost;
        if (prefix_costs[i + j].total_cost > cost) {
          prefix_costs[i + j].len = j;
          prefix_costs[i + j].dist_symbol = dist_symbols[j] + 1;
          prefix_costs[i + j].ctx = in[i].context;
          prefix_costs[i + j].total_cost = cost;
        }
      }
      // We are in a RLE sequence: skip all the symbols except the first 8 and
      // the last 8. This avoid quadratic costs for sequences with long runs of
      // the same symbol.
      if ((dist_symbols.back() == 0 && distance_multiplier == 0) ||
          (dist_symbols.back() == 1 && distance_multiplier != 0)) {
        rle_length++;
      } else {
        rle_length = 0;
      }
      if (rle_length >= 8 && dist_symbols.size() > 9) {
        skip_lz77 = dist_symbols.size() - 10;
        rle_length = 0;
      }
    }
    size_t pos = in.size();
    while (pos > 0) {
      bool is_lz77_length = prefix_costs[pos].dist_symbol != 0;
      if (is_lz77_length) {
        size_t dist_symbol = prefix_costs[pos].dist_symbol - 1;
        out.emplace_back(lz77.nonserialized_distance_context, dist_symbol);
      }
      size_t val = is_lz77_length ? prefix_costs[pos].len - min_length
                                  : in[pos - 1].value;
      out.emplace_back(prefix_costs[pos].ctx, val);
      out.back().is_lz77_length = is_lz77_length;
      pos -= prefix_costs[pos].len;
    }
    std::reverse(out.begin(), out.end());
  }
}

void ApplyLZ77(const HistogramParams& params, size_t num_contexts,
               const std::vector<std::vector<Token>>& tokens, LZ77Params& lz77,
               std::vector<std::vector<Token>>& tokens_lz77) {
  if (params.initialize_global_state) {
    lz77.enabled = false;
  }
  if (params.force_huffman) {
    lz77.min_symbol = std::min(PREFIX_MAX_ALPHABET_SIZE - 32, 512);
  } else {
    lz77.min_symbol = 224;
  }
  switch (params.lz77_method) {
    case HistogramParams::LZ77Method::kNone:
      return;
    case HistogramParams::LZ77Method::kRLE:
      ApplyLZ77_RLE(params, num_contexts, tokens, lz77, tokens_lz77);
      return;
    case HistogramParams::LZ77Method::kLZ77:
      ApplyLZ77_LZ77(params, num_contexts, tokens, lz77, tokens_lz77);
      return;
    case HistogramParams::LZ77Method::kOptimal:
      ApplyLZ77_Optimal(params, num_contexts, tokens, lz77, tokens_lz77);
      return;
  }
}
}  // namespace

Status EncodeHistograms(const std::vector<uint8_t>& context_map,
                        const EntropyEncodingData& codes, BitWriter* writer,
                        LayerType layer, AuxOut* aux_out) {
  return writer->WithMaxBits(
      128 + kClustersLimit * 136, layer, aux_out,
      [&]() -> Status {
        JXL_RETURN_IF_ERROR(Bundle::Write(codes.lz77, writer, layer, aux_out));
        if (codes.lz77.enabled) {
          EncodeUintConfig(codes.lz77.length_uint_config, writer,
                           /*log_alpha_size=*/8);
        }
        JXL_RETURN_IF_ERROR(EncodeContextMap(
            context_map, codes.encoding_info.size(), writer, layer, aux_out));
        writer->Write(1, TO_JXL_BOOL(codes.use_prefix_code));
        size_t log_alpha_size = 8;
        if (codes.use_prefix_code) {
          log_alpha_size = PREFIX_MAX_BITS;
        } else {
          log_alpha_size = 8;  // streaming_mode
          writer->Write(2, log_alpha_size - 5);
        }
        EncodeUintConfigs(codes.uint_config, writer, log_alpha_size);
        if (codes.use_prefix_code) {
          for (const auto& info : codes.encoding_info) {
            StoreVarLenUint16(info.size() - 1, writer);
          }
        }
        for (const auto& histo_writer : codes.encoded_histograms) {
          JXL_RETURN_IF_ERROR(writer->AppendUnaligned(histo_writer));
        }
        return true;
      },
      /*finished_histogram=*/true);
}

StatusOr<size_t> BuildAndEncodeHistograms(
    JxlMemoryManager* memory_manager, const HistogramParams& params,
    size_t num_contexts, std::vector<std::vector<Token>>& tokens,
    EntropyEncodingData* codes, std::vector<uint8_t>* context_map,
    BitWriter* writer, LayerType layer, AuxOut* aux_out) {
  size_t cost = 0;
  codes->lz77.nonserialized_distance_context = num_contexts;
  std::vector<std::vector<Token>> tokens_lz77;
  ApplyLZ77(params, num_contexts, tokens, codes->lz77, tokens_lz77);
  if (ans_fuzzer_friendly_) {
    codes->lz77.length_uint_config = HybridUintConfig(10, 0, 0);
    codes->lz77.min_symbol = 2048;
  }

  const size_t max_contexts = std::min(num_contexts, kClustersLimit);
  const auto& body = [&]() -> Status {
    if (writer) {
      JXL_RETURN_IF_ERROR(Bundle::Write(codes->lz77, writer, layer, aux_out));
    } else {
      size_t ebits, bits;
      JXL_RETURN_IF_ERROR(Bundle::CanEncode(codes->lz77, &ebits, &bits));
      cost += bits;
    }
    if (codes->lz77.enabled) {
      if (writer) {
        size_t b = writer->BitsWritten();
        EncodeUintConfig(codes->lz77.length_uint_config, writer,
                         /*log_alpha_size=*/8);
        cost += writer->BitsWritten() - b;
      } else {
        SizeWriter size_writer;
        EncodeUintConfig(codes->lz77.length_uint_config, &size_writer,
                         /*log_alpha_size=*/8);
        cost += size_writer.size;
      }
      num_contexts += 1;
      tokens = std::move(tokens_lz77);
    }
    size_t total_tokens = 0;
    // Build histograms.
    HistogramBuilder builder(num_contexts);
    HybridUintConfig uint_config;  //  Default config for clustering.
    // Unless we are using the kContextMap histogram option.
    if (params.uint_method == HistogramParams::HybridUintMethod::kContextMap) {
      uint_config = HybridUintConfig(2, 0, 1);
    }
    if (params.uint_method == HistogramParams::HybridUintMethod::k000) {
      uint_config = HybridUintConfig(0, 0, 0);
    }
    if (ans_fuzzer_friendly_) {
      uint_config = HybridUintConfig(10, 0, 0);
    }
    for (const auto& stream : tokens) {
      if (codes->lz77.enabled) {
        for (const auto& token : stream) {
          total_tokens++;
          uint32_t tok, nbits, bits;
          (token.is_lz77_length ? codes->lz77.length_uint_config : uint_config)
              .Encode(token.value, &tok, &nbits, &bits);
          tok += token.is_lz77_length ? codes->lz77.min_symbol : 0;
          builder.VisitSymbol(tok, token.context);
        }
      } else if (num_contexts == 1) {
        for (const auto& token : stream) {
          total_tokens++;
          uint32_t tok, nbits, bits;
          uint_config.Encode(token.value, &tok, &nbits, &bits);
          builder.VisitSymbol(tok, /*token.context=*/0);
        }
      } else {
        for (const auto& token : stream) {
          total_tokens++;
          uint32_t tok, nbits, bits;
          uint_config.Encode(token.value, &tok, &nbits, &bits);
          builder.VisitSymbol(tok, token.context);
        }
      }
    }

    if (params.add_missing_symbols) {
      for (size_t c = 0; c < num_contexts; ++c) {
        for (int symbol = 0; symbol < ANS_MAX_ALPHABET_SIZE; ++symbol) {
          builder.VisitSymbol(symbol, c);
        }
      }
    }

    if (params.initialize_global_state) {
      bool use_prefix_code =
          params.force_huffman || total_tokens < 100 ||
          params.clustering == HistogramParams::ClusteringType::kFastest ||
          ans_fuzzer_friendly_;
      if (!use_prefix_code) {
        bool all_singleton = true;
        for (size_t i = 0; i < num_contexts; i++) {
          if (builder.Histo(i).ShannonEntropy() >= 1e-5) {
            all_singleton = false;
          }
        }
        if (all_singleton) {
          use_prefix_code = true;
        }
      }
      codes->use_prefix_code = use_prefix_code;
    }

    if (params.add_fixed_histograms) {
      // TODO(szabadka) Add more fixed histograms.
      // TODO(szabadka) Reduce alphabet size by choosing a non-default
      // uint_config.
      const size_t alphabet_size = ANS_MAX_ALPHABET_SIZE;
      const size_t log_alpha_size = 8;
      JXL_ENSURE(alphabet_size == 1u << log_alpha_size);
      static_assert(ANS_MAX_ALPHABET_SIZE <= ANS_TAB_SIZE,
                    "Alphabet does not fit table");
      std::vector<int32_t> counts =
          CreateFlatHistogram(alphabet_size, ANS_TAB_SIZE);
      codes->encoding_info.emplace_back();
      codes->encoding_info.back().resize(alphabet_size);
      codes->encoded_histograms.emplace_back(memory_manager);
      BitWriter* histo_writer = &codes->encoded_histograms.back();
      JXL_RETURN_IF_ERROR(histo_writer->WithMaxBits(
          256 + alphabet_size * 24, LayerType::Header, nullptr,
          [&]() -> Status {
            JXL_ASSIGN_OR_RETURN(
                size_t ans_cost,
                BuildAndStoreANSEncodingData(
                    memory_manager, params.ans_histogram_strategy,
                    counts.data(), alphabet_size, log_alpha_size,
                    codes->use_prefix_code, codes->encoding_info.back().data(),
                    histo_writer));
            (void)ans_cost;
            return true;
          }));
    }

    // Encode histograms.
    JXL_ASSIGN_OR_RETURN(
        size_t entropy_bits,
        builder.BuildAndStoreEntropyCodes(memory_manager, params, tokens, codes,
                                          context_map, writer, layer, aux_out));
    cost += entropy_bits;
    return true;
  };
  if (writer) {
    JXL_RETURN_IF_ERROR(writer->WithMaxBits(
        128 + num_contexts * 40 + max_contexts * 96, layer, aux_out, body,
        /*finished_histogram=*/true));
  } else {
    JXL_RETURN_IF_ERROR(body());
  }

  if (aux_out != nullptr) {
    aux_out->layer(layer).num_clustered_histograms +=
        codes->encoding_info.size();
  }
  return cost;
}

size_t WriteTokens(const std::vector<Token>& tokens,
                   const EntropyEncodingData& codes,
                   const std::vector<uint8_t>& context_map,
                   size_t context_offset, BitWriter* writer) {
  size_t num_extra_bits = 0;
  if (codes.use_prefix_code) {
    for (const auto& token : tokens) {
      uint32_t tok, nbits, bits;
      size_t histo = context_map[context_offset + token.context];
      (token.is_lz77_length ? codes.lz77.length_uint_config
                            : codes.uint_config[histo])
          .Encode(token.value, &tok, &nbits, &bits);
      tok += token.is_lz77_length ? codes.lz77.min_symbol : 0;
      // Combine two calls to the BitWriter. Equivalent to:
      // writer->Write(codes.encoding_info[histo][tok].depth,
      //               codes.encoding_info[histo][tok].bits);
      // writer->Write(nbits, bits);
      uint64_t data = codes.encoding_info[histo][tok].bits;
      data |= static_cast<uint64_t>(bits)
              << codes.encoding_info[histo][tok].depth;
      writer->Write(codes.encoding_info[histo][tok].depth + nbits, data);
      num_extra_bits += nbits;
    }
    return num_extra_bits;
  }
  std::vector<uint64_t> out;
  std::vector<uint8_t> out_nbits;
  out.reserve(tokens.size());
  out_nbits.reserve(tokens.size());
  uint64_t allbits = 0;
  size_t numallbits = 0;
  // Writes in *reversed* order.
  auto addbits = [&](size_t bits, size_t nbits) {
    if (JXL_UNLIKELY(nbits)) {
      JXL_DASSERT(bits >> nbits == 0);
      if (JXL_UNLIKELY(numallbits + nbits > BitWriter::kMaxBitsPerCall)) {
        out.push_back(allbits);
        out_nbits.push_back(numallbits);
        numallbits = allbits = 0;
      }
      allbits <<= nbits;
      allbits |= bits;
      numallbits += nbits;
    }
  };
  const int end = tokens.size();
  ANSCoder ans;
  if (codes.lz77.enabled || context_map.size() > 1) {
    for (int i = end - 1; i >= 0; --i) {
      const Token token = tokens[i];
      const uint8_t histo = context_map[context_offset + token.context];
      uint32_t tok, nbits, bits;
      (token.is_lz77_length ? codes.lz77.length_uint_config
                            : codes.uint_config[histo])
          .Encode(tokens[i].value, &tok, &nbits, &bits);
      tok += token.is_lz77_length ? codes.lz77.min_symbol : 0;
      const ANSEncSymbolInfo& info = codes.encoding_info[histo][tok];
      JXL_DASSERT(info.freq_ > 0);
      // Extra bits first as this is reversed.
      addbits(bits, nbits);
      num_extra_bits += nbits;
      uint8_t ans_nbits = 0;
      uint32_t ans_bits = ans.PutSymbol(info, &ans_nbits);
      addbits(ans_bits, ans_nbits);
    }
  } else {
    for (int i = end - 1; i >= 0; --i) {
      uint32_t tok, nbits, bits;
      codes.uint_config[0].Encode(tokens[i].value, &tok, &nbits, &bits);
      const ANSEncSymbolInfo& info = codes.encoding_info[0][tok];
      // Extra bits first as this is reversed.
      addbits(bits, nbits);
      num_extra_bits += nbits;
      uint8_t ans_nbits = 0;
      uint32_t ans_bits = ans.PutSymbol(info, &ans_nbits);
      addbits(ans_bits, ans_nbits);
    }
  }
  const uint32_t state = ans.GetState();
  writer->Write(32, state);
  writer->Write(numallbits, allbits);
  for (int i = out.size(); i > 0; --i) {
    writer->Write(out_nbits[i - 1], out[i - 1]);
  }
  return num_extra_bits;
}

Status WriteTokens(const std::vector<Token>& tokens,
                   const EntropyEncodingData& codes,
                   const std::vector<uint8_t>& context_map,
                   size_t context_offset, BitWriter* writer, LayerType layer,
                   AuxOut* aux_out) {
  // Theoretically, we could have 15 prefix code bits + 31 extra bits.
  return writer->WithMaxBits(
      46 * tokens.size() + 32 * 1024 * 4, layer, aux_out, [&] {
        size_t num_extra_bits =
            WriteTokens(tokens, codes, context_map, context_offset, writer);
        if (aux_out != nullptr) {
          aux_out->layer(layer).extra_bits += num_extra_bits;
        }
        return true;
      });
}

void SetANSFuzzerFriendly(bool ans_fuzzer_friendly) {
#if JXL_IS_DEBUG_BUILD  // Guard against accidental / malicious changes.
  ans_fuzzer_friendly_ = ans_fuzzer_friendly;
#endif
}

HistogramParams HistogramParams::ForModular(
    const CompressParams& cparams,
    const std::vector<uint8_t>& extra_dc_precision, bool streaming_mode) {
  HistogramParams params;
  params.streaming_mode = streaming_mode;
  if (cparams.speed_tier > SpeedTier::kKitten) {
    params.clustering = HistogramParams::ClusteringType::kFast;
    params.ans_histogram_strategy =
        cparams.speed_tier > SpeedTier::kThunder
            ? HistogramParams::ANSHistogramStrategy::kFast
            : HistogramParams::ANSHistogramStrategy::kApproximate;
    params.lz77_method =
        cparams.decoding_speed_tier >= 3 && cparams.modular_mode
            ? (cparams.speed_tier >= SpeedTier::kFalcon
                   ? HistogramParams::LZ77Method::kRLE
                   : HistogramParams::LZ77Method::kLZ77)
            : HistogramParams::LZ77Method::kNone;
    // Near-lossless DC, as well as modular mode, require choosing hybrid uint
    // more carefully.
    if ((!extra_dc_precision.empty() && extra_dc_precision[0] != 0) ||
        (cparams.modular_mode && cparams.speed_tier < SpeedTier::kCheetah)) {
      params.uint_method = HistogramParams::HybridUintMethod::kFast;
    } else {
      params.uint_method = HistogramParams::HybridUintMethod::kNone;
    }
  } else if (cparams.speed_tier <= SpeedTier::kTortoise) {
    params.lz77_method = HistogramParams::LZ77Method::kOptimal;
  } else {
    params.lz77_method = HistogramParams::LZ77Method::kLZ77;
  }
  if (cparams.decoding_speed_tier >= 1) {
    params.max_histograms = 12;
  }
  if (cparams.decoding_speed_tier >= 1 && cparams.responsive) {
    params.lz77_method = cparams.speed_tier >= SpeedTier::kCheetah
                             ? HistogramParams::LZ77Method::kRLE
                         : cparams.speed_tier >= SpeedTier::kKitten
                             ? HistogramParams::LZ77Method::kLZ77
                             : HistogramParams::LZ77Method::kOptimal;
  }
  if (cparams.decoding_speed_tier >= 2 && cparams.responsive) {
    params.uint_method = HistogramParams::HybridUintMethod::k000;
    params.force_huffman = true;
  }
  return params;
}
}  // namespace jxl
