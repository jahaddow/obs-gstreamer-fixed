#!/bin/bash
set -euo pipefail

GSTREAMER_VERSION="${1:?usage: $0 <gstreamer-version> [output-dir]}"
OUTPUT_DIR="${2:-.ci-cache/gstreamer}"
BASE_URL="https://gstreamer.freedesktop.org/data/pkg/windows/${GSTREAMER_VERSION}/mingw"

mkdir -p "$OUTPUT_DIR"

download() {
	local url="$1"
	local output="$2"

	if [ -s "$output" ]; then
		echo "Using cached $(basename "$output")"
		return
	fi

	echo "Downloading $(basename "$output")"
	temporary="${output}.part"
	rm -f "$temporary"
	curl --fail --location --retry 5 --retry-all-errors --retry-delay 5 \
		--connect-timeout 30 --max-time 1200 \
		-o "$temporary" "$url"
	mv "$temporary" "$output"
}

download \
	"$BASE_URL/gstreamer-1.0-mingw-x86_64-${GSTREAMER_VERSION}.msi" \
	"$OUTPUT_DIR/gstreamer-runtime.msi"
download \
	"$BASE_URL/gstreamer-1.0-devel-mingw-x86_64-${GSTREAMER_VERSION}.msi" \
	"$OUTPUT_DIR/gstreamer-devel.msi"
