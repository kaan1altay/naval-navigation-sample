#!/usr/bin/env bash
# Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.
#
# Compiles and runs the engine-free navigation core against a minimal Unreal type shim, so the
# algorithm can be verified without an Unreal installation. See Shim/CoreMinimal.h for what
# this is and is not.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT_DIR="${SCRIPT_DIR}/Intermediate"
CXX="${CXX:-c++}"

mkdir -p "${OUT_DIR}"

"${CXX}" -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter \
	-I "${SCRIPT_DIR}/Shim" \
	-I "${REPO_ROOT}/Source/NavalNav" \
	"${SCRIPT_DIR}/AlgoSelfTest.cpp" \
	"${REPO_ROOT}/Source/NavalNav/Grid/SeaGridTypes.cpp" \
	"${REPO_ROOT}/Source/NavalNav/Grid/SeaGridPathfinder.cpp" \
	"${REPO_ROOT}/Source/NavalNav/Threat/ThreatEvaluator.cpp" \
	-o "${OUT_DIR}/AlgoSelfTest"

"${OUT_DIR}/AlgoSelfTest"
