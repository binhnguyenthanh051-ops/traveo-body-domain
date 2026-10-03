################################################################################
# \file Makefile
# \version 1.0
#
# \brief
# Top-level application make file.
#
################################################################################
# \copyright
# (c) 2025-2026, Infineon Technologies AG, or an affiliate of Infineon
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

# Type of ModusToolbox Makefile Options include:
#
# COMBINED    -- Top Level Makefile usually for single standalone application
# APPLICATION -- Top Level Makefile usually for multi project application
# PROJECT     -- Project Makefile under Application
#
MTB_TYPE=PROJECT

# Which core this project targets (multi-project app). Must match a name in
# bsp.mk's DEVICE_CYT4BF8CDS_CORES (CORE_NAME_CM0P_0 CORE_NAME_CM7_0
# CORE_NAME_CM7_1). CM7_1 is unused in M5.
CORE=CM7
CORE_NAME=CM7_0

# Target board/hardware (BSP).
# To change the target, it is required to use the Library manager
# ('make library-manager' from command line), which will also update Eclipse IDE launch
# configurations.
TARGET=APP_KIT_T2G-B-H_LITE

# Name of application (used to derive name of final linked file).
#
# If APPNAME is edited, ensure to update or regenerate launch
# configurations for your IDE.
APPNAME=actuator_cm7

# Name of toolchain to use. Options include:
#
# GCC_ARM -- GCC is available as part of ModusToolbox Setup program
# ARM     -- ARM Compiler (must be installed separately)
# IAR     -- IAR Compiler (must be installed separately)
#
# See also: CY_COMPILER_PATH below
TOOLCHAIN=GCC_ARM

# Default build configuration. Options include:
#
# Debug -- build with minimal optimizations, focus on debugging.
# Release -- build with full optimizations
# Custom -- build with custom configuration, set the optimization flag in CFLAGS
#
# If CONFIG is manually edited, ensure to update or regenerate launch configurations
# for your IDE.
CONFIG=Debug

# If set to "true" or "1", display full command-lines when building.
VERBOSE=


################################################################################
# Advanced Configuration
################################################################################

# Enable optional code that is ordinarily disabled by default.
#
# Available components depend on the specific targeted hardware and firmware
# in use. In general, if you have
#
#    COMPONENTS=foo bar
#
# ... then code in directories named COMPONENT_foo and COMPONENT_bar will be
# added to the build
#
COMPONENTS=FREERTOS RTOS_AWARE

# Disable the vendor CM0+ prebuilt so it is NOT linked into this CM7 image's
# .cy_m0p_image — our ../proj_cm0p builds the real CM0+ image into that
# reserved flash_cm0p window instead (see proj_cm0p/Makefile).
DISABLE_COMPONENTS=XMC7x_CM0P_SLEEP

# By default the build system automatically looks in the Makefile's directory
# tree for source code and builds it. The SOURCES variable can be used to
# manually add source code to the build process from a location not searched
# by default, or otherwise not found by the build system.
# Crypto-offload CLIENT (ADR-0018 / ADR-0021): the app links the host-tested
# transport + framing + MAC client. src/port_crypto.c (the ipc_port_if_t HW
# binding + MPU non-cacheable setup) is auto-discovered under src/. The CM7 is
# the mailbox CLIENT, so it DOES link ipc_mailbox.c (unlike the CM0+ server).
SOURCES=../../shared/crypto/src/ipc_mailbox.c \
        ../../shared/log/src/log.c \
        ../../shared/crypto/src/crypto_msg.c \
        ../../shared/crypto/src/crypto_service.c \
        ../../shared/secoc/src/secoc.c \
        ../../shared/secoc/src/secoc_freshness.c \
        ../../shared/secoc/src/secoc_rx.c \
        ../../shared/messages/src/body_msgs.c

# Like SOURCES, but for include directories. Value should be paths to
# directories (without a leading -I). ../ipc/include = Node B mailbox map;
# ../../security/include = the PUBLIC SecOC key_id (no secret bytes — REQ-SECOC-011).
INCLUDES=../ipc/include \
         ../../shared/can/include \
         ../../shared/crypto/include \
         ../../shared/secoc/include \
         ../../shared/messages/include \
         ../../shared/log/include \
         ../../security/include
# ../ipc/include MUST come first because Node A and Node B both provide an
# ipc_mailbox_map.h; the CM7 client must bind the Node B mailbox definition.

# Add additional defines to the build process (without a leading -D).
DEFINES=

