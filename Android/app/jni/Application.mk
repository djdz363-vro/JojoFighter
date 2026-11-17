
# Uncomment this if you're using STL in your project
# You can find more information here:
# https://developer.android.com/ndk/guides/cpp-support
# APP_STL := c++_shared

APP_ABI := armeabi-v7a arm64-v8a x86 x86_64
APP_OPTIM := release

# ---- JFA GPU A/B switches (M2 Stage C) ----
# JFA_GPU_SKIN / JFA_GPU_RENDER are forwarded by app/build.gradle as
# ndk-build variables (Gradle -P properties, default "0"/absent).
# Defaults define NEITHER macro: the permanent CPU-only build.
#   JFA_GPU_SKIN=1                  -> -DJFA_GPU_SKIN=1
#                                      (GPU prep active, CPU renders)
#   JFA_GPU_SKIN=1 JFA_GPU_RENDER=1 -> additionally -DJFA_GPU_RENDER=1
#                                      (GPU path draws, CPU fallback)
# JFA_GPU_RENDER=1 without JFA_GPU_SKIN=1 is rejected by build.gradle.
# No duplicate definitions: APP_CFLAGS is not set anywhere else.
ifeq ($(JFA_GPU_SKIN),1)
APP_CFLAGS += -DJFA_GPU_SKIN=1
ifeq ($(JFA_GPU_RENDER),1)
APP_CFLAGS += -DJFA_GPU_RENDER=1
endif
endif
# Min runtime API level
APP_PLATFORM=android-16
