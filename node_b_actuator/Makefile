################################################################################
# \file Makefile
# \version 1.0
#
# \brief
# Node B (actuator) top-level APPLICATION Makefile — M5 Seam 0.
# Node B is a NEW silicon bring-up on CYT4BF8CDS (TRAVEO T2G Body High Lite,
# KIT_T2G-B-H_LITE): dual Cortex-M7 + Cortex-M0+, recipe-make-cat1c. Unlike
# Node A, there is NO bootloader — this application IS the whole node, built as
# two PROJECTs:
#   proj_cm0p — CM0+ security core: starts CM7_0, runs the crypto (MAC) server.
#   proj_cm7  — CM7_0 application:  FreeRTOS actuator FSM, CANFD, SecOC client.
# `make build` / `make program` HERE build/flash both; each project's own
# Makefile still works standalone from within its directory.
#
# Mirrors node_a_gateway/bootloader/Makefile (the proven Node A multi-core
# APPLICATION), adapted for cat1c and the app-only (no-FBL) node.
#
# UNVALIDATED: written from the copied BSP (bsp.mk: DEVICE=CYT4BF8CDS,
# DEVICE_CYT4BF8CDS_CORES = CORE_NAME_CM0P_0 CORE_NAME_CM7_0 CORE_NAME_CM7_1;
# BSP_COMPONENTS = XMC7x_CM0P_SLEEP). Run `make getlibs && make build` before
# trusting it, same discipline as Node A Seam 0.
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

MTB_TYPE=APPLICATION

# Explicit project list. Order matters for `make program` on some recipes:
# the CM0+ image sits at the start of code flash and reserves the window the
# CM7 image links after, so flash CM0+ first (defensive; confirm against the
# installed recipe-make-cat1c).
MTB_PROJECTS=proj_cm0p proj_cm7

# Keep the application root aligned with both child projects so MTB can resolve
# the shared BSP + build recipe before recursing. TARGET is the copied BSP name
# (bsps/TARGET_APP_KIT_T2G-B-H_LITE).
TARGET=APP_KIT_T2G-B-H_LITE
TOOLCHAIN=GCC_ARM
CONFIG=Debug

VERBOSE=

# This application root sits one level below the repo root (node_b_actuator/),
# so it resolves the shared library cache into the SAME repo-root mtb_shared/
# that Node A uses (one cache for all variants — see the repo .gitignore).
CY_GETLIBS_SHARED_PATH=../
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

include $(CY_TOOLS_DIR)/make/application.mk
