################################################################################
# \file Makefile
# \version 1.0
#
# \brief
# Node B M5 Seam 0/3: the CM0+ security-core project. Sibling to ../proj_cm7
# (the actuator app). Two responsibilities (ADR-0017 D4, mirrored for Node B):
#   1. Start CM7_0 — exactly what the vendor XMC7x_CM0P_SLEEP prebuilt does,
#      now as OUR OWN buildable image so the crypto server has a home.
#   2. Run the crypto (MAC) dispatch server (crypto seam): decode the mailbox
#      envelope, route CRYPTO_OP_MAC to the AES-CMAC back end, hold the secret
#      key. (ADR-0021 D1: HW CMAC — CY_IP_MXCRYPTO IS present on CYT4BF8CDS and
#      the PDL compiles cy_crypto_core_cmac_v2; the earlier "no MXCRYPTO" bsp.mk
#      note was a false alarm. mbedTLS is the documented software fallback only.)
#
# Seam order (like Node A): Seam 1 = start CM7 only (main_cm0p.c). Seam 3 =
# add the dispatch/framing/keystore SOURCES below + the CMAC back end.
#
# UNVALIDATED: written against the copied BSP COMPONENT_CM0P linker/startup and
# bsp.mk (DEVICE_CYT4BF8CDS_CORES: CORE_NAME_CM0P_0; BSP_COMPONENTS =
# XMC7x_CM0P_SLEEP). Run `make getlibs && make build` before trusting it.
#
################################################################################
# \copyright
# (c) 2018-2026, Infineon Technologies AG, or an affiliate of Infineon
# Technologies AG.  SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
################################################################################


################################################################################
# Basic Configuration
################################################################################

# PROJECT, not COMBINED — one of two projects under the APPLICATION top Makefile
# (../Makefile: MTB_PROJECTS=proj_cm0p proj_cm7).
MTB_TYPE=PROJECT

# Which core this project targets. Must match a name in bsp.mk's
# DEVICE_CYT4BF8CDS_CORES (CORE_NAME_CM0P_0 CORE_NAME_CM7_0 CORE_NAME_CM7_1).
CORE=CM0P
CORE_NAME=CM0P_0

TARGET=APP_KIT_T2G-B-H_LITE

# Distinct from proj_cm7's app name — this is the second hex the combined
# `make program` flashes, into the CM0+ flash region the CM7 linker reserves as
# cm0plus_code_flash_reserve (COMPONENT_CM7/.../linker.ld: .cy_cm0p_image at
# ORIGIN(flash_cm0p)).
APPNAME=actuator_cm0p

TOOLCHAIN=GCC_ARM
CONFIG=Debug
VERBOSE=


################################################################################
# Advanced Configuration
################################################################################

# The vendor CM0+ prebuilt is BSP-wide (bsp.mk: BSP_COMPONENTS=XMC7x_CM0P_SLEEP)
# and would otherwise occupy .cy_m0p_image — disable it so THIS project's image
# fills that region instead (cat1c equivalent of Node A's CM0P_SLEEP disable).
DISABLE_COMPONENTS=XMC7x_CM0P_SLEEP

COMPONENTS=

# CM0+ has no FPU — soft float.
VFP_SELECT=softfp

# Crypto server: the host-tested dispatch + framing + keystore land here at the
# crypto seam (their host tests already exist in shared/crypto/tests). The
# CRYPTO_OP_MAC handler + AES-CMAC back end (HW or mbedTLS, per the verify note
# above) + the shared secret key are target-only src under this project's tree
# (e.g. src/main_cm0p.c starts CM7 then services the mailbox; src/crypto_mac_
# cm0p.c is the back end). ipc_mailbox.c is NOT here — the CM0+ is the mailbox
# SERVER and reads/writes the shared region directly (ADR-0018; mirrors Node A).
#
# Seam 1 (start CM7 only): comment the SOURCES/INCLUDES below until the crypto
# seam, exactly as Node A brought its CM0+ up in stages.
SOURCES=../../shared/crypto/src/crypto_dispatch.c \
        ../../shared/crypto/src/crypto_msg.c \
        ../../shared/crypto/src/crypto_keystore.c
INCLUDES=../ipc/include \
         ../../shared/crypto/include \
         ../../security/include
# src/crypto_ops_cm0p.c (the CRYPTO_OP_MAC back end + shared-secret keystore) is
# auto-discovered under this project's src/ tree (like Node A). ../ipc/include
# MUST come before ../../shared/crypto/include because both trees ship an
# ipc_mailbox_map.h and Node B must bind its own mailbox map for the server loop.

DEFINES=

# Bench toggle (M5 Stage 2 + 4.1): `CRYPTO_BRINGUP_KAT=1` on the make command line
# compiles the CMAC known-answer test in (crypto_ops_cm0p.c). A named variable
# rather than DEFINES=..., so it can be given at the app root: it reaches every
# sub-make without wiping another project's DEFINES.
ifeq ($(CRYPTO_BRINGUP_KAT),1)
DEFINES+=CRYPTO_BRINGUP_KAT=1
endif
CFLAGS=
CXXFLAGS=
ASFLAGS=
LDFLAGS=
LDLIBS=

# No LINKER_SCRIPT override — use the BSP's COMPONENT_CM0P/TOOLCHAIN_GCC_ARM/
# linker.ld unmodified (it reserves the CM0+ code-flash window the CM7 linker
# expects). Verify both agree on cm0plus_code_flash_reserve at bring-up.
LINKER_SCRIPT=

PREBUILD=
POSTBUILD=


################################################################################
# Paths
################################################################################

CY_APP_PATH=

# Same physical repo-root mtb_shared/ as ../proj_cm7 and Node A — one cache, no
# duplicate downloads. This project lives at node_b_actuator/proj_cm0p/, two
# levels below the repo root.
CY_GETLIBS_SHARED_PATH=../../
CY_GETLIBS_SHARED_NAME=mtb_shared

CY_WIN_HOME=$(subst \,/,$(USERPROFILE))
CY_TOOLS_PATHS ?= $(wildcard \
    $(CY_WIN_HOME)/ModusToolbox/tools_* \
    $(HOME)/ModusToolbox/tools_* \
    /Applications/ModusToolbox/tools_*)

CY_TOOLS_PATHS+=

CY_TOOLS_DIR=$(lastword $(sort $(wildcard $(CY_TOOLS_PATHS))))

ifeq ($(CY_TOOLS_DIR),)
$(error Unable to find any of the available CY_TOOLS_PATHS -- $(CY_TOOLS_PATHS). On Windows, use forward slashes.)
endif

$(info Tools Directory: $(CY_TOOLS_DIR))

include $(CY_TOOLS_DIR)/make/start.mk
