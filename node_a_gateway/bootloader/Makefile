################################################################################
# \file Makefile
# \version 1.0
#
# \brief
# Top-level APPLICATION Makefile — M4 Seam 0 (ADR-0017 D4 / docs/briefs/
# M4-bringup-plan.md). Was a single COMBINED project (gateway_fbl) through
# M1–M3; split here into two PROJECTs so the crypto-service CM0+ image
# (proj_cm0p) is a real, buildable sibling of the FBL (proj_cm4) instead of
# the vendor's linked-in COMPONENT_CM0P_SLEEP prebuilt. `make build`/`make
# program` at THIS level build/flash both; each project's own Makefile still
# works standalone from within its own directory.
#
# Validated on MTB 3.8 (2026-07-06): both projects `make build` clean —
# proj_cm4 (CM4 FBL) and proj_cm0p (CM0+) compile + link with MTB_DISABLED_
# COMPONENTS=CM0P_SLEEP, so the vendor CM0+ prebuilt is excluded and proj_cm0p
# fills .cy_m0p_image instead. The initial "older build flow" break was a
# missing CY_GETLIBS_SHARED_PATH here (set below); getlibs then generates the
# per-project libs/mtb.mk offline from the already-locked mtb_shared assets
# (its later non-zero exit is only the blocked online-manifest refresh, not a
# build blocker). STILL PENDING: on-board flash + the Seam-0 no-regression
# check (board boots the app as it did at end of M3) — see M4-bringup-plan.md.
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

# Explicit project list (see the UNVALIDATED note above). Order matters for
# `make program` on some MTB recipes (CM0+ flashed before CM4 is conventional,
# since the CM4 region sits right after it in the same code flash) — kept in
# that order defensively; confirm against the installed recipe-make-cat1a.
MTB_PROJECTS=proj_cm0p proj_cm4

# Keep the application root aligned with both child projects so MTB can resolve
# the BSP and build recipe before it recurses into proj_cm0p/proj_cm4.
TARGET=CYTVII-B-E-1M-SK
TOOLCHAIN=GCC_ARM
CONFIG=Debug

VERBOSE=

# Required by current MTB build flow detection in start.mk. This application
# root sits at the same depth as node_a_gateway/app, so it resolves the shared
# repo into the same repo-root mtb_shared/ directory.
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

include $(CY_TOOLS_DIR)/make/application.mk
