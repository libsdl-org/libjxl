#!/usr/bin/env bash

# Copyright (c) the JPEG XL Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

decoder="$(dirname "$0")"/TAppDecoderHighBitDepthStatic

usage() {
  echo "$0 [-v] <input.bin> <output.png>" >&2
  exit 1
}

verbose=0

while getopts ':hv' arg; do
  case "$arg" in
    h)
      usage
      ;;

    v)
      verbose=1
      ;;

    \?)
      echo "Unrecognized option -$OPTARG" >&2
      exit 1
      ;;
  esac
done
shift $((OPTIND-1))

if [ $# -lt 2 ]; then
  usage
fi

run() {
  if [ "$verbose" -eq 1 ]; then
    "$@"
  else
    "$@" > /dev/null 2>&1
  fi
}

input="$1"
output="$2"

bin="$(mktemp)"
yuv="$(mktemp)"
width_file="$(mktemp)"
height_file="$(mktemp)"

cleanup() {
  rm -- "$bin" "$yuv" "$width_file" "$height_file"
}
trap cleanup EXIT

unpack_program="$(cat <<'END'
  use File::Copy;
  my ($input, $bin, $width_file, $height_file) = @ARGV;
  open my $input_fh, '<:raw', $input;
  sysread($input_fh, my $size, 8) == 8 or die;
  my ($width, $height) = unpack 'NN', $size;
  open my $width_fh, '>', $width_file;
  print {$width_fh} "$width\n";
  open my $height_fh, '>', $height_file;
  print {$height_fh} "$height\n";
  copy $input_fh, $bin;
END
)"
run perl -Mstrict -Mwarnings -Mautodie -e "$unpack_program" -- "$input" "$bin" "$width_file" "$height_file"

width="$(cat "$width_file")"
height="$(cat "$height_file")"

start="$EPOCHREALTIME"
run "$decoder" --OutputBitDepth=10 -b "$bin" -o "$yuv"
end="$EPOCHREALTIME"

elapsed="$(echo "$end - $start" | bc)"
run echo "Completed in $elapsed seconds"

echo "$elapsed" > "${output%.png}".time

run ffmpeg -hide_banner -f rawvideo -vcodec rawvideo -s "${width}x$height" -r 25 -pix_fmt yuv444p10le -i "$yuv" -pix_fmt rgb24 -vf scale=in_color_matrix=bt709 -y "$output"
