#---------------------------------------------------------------------------------
# Non-recursive Makefile — relative paths only so Windows/MSYS paths with
# spaces or parentheses (e.g. "IB2-Switch-audio-fixed (1)") do not break make.
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO. export DEVKITPRO=/opt/devkitpro")
endif

include $(DEVKITPRO)/libnx/switch_rules

TARGET      := infinityblade2_nx
APP_TITLE   := Infinity Blade II
APP_AUTHOR  := aks796, InfinityBladeGuy
APP_VERSION := 1.0.5
APP_ICON    := resources/icon.jpg

BUILD       := build
SOURCES     := source
INCLUDES    := source

CFILES   := $(notdir $(wildcard $(SOURCES)/*.c))
CPPFILES := $(notdir $(wildcard $(SOURCES)/*.cpp))
SFILES   := $(notdir $(wildcard $(SOURCES)/*.s))

OFILES   := $(addprefix $(BUILD)/,$(CFILES:.c=.o) $(CPPFILES:.cpp=.o) $(SFILES:.s=.o))
DEPENDS  := $(OFILES:.o=.d)

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -Wextra -Werror -Wno-unused-parameter -O2 -ffunction-sections $(ARCH) \
            -D__SWITCH__ -DDEBUG_LOG=1 \
            $(foreach dir,$(INCLUDES),-I$(dir)) \
            $(foreach dir,$(LIBNX) $(PORTLIBS),-I$(dir)/include) \
            -I$(PORTLIBS)/include/SDL2 \
            -I$(BUILD)
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(TARGET).map
LIBS     := -lSDL2 -lmpg123 -lpng -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lz -lnx -lm
LIBPATHS := $(foreach dir,$(LIBNX) $(PORTLIBS),-L$(dir)/lib)

export LD := $(CXX)

NROFLAGS := --icon=$(APP_ICON) --nacp=$(TARGET).nacp

.PHONY: all clean

all: $(TARGET).nro

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	@echo building $@
	elf2nro $(TARGET).elf $@ $(NROFLAGS)
	@echo built ... $@

$(TARGET).elf: $(OFILES)
	@echo linking $@
	$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

$(TARGET).nacp:
	@echo creating $@
	nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(BUILD)/%.o: $(SOURCES)/%.c
	@mkdir -p $(BUILD)
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(BUILD)/$*.d $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SOURCES)/%.cpp
	@mkdir -p $(BUILD)
	@echo $(notdir $<)
	$(CXX) -MMD -MP -MF $(BUILD)/$*.d $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SOURCES)/%.s
	@mkdir -p $(BUILD)
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(BUILD)/$*.d $(ASFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).map

-include $(DEPENDS)
