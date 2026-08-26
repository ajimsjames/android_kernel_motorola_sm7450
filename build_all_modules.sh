#!/bin/bash
# Script to compile all internal and external modules for sm7450 (tank) inside sm7450 workspace

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$SCRIPT_DIR"
OUT_DIR="$KERNEL_DIR/out"
MODULES_SRC_DIR="$(cd "$KERNEL_DIR/../sm7450-modules" && pwd)"
TOOLCHAIN_DIR="/home/aju/Android/evox/prebuilts/clang/host/linux-x86/clang-r574158"
GCC_DIR="/home/aju/Android/evox/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9"
BUILD_TOOLS_PATH="/home/aju/Android/evox/prebuilts/build-tools/path/linux-x86"
BUILD_TOOLS_BIN="/home/aju/Android/evox/prebuilts/build-tools/linux-x86/bin"
export PATH="$BUILD_TOOLS_PATH:$BUILD_TOOLS_BIN:$TOOLCHAIN_DIR/bin:$GCC_DIR/bin:$PATH"
mkdir -p "$OUT_DIR"
exec > >(tee -a "$OUT_DIR/build.log") 2>&1


export ARCH=arm64
export SUBARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export CLANG_TRIPLE=aarch64-linux-gnu-
export CONFIG_USB_POWER_DELIVERY=y

# Define common build parameters
export KCFLAGS="-Wno-error -Wno-error=strict-prototypes -Wno-strict-prototypes -I$MODULES_SRC_DIR/motorola/include -DPOWER_SUPPLY_TYPE_USB_HVDCP_3=21 -DPOWER_SUPPLY_TYPE_USB_HVDCP_3P5=22 -DPOWER_SUPPLY_TYPE_USB_FLOAT=23 -DPOWER_SUPPLY_TYPE_USB_HVDCP=20 -DPSY_IIO_MMI_OTG_ENABLE=116 -DPSY_IIO_USB_CHARGING_ENABLED=117 -DPSY_IIO_INPUT_CURRENT_SETTLED=118 -DPSY_IIO_USB_TERMINATION_ENABLED=119 -DPSY_IIO_MMI_QC3P_POWER=120 -DPSY_IIO_MMI_PD_VDM_VERIFY=121 -DPSY_IIO_MMI_CP_INPUT_CURRENT_NOW=122 -DPSY_IIO_MMI_CP_INPUT_VOLTAGE_NOW=123 -DPSY_IIO_CP_CLEAR_ERROR=124 -DPSY_IIO_MMI_CP_CHIP_ID=125"
export MAKE_ARGS="LLVM=1 LLVM_IAS=1 -C $KERNEL_DIR O=$OUT_DIR"

echo "=== Step 1: Merging configurations ==="
cd "$KERNEL_DIR"
mkdir -p "$OUT_DIR"
# Merging Waipio GKI and Tank specific configs
scripts/kconfig/merge_config.sh -m -O "$OUT_DIR" \
  arch/arm64/configs/gki_defconfig \
  arch/arm64/configs/vendor/waipio_GKI.config \
  arch/arm64/configs/vendor/ext_config/moto-waipio.config \
  arch/arm64/configs/vendor/ext_config/moto-waipio-gki.config \
  arch/arm64/configs/vendor/ext_config/moto-waipio-tank.config

echo "=== Step 2: Generating defconfig ==="
make O="$OUT_DIR" LLVM=1 LLVM_IAS=1 olddefconfig

echo "=== Step 2.05: Force-enabling security and HDCP dependencies ==="
scripts/config --file "$OUT_DIR/.config" -m CONFIG_QSEECOM
scripts/config --file "$OUT_DIR/.config" -m CONFIG_HDCP_QSEECOM
make O="$OUT_DIR" LLVM=1 LLVM_IAS=1 olddefconfig

echo "=== Step 2.1: Disabling debug info, LTO, and warnings-as-errors ==="
scripts/config --file "$OUT_DIR/.config" -d CONFIG_DEBUG_INFO
scripts/config --file "$OUT_DIR/.config" -d CONFIG_DEBUG_INFO_DWARF4
scripts/config --file "$OUT_DIR/.config" -e CONFIG_DEBUG_INFO_NONE
scripts/config --file "$OUT_DIR/.config" -d CONFIG_WERROR
scripts/config --file "$OUT_DIR/.config" -d CONFIG_LTO_CLANG -d CONFIG_LTO -d CONFIG_LTO_CLANG_FULL -d CONFIG_LTO_CLANG_THIN -e CONFIG_LTO_NONE
make O="$OUT_DIR" LLVM=1 LLVM_IAS=1 olddefconfig

echo "=== Step 2.5: Compiling Kernel Image & In-Tree Modules ==="
# Compile both core Image and all in-tree modules to populate Module.symvers correctly
make KCFLAGS="-Wno-error -Wno-error=strict-prototypes -Wno-strict-prototypes" O="$OUT_DIR" LLVM=1 LLVM_IAS=1 -j$(nproc) Image modules || { echo "ERROR: Kernel compilation failed!"; exit 1; }

