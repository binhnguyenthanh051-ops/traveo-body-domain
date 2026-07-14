################################################################################
# \file Makefile
# \version 1.0
#
# \brief
# M4 Seam 0 (ADR-0017 D4 / docs/briefs/M4-bringup-plan.md): the crypto-service
# CM0+ project. Sibling to ../proj_cm4 (the FBL). At THIS seam it does nothing
# but bring up clocks and start the CM4 core -- exactly what the vendor
# COMPONENT_CM0P_SLEEP prebuilt did, now as OUR OWN buildable image instead of
# a linked-in blob, so shared/crypto's dispatch loop has a real project to
# land in at Seam 1+. No IPC, no crypto yet -- see the bring-up plan.
#
# UNVALIDATED: written against the BSP's existing COMPONENT_CM0P linker/
# startup and the DEVICE_CYT2B75CAS_CORES/CORE_NAME_CM0P_0 convention in
# bsps/.../bsp.mk, but no ModusToolbox install was available to run
# `make getlibs && make build` against it. Do that before trusting this file.
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

# PROJECT, not COMBINED -- this is one of two projects under the APPLICATION
# top Makefile (../Makefile: PROJECTS=proj_cm0p proj_cm4).
MTB_TYPE=PROJECT

# Required in multi-project apps: which core this project targets. Must match
# a name in bsp.mk's DEVICE_CYT2B75CAS_CORES (CORE_NAME_CM0P_0 CORE_NAME_CM4_0).
CORE=CM0P
CORE_NAME=CM0P_0

TARGET=CYTVII-B-E-1M-SK

# Distinct from proj_cm4's gateway_fbl -- this is the second hex the combined
# `make program` at the app level flashes (into the CM0+ flash region the FBL
# linker reserves as FLASH_CM0P_SIZE, ORIGIN(flash)..+0x20000).
APPNAME=gateway_fbl_cm0p

TOOLCHAIN=GCC_ARM
CONFIG=Debug
VERBOSE=


################################################################################
# Advanced Configuration
################################################################################

# The vendor CM0P_SLEEP prebuilt is BSP-wide (bsp.mk: BSP_COMPONENTS=CM0P_SLEEP)
# and would otherwise also try to occupy this project's own build -- disable it
# here too, for the same reason proj_cm4 disables it (see that Makefile's note).
DISABLE_COMPONENTS=CM0P_SLEEP

COMPONENTS=

# src/main_cm0p.c + src/crypto_ops_cm0p.c are auto-discovered under this
# project's own tree. M4 Seam 2/3: the host-tested dispatch + framing +
# keystore (crypto_dispatch.c, crypto_msg.c, crypto_keystore.c) join here —
# main_cm0p.c feeds the mailbox envelope to crypto_dispatch, which routes
# CRYPTO_OP_HASH / CRYPTO_OP_VERIFY_IMAGE to the HW handlers in
# crypto_ops_cm0p.c. (ipc_mailbox.c is NOT needed here — the CM0+ is the
# mailbox server, it reads/writes the shared region directly.)
# M4 Seam 5 (ADR-0020): src/prot_config_cm0p.c is auto-discovered under this
# project's tree; it consumes the host-tested region helper shared/prot, added
# here. The wall setup is behind FBL_M4_SEAM5_PROT (below) — OFF here so the
# build stays byte-identical to Seam 4; define it at the bench to enable.
SOURCES=../../../shared/crypto/src/crypto_dispatch.c \
        ../../../shared/crypto/src/crypto_msg.c \
        ../../../shared/crypto/src/crypto_keystore.c \
        ../../../shared/prot/src/prot_region.c
INCLUDES=../../../shared/crypto/include \
         ../../../shared/prot/include

# Bench toggles (all OFF by default so the committed image matches Seam 4):
#   FBL_M4_SEAM5_PROT   — raise the TCB-isolation walls (ADR-0020).
#   FBL_M4_SEAM6_DEAD   — CM0+ plays dead (never services crypto) -> CM4 verify
#                         times out -> stay-in-FBL (ADR-0016 D5).
#   FBL_M4_SEAM6_GARBAGE— CM0+ returns a corrupted reply -> decode reject ->
#                         stay-in-FBL. (Use SEAM6_DEAD and SEAM6_GARBAGE one at a
#                         time.) Example: DEFINES=FBL_M4_SEAM6_DEAD
DEFINES=
VFP_SELECT=
CFLAGS=
CXXFLAGS=
ASFLAGS=
LDFLAGS=
LDLIBS=

# No LINKER_SCRIPT override -- unlike proj_cm4 (which forks the CM4 linker for
# the shared .noinit pin, ADR-0007 D8), this project has no such requirement
# yet, so it uses the BSP's own bsps/.../COMPONENT_CM0P/TOOLCHAIN_GCC_ARM/
# linker.ld unmodified. That linker independently reserves the identical
# 0x1000_0000 / 0x20000 flash window proj_cm4/linker/fbl_cm4.ld expects
# (verify: both encode cm0plus_code_flash_reserve = 0x20000 at
# code_flash_base_address = 0x1000_0000 -- confirmed by inspection, not by a
# real link, since no toolchain was available here).
LINKER_SCRIPT=

PREBUILD=
POSTBUILD=


################################################################################
# Paths
################################################################################

CY_APP_PATH=

# Same physical mtb_shared/ as ../proj_cm4 (repo root) -- both projects resolve
# libraries into ONE directory, no duplicate downloads. One level deeper than
# the app-level bsps/ (this project lives at .../bootloader/proj_cm0p/).
CY_GETLIBS_SHARED_PATH=../../../
CY_GETLIBS_SHARED_NAME=mtb_shared

# CY_COMPILER_GCC_ARM_DIR=

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
