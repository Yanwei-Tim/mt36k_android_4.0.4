LOCAL_PATH :=$(call my-dir)
include $(CLEAR_VARS)

# Module name should match apk name to be installed
LOCAL_MODULE := lexue3.0
LOCAL_SRC_FILES := TCL_ALL_Lexue3.0.apk
LOCAL_MODULE_CLASS := APPS
LOCAL_MODULE_SUFFIX:=$(COMMON_ANDROID_PACKAGE_SUFFIX)

#choose apk's location 
LOCAL_MODULE_PATH := $(TARGET_OUT_APPS)
LOCAL_CERTIFICATE:=platform
LOCAL_MODULE_TAGS := optional
include $(BUILD_PREBUILT)