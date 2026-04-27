#!/bin/bash

set -e

PROJECT_BUILD_DIR=$1
DPA_KERNEL_SRC=$2
TARGET_NAME=$3
PROGRAM_NAME=$4
DPACC_MCPU_FLAG=$5
DOCA_LIB_DIR=$6

DOCA_DIR="/opt/mellanox/doca"
DOCA_INCLUDE="${DOCA_DIR}/include"
DOCA_TOOLS="${DOCA_DIR}/tools"
DOCA_DPACC="${DOCA_TOOLS}/dpacc"

HOST_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_ALLOW_EXPERIMENTAL_API"
DEVICE_CC_FLAGS="-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API -O2"
DPA_APP_NAME="dpa_sample_app"

TARGET_BUILD_DIR="${PROJECT_BUILD_DIR}/${TARGET_NAME}/device/build_dpacc"

rm -rf "${TARGET_BUILD_DIR}"
mkdir -p "${TARGET_BUILD_DIR}"

"${DOCA_DPACC}" "${DPA_KERNEL_SRC}" \
	-o "${TARGET_BUILD_DIR}/${PROGRAM_NAME}.a" \
	-mcpu="${DPACC_MCPU_FLAG}" \
	-hostcc=gcc \
	-hostcc-options="${HOST_CC_FLAGS}" \
	--devicecc-options="${DEVICE_CC_FLAGS}" \
	--app-name="${DPA_APP_NAME}" \
	-device-libs="-L${DOCA_LIB_DIR} -ldoca_dpa_dev -ldoca_dpa_dev_comm" \
	-flto \
	-I"${DOCA_INCLUDE}"