echo "=== Step 2.7: Preparing modules build scripts ==="
make KCFLAGS="-Wno-error -Wno-error=strict-prototypes -Wno-strict-prototypes" O="$OUT_DIR" LLVM=1 LLVM_IAS=1 modules_prepare || { echo "ERROR: modules_prepare failed!"; exit 1; }

# Initialize Extra Symbols as empty (Kbuild loads the core kernel's Module.symvers automatically)
EXTRA_SYMBOLS=""

# Build mmrm-driver
if [ -d "$MODULES_SRC_DIR/qcom/opensource/mmrm-driver" ]; then
  echo "--------------------------------------------------"
  echo "Building MMRM module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/mmrm-driver"
  make $MAKE_ARGS M=$(pwd) MMRM_ROOT=$(pwd) KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build mmrm-driver"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

# Build cvp-kernel
if [ -d "$MODULES_SRC_DIR/qcom/opensource/cvp-kernel" ]; then
  echo "--------------------------------------------------"
  echo "Building CVP module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/cvp-kernel"
  make $MAKE_ARGS M=$(pwd) CVP_ROOT=$(pwd) KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build cvp-kernel"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

# Build display-drivers (with KERNEL_ROOT and KERNEL_SRC passed to fix clk-regmap.h include)
if [ -d "$MODULES_SRC_DIR/qcom/opensource/display-drivers" ]; then
  echo "--------------------------------------------------"
  echo "Building Display module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/display-drivers"
  source config/gki_waipiodisp.conf
  make $MAKE_ARGS M=$(pwd) DISPLAY_ROOT=$(pwd) KERNEL_ROOT=$KERNEL_DIR KERNEL_SRC=$KERNEL_DIR KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build display-drivers"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

# Build audio-kernel (with MODNAME passed to force external build and configuration inclusion)
if [ -d "$MODULES_SRC_DIR/qcom/opensource/audio-kernel" ]; then
  echo "--------------------------------------------------"
  echo "Building Audio module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/audio-kernel"
  source config/waipioauto.conf
  make $MAKE_ARGS M=$(pwd) AUDIO_ROOT=$(pwd) MODNAME=audio_dlkm KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build audio-kernel"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

# Build camera-kernel
if [ -d "$MODULES_SRC_DIR/qcom/opensource/camera-kernel" ]; then
  echo "--------------------------------------------------"
  echo "Building Camera module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/camera-kernel"
  make $MAKE_ARGS KERNEL_ROOT="$KERNEL_DIR" M=$(pwd) CAMERA_KERNEL_ROOT=$(pwd) KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build camera-kernel"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

# Build video-driver
if [ -d "$MODULES_SRC_DIR/qcom/opensource/video-driver" ]; then
  echo "--------------------------------------------------"
  echo "Building Video module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/video-driver"
  make $MAKE_ARGS M=$(pwd) VIDEO_ROOT=$(pwd) KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build video-driver"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi



# Build eva-kernel
if [ -d "$MODULES_SRC_DIR/qcom/opensource/eva-kernel" ]; then
  echo "--------------------------------------------------"
  echo "Building EVA module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/eva-kernel"
  make $MAKE_ARGS M=$(pwd) EVA_ROOT=$(pwd) KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build eva-kernel"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

# Build dataipa
if [ -d "$MODULES_SRC_DIR/qcom/opensource/dataipa" ]; then
  echo "--------------------------------------------------"
  echo "Building DataIPA module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/dataipa"
  make $MAKE_ARGS M=$(pwd) IPA_ROOT=$(pwd) KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build dataipa"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

# Build datarmnet core
if [ -d "$MODULES_SRC_DIR/qcom/opensource/datarmnet/core" ]; then
  echo "--------------------------------------------------"
  echo "Building Datarmnet Core module..."
  cd "$MODULES_SRC_DIR/qcom/opensource/datarmnet/core"
  make $MAKE_ARGS M=$(pwd) KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build datarmnet core"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

# Build datarmnet extensions
if [ -d "$MODULES_SRC_DIR/qcom/opensource/datarmnet-ext" ]; then
  echo "--------------------------------------------------"
  echo "Building Datarmnet Extensions..."
  for ext in "aps" "offload" "shs" "perf" "perf_tether" "sch" "wlan"; do
    if [ -d "$MODULES_SRC_DIR/qcom/opensource/datarmnet-ext/$ext" ]; then
      echo "Building Datarmnet Extension: $ext"
      cd "$MODULES_SRC_DIR/qcom/opensource/datarmnet-ext/$ext"
      make $MAKE_ARGS RMNET_CORE_INC_DIR="$MODULES_SRC_DIR/qcom/opensource/datarmnet/core" M=$(pwd) KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build datarmnet-ext/$ext"
      if [ -f "Module.symvers" ]; then
        EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
      fi
    fi
  done
fi

