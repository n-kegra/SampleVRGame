# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.4.1)

# build native_app_glue as a static lib
set(APP_GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)
include_directories(${APP_GLUE_DIR})
add_library( app-glue STATIC ${APP_GLUE_DIR}/android_native_app_glue.c)


add_definitions(-DXR_USE_PLATFORM_ANDROID)
# add_definitions(-DXR_USE_GRAPHICS_API_OPENGL_ES)
add_definitions(-DXR_USE_GRAPHICS_API_VULKAN)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")


file(GLOB LOCAL_HEADERS  "hello_xr/*.h")
file(GLOB LOCAL_SOURCE   "hello_xr/*.cpp")
file(GLOB VULKAN_SHADERS "hello_xr/vulkan_shaders/*.glsl")

LINK_DIRECTORIES(openxr_loader/${ANDROID_ABI})
include_directories(openxr_loader/include)

add_library( # Sets the name of the library.
        hello_xr
		SHARED
        ${LOCAL_SOURCE}
        ${LOCAL_HEADERS}
		${VULKAN_SHADERS}
        openxr_loader/include/common/gfxwrapper_opengl.c
		)

target_compile_features(hello_xr PUBLIC cxx_std_17)

source_group("Headers" FILES ${LOCAL_HEADERS})
source_group("Shaders" FILES ${VULKAN_SHADERS})


if(VulkanHeaders_INCLUDE_DIRS)
    target_include_directories(hello_xr
            PRIVATE
            ${VulkanHeaders_INCLUDE_DIRS}
            )
endif()

if(Vulkan_LIBRARY)
    target_link_libraries(hello_xr ${Vulkan_LIBRARY})
endif()

# Specifies libraries CMake should link to your target library. You
# can link multiple libraries, such as libraries you define in this
# build script, prebuilt third-party libraries, or system libraries.

find_package(OpenAL CONFIG REQUIRED)
target_link_libraries(hello_xr OpenAL::OpenAL)

find_package(FreeALUT CONFIG REQUIRED)
target_link_libraries(hello_xr FreeALUT::alut)

find_package(glm CONFIG REQUIRED)
target_link_libraries(hello_xr glm::glm)

find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")
target_include_directories(hello_xr PRIVATE ${TINYGLTF_INCLUDE_DIRS})

find_package(fmt CONFIG REQUIRED)
target_link_libraries(hello_xr fmt::fmt)

find_package(OpenXR CONFIG REQUIRED)
target_link_libraries(hello_xr OpenXR::headers)

#find_package(libsoundio CONFIG REQUIRED)
#target_link_libraries(hello_xr libsoundio::libsoundio libsoundio::libsoundio_static)

target_link_libraries( # Specifies the target library.
                       hello_xr
                       openxr_loader
                       android
                       vulkan
                       EGL
                       GLESv3
                       OpenSLES
                       app-glue
                       log
#						aaudio
                       )

