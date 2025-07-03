export ARCH="x86_64"
export ANDROID_SDK=29
export SDK_PATH=$HOME/Android
export NDK_PATH=$HOME/ndk
export TOOLCHAIN=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64
export MINGW_PATH=$HOME/mingw-llvm
export WORKDIR=$HOME/aurora-runtime/workdir
export PATH="$TOOLCHAIN/bin:$MINGW_PATH/bin:$PATH"
export WINETOOLS_PATH="$HOME/aurora-runtime/wine/build-tools"

export CC="${TOOLCHAIN}/bin/${ARCH}-linux-android${ANDROID_SDK}-clang"
export CXX=CC++

cmake -DCMAKE_CXX_FLAGS="-Wno-error=incompatible-function-pointer-types" -DCMAKE_TOOLCHAIN_FILE=$NDK_PATH/build/cmake/android.toolchain.cmake -DANDROID_SDK=$SDK_PATH -DANDROID_NDK=$NDK_PATH -DAURORA_WORKDIR=$WORKDIR -DWINETOOLS_PATH=$WINETOOLS_PATH -DBUILD_ARCH=$ARCH -DMINGW_SYSROOT_PATH=$MINGW_PATH .                              