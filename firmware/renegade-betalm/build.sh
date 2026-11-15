#!/usr/bin/env bash
# Renegade edk2-msm UEFI for the LG G8S (betalm) with UniXNU + DarwinBoot in its
# firmware volume. Produces a fastboot-bootable Android boot image (header v1).
#
#   firmware/renegade-betalm/build.sh OUT.img
#
# Linux host: clang/lld/llvm, aarch64-linux-gnu-gcc, nasm, iasl, uuid-dev,
# gettext, python3, git. Everything is fetched into $WORK (default ./fw-work).
set -euo pipefail
OUT=$(realpath -m "${1:?usage: build.sh OUT.img}")
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${WORK:-$PWD/fw-work}
mkdir -p "$WORK" && cd "$WORK"

UNIXNU_REV=b6ae99c17194e6a18fd819d3148e69fe405e91fc   # same pin as firmware-only.yml (raphael)
EDK2MSM_REV=${EDK2MSM_REV:-e195262}                     # edk2-porting/edk2-msm, tested 2026-11-15

# 1. Mu_Basecore (UniXNUPkg needs BaseFdtLib; Renegade's 2022 edk2 does not have it)
if [ ! -d Mu_Basecore ]; then
  git clone -q --depth 1 --branch release/202608 https://github.com/microsoft/mu_basecore Mu_Basecore
  git -C Mu_Basecore submodule update --init --depth 1 MdePkg/Library/BaseFdtLib/libfdt \
    MdePkg/Library/MipiSysTLib/mipisyst BaseTools/Source/C/BrotliCompress/brotli
fi

# 2. UniXNU + DarwinBoot at the raphael pin, with UniXNU-raphael's patches
if [ ! -d unixnu ]; then
  git init -q unixnu
  git -C unixnu fetch -q --depth 1 https://github.com/UniXNU/UniXNU "$UNIXNU_REV"
  git -C unixnu checkout -q FETCH_HEAD
  git -C unixnu submodule update --init --depth 1 drivers/DarwinBoot
  git -C unixnu apply "$REPO/patches/unixnu-guacamole.patch" "$REPO/patches/unixnu-diag.patch"
  for p in darwinboot-guacamole darwinboot-diag darwinboot-window-split darwinboot-window-below4g darwinboot-window-dram-base; do
    git -C unixnu/drivers/DarwinBoot apply "$REPO/patches/$p.patch"
  done
fi
UNIXNU_EDK2=$WORK/Mu_Basecore unixnu/package/build.sh -b DEBUG -t CLANGPDB >/dev/null
(
  cd unixnu/drivers/DarwinBoot
  E=$WORK/Mu_Basecore
  export WORKSPACE=$PWD PACKAGES_PATH=$PWD:$WORK/unixnu/package:$E EDK_TOOLS_PATH=$E/BaseTools \
    CONF_PATH=$PWD/Conf PYTHON_COMMAND=python3 CLANG_BIN=$(dirname "$(command -v clang)")/
  export PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH
  mkdir -p Conf
  for f in target tools_def build_rule; do cp "$E/BaseTools/Conf/$f.template" "Conf/$f.txt"; done
  build -a AARCH64 -t CLANGPDB -b DEBUG -p DarwinBootPkg/DarwinBoot.dsc -m DarwinBootPkg/Driver/DarwinBoot.inf >/dev/null
)

# 3. Renegade edk2-msm; freetype comes from its GitHub mirror
if [ ! -d edk2-msm ]; then
  git clone -q https://github.com/edk2-porting/edk2-msm edk2-msm
  git -C edk2-msm checkout -q "$EDK2MSM_REV"
  git -C edk2-msm submodule update --init --depth 1 Common/edk2 Common/edk2-platforms Platform/EFI_Binaries GPLDrivers/Library/SimpleInit
  git -C edk2-msm/Common/edk2 submodule update --init --depth 1
  git -C edk2-msm/GPLDrivers/Library/SimpleInit config submodule.libs/freetype.url https://github.com/freetype/freetype.git
  git -C edk2-msm/GPLDrivers/Library/SimpleInit submodule update --init --depth 1 libs/freetype
  git -C edk2-msm apply "$HERE/betalm-unixnu-fdf.patch"
fi
D=edk2-msm/Platform/LG/sm8150/UniXNU
mkdir -p "$D"
B=unixnu/package/Build
cp "$B/Out/AARCH64-DEBUG/UniXNU.efi" "$B/Out/AARCH64-DEBUG/UniXNUDxe.efi" \
   "$B/UniXNU/DEBUG_CLANGPDB/AARCH64/UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe/OUTPUT/UniXNUDxe.depex" \
   unixnu/drivers/DarwinBoot/build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi "$D/"
(cd edk2-msm && bash build.sh -d betalm -r RELEASE >/dev/null)
grep -q 9B665204-A133-47A2-9396-9E2B370A4E25 edk2-msm/workspace/Build/betalm/RELEASE_CLANG38/FV/FVMAIN.inf
cp edk2-msm/boot-betalm.img "$OUT"
sha256sum "$OUT"
