LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := VSLProbe
LOCAL_SRC_FILES := main.cpp
LOCAL_C_INCLUDES += $(AML_INC)
LOCAL_CPPFLAGS  += -std=c++17 -O2 -DNDEBUG -fno-exceptions -fno-rtti
LOCAL_LDLIBS    := -llog
include $(BUILD_SHARED_LIBRARY)