# Build WLAN separately with custom flags
if [ -d "$MODULES_SRC_DIR/qcom/opensource/wlan/qcacld-3.0" ]; then
  echo "--------------------------------------------------"
  echo "Building WLAN module: qcacld-3.0 (qca6750)"
  cd "$MODULES_SRC_DIR/qcom/opensource/wlan/qcacld-3.0"
  make $MAKE_ARGS M=$(pwd) WLAN_ROOT=$(pwd) CONFIG_QCA_CLD_WLAN=m CONFIG_WBUILD=y CONFIG_QCA_CLD_WLAN_PROFILE=qca6750 MODNAME=qca_cld3_qca6750 KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build qcacld-3.0"
  if [ -f "Module.symvers" ]; then
    EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
  fi
fi

echo "=== Step 4: Compiling Motorola external modules ==="
MOTO_DIRS=(
  "motorola/drivers/mmi_annotate"
  "motorola/drivers/sensors"
  "motorola/drivers/mmi_info"
  "motorola/drivers/mmi_relay"
  "motorola/drivers/ese/st54x"
  "motorola/drivers/input/misc/goodix_fod_mmi"
  "motorola/drivers/input/misc/vl53L1_14_1_2"
  "motorola/drivers/input/touchscreen/touchscreen_mmi"
  "motorola/drivers/input/touchscreen/goodix_berlin_mmi"
  "motorola/drivers/misc/awinic/sarsensor"
  "motorola/drivers/misc/mmi_sys_temp"
  "motorola/drivers/misc/sx937x"
  "motorola/drivers/misc/utag"
  "motorola/drivers/moto_netopt/con_dfpar"
  "motorola/drivers/moto_f_mass_storage"
  "motorola/drivers/moto_f_usbnet"
  "motorola/drivers/moto_mm"
  "motorola/drivers/moto_mmap_fault"
  "motorola/drivers/moto_sched"
  "motorola/drivers/moto_swap"
  "motorola/drivers/nfc/sn1xx"
  "motorola/drivers/nfc/st21nfc"
  "motorola/drivers/power/bm_adsp_ulog"
  "motorola/drivers/power/mmi_charger"
  "motorola/drivers/power/qpnp_adaptive_charge"
  "motorola/drivers/power/qti_glink_charger"
  "motorola/drivers/regulator/dio8018"
  "motorola/drivers/regulator/slg5bm43670"
  "motorola/drivers/regulator/wl2864c"
  "motorola/drivers/usb/typec/adapter_class"
  "motorola/drivers/power/mmi_discrete_charger"
  "motorola/drivers/usb/typec/mmi_tcpc"
  "motorola/drivers/watchdogtest"
  "motorola/drivers/wlan_elna"
)

for dir in "${MOTO_DIRS[@]}"; do
  full_path="$MODULES_SRC_DIR/$dir"
  if [ -d "$full_path" ]; then
    echo "--------------------------------------------------"
    echo "Building Motorola module in: $dir"
    cd "$full_path"
    # Compile using local Makefile if it exists, to automatically include KBUILD_OPTIONS
    if [ -f "Makefile" ]; then
      if make KERNEL_SRC="$KERNEL_DIR" M="$(pwd)" LLVM=1 LLVM_IAS=1 O="$OUT_DIR" ARCH=arm64 -n modules >/dev/null 2>&1; then
        make KERNEL_SRC="$KERNEL_DIR" M="$(pwd)" LLVM=1 LLVM_IAS=1 O="$OUT_DIR" ARCH=arm64 KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" modules || echo "  - Warning: Failed to build $dir"
      else
        make KERNEL_SRC="$KERNEL_DIR" M="$(pwd)" LLVM=1 LLVM_IAS=1 O="$OUT_DIR" ARCH=arm64 KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" || echo "  - Warning: Failed to build $dir"
      fi
    else
      make $MAKE_ARGS KBUILD_EXTRA_SYMBOLS="$EXTRA_SYMBOLS" M="$(pwd)" modules || echo "  - Warning: Failed to build $dir"
    fi
    if [ -f "Module.symvers" ]; then
      EXTRA_SYMBOLS="$EXTRA_SYMBOLS $(pwd)/Module.symvers"
    fi
  fi
done

echo "=== Step 5: Collecting all compiled modules ==="
COMPILED_DIR="$OUT_DIR/compiled_modules"
rm -rf "$COMPILED_DIR"
mkdir -p "$COMPILED_DIR"

# Copy in-tree modules
echo "Collecting in-tree modules..."
find "$OUT_DIR" -name "*.ko" -not -path "$COMPILED_DIR/*" -exec cp -t "$COMPILED_DIR" {} + || true

# Copy external modules
echo "Collecting external modules..."
find "$MODULES_SRC_DIR" -name "*.ko" -exec cp -t "$COMPILED_DIR" {} + || true

echo "=== All builds completed! ==="
echo "Total compiled modules in $COMPILED_DIR:"
ls -1 "$COMPILED_DIR" | wc -l