# Banner identity (ADR-0023). Distinguishes this node's stream from Node A's
# when the BVT captures both consoles. Must come AFTER the plain `DEFINES=`
# above, which would otherwise wipe it.
DEFINES+=LOG_IMAGE_NAME=\"actuator\"

# Bench toggle (M5 Stage 3.3): `SECOC_CRYPTO_BRINGUP=1` on the make command line
# builds secoc_crypto_bringup_mac() (port_crypto.c). Named variable, not
# DEFINES=..., so LOG_IMAGE_NAME above survives.
ifeq ($(SECOC_CRYPTO_BRINGUP),1)
DEFINES+=SECOC_CRYPTO_BRINGUP=1
endif

# Bench toggle: `CAN_LOOPBACK_TEST=1` = Phase A on-chip loopback (no bus needed).
# The default (unset) is Phase B, the real bus (can_task.c).
ifeq ($(CAN_LOOPBACK_TEST),1)
DEFINES+=CAN_LOOPBACK_TEST=1
endif

# Bench toggle: `CAN_ECHO_TEST=1` re-enables the echo of unknown CAN IDs for the
# single-node echo probe (host_tools/can_echo_probe). Off by default: with both
# nodes on the bus the two echoes feed each other forever (F-011).
ifeq ($(CAN_ECHO_TEST),1)
DEFINES+=CAN_ECHO_TEST=1
endif

# Select softfp or hardfp floating point. Default is softfp.
VFP_SELECT=hardfp

# Additional / custom C compiler flags.
#
# NOTE: Includes and defines should use the INCLUDES and DEFINES variable
# above.
CFLAGS=

# Additional / custom C++ compiler flags.
#
# NOTE: Includes and defines should use the INCLUDES and DEFINES variable
# above.
CXXFLAGS=

# Additional / custom assembler flags.
#
# NOTE: Includes and defines should use the INCLUDES and DEFINES variable
# above.
ASFLAGS=

# Additional / custom linker flags.
ifeq ($(TOOLCHAIN),ARM)
LDFLAGS=--diag_suppress=L6848
endif

# Additional / custom libraries to link in to the application.
LDLIBS=

# Path to the linker script to use (if empty, use the default linker script).
LINKER_SCRIPT=

# Custom pre-build commands to run.
PREBUILD=

# Custom post-build commands to run.
POSTBUILD=


################################################################################
# Paths
################################################################################

# Relative path to the project directory (default is the Makefile's directory).
#
# This controls where automatic source code discovery looks for code.
CY_APP_PATH=

# Relative path to the shared repo location.
#
# All .mtb files have the format, <URI>#<COMMIT>#<LOCATION>. If the <LOCATION> field
# begins with $$ASSET_REPO$$, then the repo is deposited in the path specified by
# the CY_GETLIBS_SHARED_PATH variable. The default location is one directory level
# above the current app directory.
# This is used with CY_GETLIBS_SHARED_NAME variable, which specifies the directory name.
# node_b_actuator/proj_cm7/ is two levels below the repo root, so reach the
# shared repo-root mtb_shared/ (the one cache all variants share).
CY_GETLIBS_SHARED_PATH=../../

# Directory name of the shared repo location.
#
CY_GETLIBS_SHARED_NAME=mtb_shared

# Absolute path to the compiler's "bin" directory. The variable name depends on the
# toolchain used for the build. Refer to the ModusToolbox user guide to get the correct
# variable name for the toolchain used in your build.
#
# The default path depends on the selected TOOLCHAIN and is set in the Make recipe.
# CY_COMPILER_GCC_ARM_DIR?=


# Locate ModusToolbox helper tools folders in default installation
# locations for Windows, Linux, and macOS.
CY_WIN_HOME=$(subst \,/,$(USERPROFILE))
CY_TOOLS_PATHS ?= $(wildcard \
    $(CY_WIN_HOME)/ModusToolbox/tools_* \
    $(HOME)/ModusToolbox/tools_* \
    /Applications/ModusToolbox/tools_*)

# If you install ModusToolbox software in a custom location, add the path to its
# "tools_X.Y" folder (where X and Y are the version number of the tools
# folder). Make sure you use forward slashes.
CY_TOOLS_PATHS+=

# Default to the newest installed tools folder, or the users override (if it's
# found).
CY_TOOLS_DIR=$(lastword $(sort $(wildcard $(CY_TOOLS_PATHS))))

ifeq ($(CY_TOOLS_DIR),)
$(error Unable to find any of the available CY_TOOLS_PATHS -- $(CY_TOOLS_PATHS). On Windows, use forward slashes.)
endif

$(info Tools Directory: $(CY_TOOLS_DIR))

include $(CY_TOOLS_DIR)/make/start.mk
