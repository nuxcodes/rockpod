# Build libfaad + IMDCT as a static library for the core binary.
# Uses shim codeclib.h to bypass codec ABI dependencies.
# Included from root.make only for ipod6g.
#
# Objects go to $(BUILDDIR)/apps/faad_core/ to avoid conflicting with
# the codec build (which uses CODECFLAGS at the original paths).

FAAD_CORE_DIR  := $(RBCODECLIB_DIR)/codecs/libfaad
CODEC_LIB_DIR  := $(RBCODECLIB_DIR)/codecs/lib
FAAD_CORE_BLD  := $(BUILDDIR)/apps/faad_core
FAAD_CORE_LIB  := $(BUILDDIR)/lib/libfaad_core.a

# libfaad sources (from its own SOURCES file)
FAAD_CORE_SRC := $(call preprocess, $(FAAD_CORE_DIR)/SOURCES)

# IMDCT/FFT sources (3 files — verified complete dependency chain)
FAAD_CORE_SRC += $(CODEC_LIB_DIR)/mdct.c
FAAD_CORE_SRC += $(CODEC_LIB_DIR)/fft-ffmpeg.c
FAAD_CORE_SRC += $(CODEC_LIB_DIR)/mdct_lookup.c

# Custom object path mapping: source → $(FAAD_CORE_BLD)/basename.o
FAAD_CORE_OBJ := $(addprefix $(FAAD_CORE_BLD)/, \
    $(addsuffix .o, $(basename $(notdir $(FAAD_CORE_SRC)))))

# CFLAGS for libfaad core build:
# - Shim codeclib.h FIRST (via -I) to shadow the real one
# - No IRAM (override IBSS_ATTR etc. to empty since core IRAM is full)
# - No -DCODEC (this is core, not codec context)
# Shim -I path MUST come BEFORE base CFLAGS so GCC finds our
# codeclib.h before the real one (both "quotes" and <angle> includes).
# Also force-include faad_noiram.h to undef IRAM attrs set by config.h.
FAAD_CORE_CFLAGS := \
    -I$(ROOTDIR)/apps/faad_shim \
    -I$(CODEC_LIB_DIR) \
    -I$(FAAD_CORE_DIR) \
    $(filter-out -DCODEC,$(CFLAGS)) \
    -include $(ROOTDIR)/apps/faad_shim/faad_noiram.h

# Compile rules with -MMD for automatic header dependency tracking.
# Without this, header changes won't trigger rebuilds.
$(FAAD_CORE_BLD)/%.o: $(FAAD_CORE_DIR)/%.c
	$(SILENT)mkdir -p $(FAAD_CORE_BLD)
	$(call PRINTS,CC faad_core/$(notdir $<))$(CC) $(FAAD_CORE_CFLAGS) -MMD -c $< -o $@

$(FAAD_CORE_BLD)/%.o: $(CODEC_LIB_DIR)/%.c
	$(SILENT)mkdir -p $(FAAD_CORE_BLD)
	$(call PRINTS,CC faad_core/$(notdir $<))$(CC) $(FAAD_CORE_CFLAGS) -MMD -c $< -o $@

-include $(FAAD_CORE_OBJ:.o=.d)

$(FAAD_CORE_LIB): $(FAAD_CORE_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(notdir $@))$(AR) rcs $@ $^ >/dev/null

CORE_LIBS += $(FAAD_CORE_LIB)

# Core source files that include libfaad headers need the shim -I paths
$(BUILDDIR)/apps/video_audio.o: CFLAGS += \
    -I$(ROOTDIR)/apps/faad_shim \
    -I$(CODEC_LIB_DIR) \
    -I$(FAAD_CORE_DIR)
