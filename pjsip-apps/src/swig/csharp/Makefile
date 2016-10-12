include ../../../../build.mak
include ../../../../build/common.mak

ifneq ($(findstring android,$(TARGET_NAME)),)
 OS=android
else
 ifneq ($(findstring darwin,$(TARGET_NAME)),)
  OS=darwin
 endif
endif

OUT_DIR=output
LIB_DIR=$(OUT_DIR)
ifeq ($(OS),Windows_NT)
  LIBPJSUA2_SO=$(OUT_DIR)/pjsua2.dll
else
  ifeq ($(OS),darwin)
    LIBPJSUA2_SO=$(OUT_DIR)/pjsua2.bundle
  else
    ifeq ($(OS),android)
      LIBPJSUA2_SO=android/libs/armeabi/libpjsua2.so
      LIB_DIR=android/libs/armeabi
    else
      LIBPJSUA2_SO=$(OUT_DIR)/libpjsua2.so
    endif
  endif
endif

# Env settings, e.g: path to SWIG, JDK, java(.exe), javac(.exe)
MY_SWIG		 := swig
MY_MONO	 	 := mono
MY_MONOC	 := mcs
MY_MONO_CFLAGS	 := -I.
MY_MONO_LDFLAGS	 :=

# OS specific
ifeq ($(OS),Windows_NT)
  MY_MONO_LDFLAGS += -Wl,--kill-at
else
  MY_MONO_CFLAGS += -fPIC
  ifeq ($(OS),android)
    MY_MONO_CFLAGS+= -D__ANDROID__
    SWIG_FLAGS+= -D__ANDROID__
  endif
endif

# Build settings
MY_CFLAGS	 := $(PJ_CXXFLAGS) $(MY_MONO_CFLAGS) $(CFLAGS)
MY_LDFLAGS	 := $(PJ_LDXXFLAGS) $(PJ_LDXXLIBS) $(MY_MONO_LDFLAGS) $(LDFLAGS)
MY_PACKAGE_NAME	 := org.pjsip.pjsua2
ifeq ($(OS),android)
  MY_PACKAGE_PATH  := android/src/$(subst .,/,$(MY_PACKAGE_NAME))
else
  MY_PACKAGE_PATH  := $(OUT_DIR)/$(subst .,/,$(MY_PACKAGE_NAME))
endif

.PHONY: all csharp install uninstall

all: $(LIBPJSUA2_SO) csharp

$(LIBPJSUA2_SO): $(OUT_DIR)/pjsua2_wrap.o
	mkdir -p $(LIB_DIR)
	$(PJ_CXX) -dynamiclib -o $(LIBPJSUA2_SO) $(OUT_DIR)/pjsua2_wrap.o \
		$(MY_CFLAGS) $(MY_LDFLAGS)

$(OUT_DIR)/pjsua2_wrap.o: $(OUT_DIR)/pjsua2_wrap.cpp Makefile
	$(PJ_CXX) -c $(OUT_DIR)/pjsua2_wrap.cpp -o $(OUT_DIR)/pjsua2_wrap.o \
		$(MY_CFLAGS)

$(OUT_DIR)/pjsua2_wrap.cpp: ../pjsua2.i ../symbols.i $(SRCS)
	mkdir -p $(MY_PACKAGE_PATH)
	mkdir -p $(OUT_DIR)
	swig $(SWIG_FLAGS) -csharp -namespace $(MY_PACKAGE_NAME) \
		-outdir $(MY_PACKAGE_PATH) \
		-o $(OUT_DIR)/pjsua2_wrap.cpp ../pjsua2.i

clean distclean realclean:
	rm -rf $(LIBPJSUA2_SO) $(OUT_DIR)/* \
		$(MY_PACKAGE_PATH)/*.cs

csharp: 
install:
uninstall:
