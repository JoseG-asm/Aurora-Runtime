cmake_minimum_required(VERSION 3.10)

project(AuroraRuntimes)

set(ARCH ${BUILD_ARCH})

set(TOOLCHAIN "${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64") 

set(CMAKE_C_COMPILER "${TOOLCHAIN}/bin/${ARCH}-linux-android${ANDROID_SDK}-clang")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN}/bin/${ARCH}-linux-android${ANDROID_SDK}-clang++")


set(ENV{PATH} "${MINGW_PATH}/bin:${NDK_PATH}/bin:$ENV{PATH}")
set(MESON_CROSSFILE "${CMAKE_SOURCE_DIR}/crossfiles/meson-${ARCH}-crossfile")

option(FREEGLUT "Enable build of freeglut library" OFF)
option(ANDROIDSHM "Enable build of android-shm library" OFF)
option(GLU "Enable build of glu library" OFF)
option(WINE "Enable build of wine compatibility layer" ON)
option(ENABLE_LIBX11 "Enable libX11 compile" OFF)

# glu dep
set(glu_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/glu")

set(glu_BUILD_DIR "${glu_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${glu_BUILD_DIR})
file(MAKE_DIRECTORY ${glu_BUILD_DIR})

execute_process(
  COMMAND meson setup "${glu_BUILD_DIR}" "${glu_SOURCE_DIR}" --cross-file=${MESON_CROSSFILE} -Dprefix=${AURORA_WORKDIR} -Dbuildtype=release 
  COMMAND_ECHO STDOUT
  RESULT_VARIABLE result
  OUTPUT_VARIABLE meson_output
  ERROR_VARIABLE meson_error
)

# retun 0

if(result)
  message(STATUS "meson result: ${result}")
  message(STATUS "meson output: <<${meson_output}>>")
  message(STATUS "meson error: <<${meson_error}>>")
  message(FATAL_ERROR "Meson error: ${meson_error}")
else()
  message(STATUS "Meson output:\n${meson_output}")
endif()

execute_process(
  COMMAND ninja -C ${glu_BUILD_DIR} install
  WORKING_DIRECTORY ${glu_BUILD_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "error to compile and install glu")
endif()

# brotli dep
set(brotli_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/brotli")

set(brotli_BUILD_DIR "${brotli_SOURCE_DIR}/build")


file(REMOVE_RECURSE ${brotli_BUILD_DIR})
file(MAKE_DIRECTORY ${brotli_BUILD_DIR})

execute_process(
  COMMAND cmake -S ${brotli_SOURCE_DIR} -B ${brotli_BUILD_DIR}
    -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK}/build/cmake/android.toolchain.cmake
    -DANDROID_ABI=x86_64 
    -DANDROID_PLATFORM=android-29 
    -DANDROID_NDK=${ANDROID_NDK}
    -DCMAKE_PREFIX_PATH=${AURORA_WORKDIR}
    -DANDROID_SDK=${ANDROID_SDK}
    -DCMAKE_INSTALL_PREFIX=${AURORA_WORKDIR}
  WORKING_DIRECTORY ${brotli_BUILD_DIR}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE cmake_output
  ERROR_VARIABLE cmake_error
  COMMAND_ECHO STDOUT
)

if(result)
  message(FATAL_ERROR "CMake Configure brotli error: ${cmake_error}")
else()
  message(STATUS "CMake output:\n${cmake_output}")
endif()

execute_process(
  COMMAND cmake --build ${brotli_BUILD_DIR} --target install
  WORKING_DIRECTORY ${brotli_BUILD_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "Error to compile and install brotli")
endif()

# freetype dep
set(ENV{LDFLAGS} "-L${AURORA_WORKDIR}/lib")
set(ENV{CFLAGS} "-I${AURORA_WORKDIR}/include")

set(freetype_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/freetype")

set(freetype_BUILD_DIR "${freetype_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${freetype_BUILD_DIR})
file(MAKE_DIRECTORY ${freetype_BUILD_DIR})


execute_process(
  COMMAND meson setup "${freetype_BUILD_DIR}" "${freetype_SOURCE_DIR}" --cross-file=${MESON_CROSSFILE} -Dprefix=${AURORA_WORKDIR} -Dbuildtype=release
  COMMAND_ECHO STDOUT
  RESULT_VARIABLE result
  OUTPUT_VARIABLE meson_output
  ERROR_VARIABLE meson_error
)

if(result)
  message(STATUS "Meson result: ${result}")
  message(STATUS "Meson output: <<${meson_output}>>")
  message(STATUS "Meson error: <<${meson_error}>>")
  message(FATAL_ERROR "Meson error: ${meson_error}")
else()
  message(STATUS "Meson output:\n${meson_output}")
endif()

execute_process(
  COMMAND ninja -C ${freetype_BUILD_DIR} install
  WORKING_DIRECTORY ${freetype_BUILD_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "error to compile and install freetype")
endif()

# gstreamer dep
set(ENV{LDFLAGS} "-L${AURORA_WORKDIR}/lib")
set(ENV{CFLAGS} "-I${AURORA_WORKDIR}/include")

set(gs_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/gstreamer")

set(gs_BUILD_DIR "${gs_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${gs_BUILD_DIR})
file(MAKE_DIRECTORY ${gs_BUILD_DIR})


execute_process(
  COMMAND meson setup "${gs_BUILD_DIR}" "${gs_SOURCE_DIR}" --cross-file=${MESON_CROSSFILE} -Dprefix=${AURORA_WORKDIR} -Dbuildtype=release -Dintrospection=disabled -Dcheck=disabled -Dtests=disabled -Dexamples=disabled -Dbenchmarks=disabled -Dlibunwind=disabled -Dlibdw=disabled -Dnls=disabled
  COMMAND_ECHO STDOUT
  RESULT_VARIABLE result
  OUTPUT_VARIABLE meson_output
  ERROR_VARIABLE meson_error
)

if(result)
  message(STATUS "Meson result: ${result}")
  message(STATUS "Meson output: <<${meson_output}>>")
  message(STATUS "Meson error: <<${meson_error}>>")
  message(FATAL_ERROR "Meson error: ${meson_error}")
else()
  message(STATUS "Meson output:\n${meson_output}")
endif()

execute_process(
  COMMAND ninja -C ${gs_BUILD_DIR} install
  WORKING_DIRECTORY ${gs_BUILD_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "error to compile and install gstreamer")
endif()

# xorg-util-macros
set(ENV{PKG_CONFIG_PATH} "${AURORA_WORKDIR}/lib/pkgconfig:${AURORA_WORKDIR}/share/pkgconfig")

set(xorgmacros_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/xorg-util-macros")
set(xorgmacros_BUILD_DIR "${xorgmacros_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${xorgmacros_BUILD_DIR})
file(MAKE_DIRECTORY ${xorgmacros_BUILD_DIR})

execute_process(
  COMMAND ./configure --libdir=${AURORA_WORKDIR}/lib --prefix=${AURORA_WORKDIR} --host=${ARCH}-linux-android host_alias=${ARCH}-linux-android 
  WORKING_DIRECTORY ${xorgmacros_SOURCE_DIR}
  COMMAND_ECHO STDOUT
  RESULT_VARIABLE result
  OUTPUT_VARIABLE meson_output
  ERROR_VARIABLE meson_error
)

if(result)
  message(STATUS "Meson result: ${result}")
  message(STATUS "Meson output: <<${meson_output}>>")
  message(STATUS "Meson error: <<${meson_error}>>")
  message(FATAL_ERROR "Meson error: ${meson_error}")
else()
  message(STATUS "Meson output:\n${meson_output}")
endif()

execute_process(
  COMMAND make -j8 install
  WORKING_DIRECTORY ${xorgmacros_SOURCE_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "error to compile and install xorg-util-macros")
endif()

# vulkan volk dep 
set(vulkan_volk_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/vulkan-volk")

set(vulkan_volk_BUILD_DIR "${vulkan_volk_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${vulkan_volk_BUILD_DIR})
file(MAKE_DIRECTORY ${vulkan_volk_BUILD_DIR})

execute_process(
  COMMAND cmake -S ${vulkan_volk_SOURCE_DIR} -B ${vulkan_volk_BUILD_DIR}
    -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK}/build/cmake/android.toolchain.cmake
    -DANDROID_ABI=x86_64 
    -DANDROID_PLATFORM=android-29 
    -DANDROID_NDK=${ANDROID_NDK}
    -DCMAKE_PREFIX_PATH=${AURORA_WORKDIR}
    -DANDROID_SDK=${ANDROID_SDK}
    -DCMAKE_INSTALL_PREFIX=${AURORA_WORKDIR}
    -DVULKAN_HEADERS_INSTALL_DIR=${AURORA_WORKDIR}
    -DVOLK_INSTALL=ON
  WORKING_DIRECTORY ${vulkan_volk_BUILD_DIR}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE cmake_output
  ERROR_VARIABLE cmake_error
  COMMAND_ECHO STDOUT
)

if(result)
  message(FATAL_ERROR "CMake Configure error: ${cmake_error}")
else()
  message(STATUS "CMake output:\n${cmake_output}")
endif()

execute_process(
  COMMAND cmake --build ${vulkan_volk_BUILD_DIR} --target install
  WORKING_DIRECTORY ${vulkan_volk_BUILD_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "Error to compile and install vulkan-volk")
endif()




# vulkan-headers
set(vulkan_headers_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/vulkan-headers")

set(vulkan_headers_BUILD_DIR "${vulkan_headers_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${vulkan_headers_BUILD_DIR})
file(MAKE_DIRECTORY ${vulkan_headers_BUILD_DIR})

execute_process(
  COMMAND cmake -S ${vulkan_headers_SOURCE_DIR} -B ${vulkan_headers_BUILD_DIR}
    -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK}/build/cmake/android.toolchain.cmake
    -DANDROID_ABI=x86_64 
    -DANDROID_PLATFORM=android-29 
    -DANDROID_NDK=${ANDROID_NDK}
    -DCMAKE_PREFIX_PATH=${AURORA_WORKDIR}
    -DANDROID_SDK=${ANDROID_SDK}
    -DCMAKE_INSTALL_PREFIX=${AURORA_WORKDIR}
    -DVULKAN_HEADERS_INSTALL_DIR=${AURORA_WORKDIR}
  WORKING_DIRECTORY ${vulkan_headers_BUILD_DIR}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE cmake_output
  ERROR_VARIABLE cmake_error
  COMMAND_ECHO STDOUT
)

if(result)
  message(FATAL_ERROR "CMake Configure error: ${cmake_error}")
else()
  message(STATUS "CMake output:\n${cmake_output}")
endif()

execute_process(
  COMMAND cmake --build ${vulkan_headers_BUILD_DIR} --target install
  WORKING_DIRECTORY ${vulkan_headers_BUILD_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "Error to compile and install vulkan-headers")
endif()


# vulkan-tools dep
set(vulkan_tools_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/vulkan-tools")

set(vulkan_tools_BUILD_DIR "${vulkan_tools_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${vulkan_tools_BUILD_DIR})
file(MAKE_DIRECTORY ${vulkan_tools_BUILD_DIR})

execute_process(
  COMMAND cmake -S ${vulkan_tools_SOURCE_DIR} -B ${vulkan_tools_BUILD_DIR}
    -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK}/build/cmake/android.toolchain.cmake
    -DANDROID_ABI=x86_64 
    -DANDROID_PLATFORM=android-29 
    -DANDROID_NDK=${ANDROID_NDK}
    -DCMAKE_PREFIX_PATH=${AURORA_WORKDIR}
    -DANDROID_SDK=${ANDROID_SDK}
    -DCMAKE_INSTALL_PREFIX=${AURORA_WORKDIR}
    -DCMAKE_SYSTEM_NAME=Linux
    -DBUILD_CUBE=ON -DBUILD_ICD=OFF
    -DBUILD_WSI_WAYLAND_SUPPORT=OFF
    -DBUILD_WSI_XCB_SUPPORT=ON
    -DBUILD_WSI_XLIB_SUPPORT=ON
    -DVULKAN_HEADERS_INSTALL_DIR=${AURORA_WORKDIR}
  WORKING_DIRECTORY ${vulkan_tools_BUILD_DIR}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE cmake_output
  ERROR_VARIABLE cmake_error
  COMMAND_ECHO STDOUT
)

if(result)
  message(FATAL_ERROR "CMake Configure error: ${cmake_error}")
else()
  message(STATUS "CMake output:\n${cmake_output}")
endif()

execute_process(
  COMMAND cmake --build ${vulkan_tools_BUILD_DIR} --target install
  WORKING_DIRECTORY ${vulkan_tools_BUILD_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "Error to compile and install vulkan-tools")
endif()


# libX11 dep
if(ENABLE_LIBX11)
set(libX11_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/libX11")
set(libX11_BUILD_DIR "${libX11_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${libX11_BUILD_DIR})
file(MAKE_DIRECTORY ${libX11_BUILD_DIR})

execute_process(
  COMMAND ./configure --libdir=${AURORA_WORKDIR}/lib --prefix=${AURORA_WORKDIR} --host=${ARCH}-linux-android host_alias=${ARCH}-linux-android --enable-malloc0returnsnull
  WORKING_DIRECTORY ${libX11_SOURCE_DIR}
  COMMAND_ECHO STDOUT
  RESULT_VARIABLE result
  OUTPUT_VARIABLE meson_output
  ERROR_VARIABLE meson_error
)

if(result)
  message(STATUS "Meson result: ${result}")
  message(STATUS "Meson output: <<${meson_output}>>")
  message(STATUS "Meson error: <<${meson_error}>>")
  message(FATAL_ERROR "Meson error: ${meson_error}")
else()
  message(STATUS "Meson output:\n${meson_output}")
endif()

execute_process(
  COMMAND make -j8 install
  WORKING_DIRECTORY ${libX11_SOURCE_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "error to compile and install libX11")
  message(FATAL_ERROR "could not install libX11")
endif()

endif()

# freeglut dep
if(FREEGLUT)
set(freeglut_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/freeglut")

if(EXISTS freeglut_SOURCE_DIR)
   message(FATAL_ERROR "The directory ${freeglut_SOURCE_DIR} not exists.")   
endif()

set(freeglut_BUILD_DIR "${freeglut_SOURCE_DIR}/build")

file(REMOVE_RECURSE ${freeglut_BUILD_DIR})
file(MAKE_DIRECTORY ${freeglut_BUILD_DIR})

if(EXISTS freeglut_BUILD_DIR)
  message(FATAL_ERROR "The directory ${freeglut_BUILD_DIR} not exists")
endif()

execute_process(
  COMMAND cmake -S ${freeglut_SOURCE_DIR} -B ${freeglut_BUILD_DIR}
    -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK}/build/cmake/android.toolchain.cmake
    -DANDROID=OFF
    -DANDROID_ABI=x86_64 
    -DANDROID_PLATFORM=android-29 
    -DFREEGLUT_GLES=ON
    -DFREEGLUTCOCOA=OFF
    -DFREEGLUT_BUILD_DEMOS=NO
    -DOPENGL_INCLUDE_DIR=${AURORA_WORKDIR}/include 
    -DOPENGL_gl_LIBRARY=${AURORA_WORKDIR}/lib/libGLU.so
    -DANDROID_NDK=${ANDROID_NDK}
    -DCMAKE_PREFIX_PATH=${AURORA_WORKDIR}
    -DANDROID_SDK=${ANDROID_SDK}
    -DCMAKE_INSTALL_PREFIX=${AURORA_WORKDIR}
  WORKING_DIRECTORY ${freeglut_BUILD_DIR}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE cmake_output
  ERROR_VARIABLE cmake_error
  COMMAND_ECHO STDOUT
)

if(result)
  message(FATAL_ERROR "Configure freeglut error: ${cmake_error}")
else()
  message(STATUS "CMake output:\n${cmake_output}")
endif()

execute_process(
  COMMAND cmake --build ${freeglut_BUILD_DIR} --target install
  WORKING_DIRECTORY ${freeglut_BUILD_DIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "error to compile and install freeglut")
endif()
endif()

# create aurora-runtime.tar.gz
execute_process(
  COMMAND tar czf ${CMAKE_SOURCE_DIR}/runtime.tar.gz .
  WORKING_DIRECTORY ${AURORA_WORKDIR}
  RESULT_VARIABLE result
)

if(result)
  message(FATAL_ERROR "error to compress aurora-runtime.tar.gz")
endif()


