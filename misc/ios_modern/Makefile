.SUFFIXES:

#---------------------------------------------------------------------------------
# Modern iOS build of ClassiCube
#
# Unlike misc/ios/Makefile (which still targets 32-bit armv7 and iOS 6.0 for
#  maximum device compatibility), this build is for modern iPhones/iPads only:
#   - arm64 only (no legacy 32-bit slice)
#   - iOS 16.0 minimum deployment target
#   - links GameController + UniformTypeIdentifiers for modern features
#   - compiled with -O2
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
# Configurable options
#---------------------------------------------------------------------------------
# Name of the final output
TARGET 			= ClassiCube
# List of directories containing source code (note: src/ios_modern, NOT src/ios)
SOURCE_DIRS		= src src/ios_modern third_party/bearssl

# CPU architecture - arm64 only on modern iOS
ifdef IOS_SIM
	ARCH	:= arm64
	SDK_TYPE:= iPhoneSimulator
else
	ARCH	:= arm64
	SDK_TYPE:= iPhoneOS
endif
FULL_TARGET	:= $(TARGET)-$(SDK_TYPE)

# override if need a specific xcode version
XCODE_APP		?= Xcode.app
# override to change iOS SDK version used to compile
IOS_SDK_VERSION	?= 17.0
# override to change minimum iOS runtime/deployment version
IOS_MIN_VERSION ?= 16.0


#---------------------------------------------------------------------------------
# Paths
#---------------------------------------------------------------------------------
XCODE_ROOT		:= /Applications/$(XCODE_APP)/Contents/Developer
IOS_SDK_PATH	:= $(XCODE_ROOT)/Platforms/$(SDK_TYPE).platform/Developer/SDKs/$(SDK_TYPE)$(IOS_SDK_VERSION).sdk

# Directories where object files are placed
BUILD_DIR_ROOT	:= build/ios_modern
BUILD_DIR_APP	:= $(BUILD_DIR_ROOT)/$(FULL_TARGET).app
BUILD_DIR_OBJ	:= $(BUILD_DIR_ROOT)/$(ARCH)

# Asset/config files are shared with the legacy iOS build
ASSETS_DIR		:= misc/ios/Assets.xcassets
PKGINFO_FILE	:= misc/ios/PkgInfo
INFO_PLIST		:= misc/ios_modern/Info.plist


#---------------------------------------------------------------------------------
# Compiler tools/paths
#---------------------------------------------------------------------------------
CC 		:= $(XCODE_ROOT)/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
LD 		:= $(XCODE_ROOT)/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
LIPO	:= $(XCODE_ROOT)/Toolchains/XcodeDefault.xctoolchain/usr/bin/lipo
ACTOOL	:= $(XCODE_ROOT)/usr/bin/actool
PLBUDDY	:= /usr/libexec/PlistBuddy
PLUTIL  := /usr/bin/plutil


#---------------------------------------------------------------------------------
# Code generation
#---------------------------------------------------------------------------------
M_FILES := $(wildcard src/ios_modern/*.m)
C_FILES := $(foreach dir,$(SOURCE_DIRS),$(wildcard $(dir)/*.c))
OBJS	:= $(addprefix $(BUILD_DIR_OBJ)/, $(notdir $(C_FILES:%.c=%.o) $(M_FILES:%.m=%.o)))

CFLAGS  := -g -O2 -fno-math-errno -fvisibility=hidden -fno-common -Wno-missing-field-initializers -Wno-missing-prototypes -Werror=return-type -Wunreachable-code -Wno-missing-braces -Wswitch -Wunused-function -Wno-unused-label -Wno-unused-parameter -Wunused-variable -Wunused-value -Wuninitialized -Wconditional-uninitialized -Wno-shadow -Wno-conversion -Wconstant-conversion -Wint-conversion -Wbool-conversion -Wenum-conversion -Wno-float-conversion -Wobjc-literal-conversion -Wshorten-64-to-32 -Wpointer-sign -isysroot $(IOS_SDK_PATH) -fstrict-aliasing -miphoneos-version-min=$(IOS_MIN_VERSION) -Wno-sign-conversion -Wcomma -Wstrict-prototypes
MFLAGS  := $(CFLAGS)

LDFLAGS  := -isysroot $(IOS_SDK_PATH) -Xlinker -rpath -Xlinker @executable_path/Frameworks
LIBS 	 := -framework Foundation -framework CoreFoundation -framework CoreGraphics -framework OpenGLES -framework UIKit -framework Security -framework CoreText -framework QuartzCore -framework GameController -framework UniformTypeIdentifiers -lobjc

# Dependency tracking
DEPFLAGS := -MT $@ -MMD -MP -MF $(BUILD_DIR_OBJ)/$*.d
DEPFILES := $(OBJS:%.o=%.d)


#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all: $(BUILD_DIR_OBJ) $(BUILD_DIR_APP) $(FULL_TARGET).ipa

$(BUILD_DIR_OBJ):
	mkdir -p $(BUILD_DIR_OBJ)

$(BUILD_DIR_APP):
	mkdir -p $(BUILD_DIR_APP)


#---------------------------------------------------------------------------------
# executable generation
#---------------------------------------------------------------------------------
$(BUILD_DIR_OBJ)/$(TARGET): $(OBJS)
	export IPHONEOS_DEPLOYMENT_TARGET=$(IOS_MIN_VERSION)
	$(LD) $^ -arch $(ARCH) $(LDFLAGS) $(LIBS) -miphoneos-version-min=$(IOS_MIN_VERSION) -dead_strip -o $@

$(BUILD_DIR_APP)/$(TARGET): $(BUILD_DIR_OBJ)/$(TARGET)
	$(LIPO) -create $^ -output $@

$(BUILD_DIR_APP)/Assets.car:
	$(ACTOOL) --output-format human-readable-text --notices --warnings --output-partial-info-plist $(BUILD_DIR_APP)/assetcatalog_generated_info.plist --app-icon AppIcon --compress-pngs --enable-on-demand-resources YES --target-device iphone --target-device ipad --minimum-deployment-target $(IOS_MIN_VERSION) --platform iphoneos --product-type com.apple.product-type.application --compile $(BUILD_DIR_APP) $(ASSETS_DIR)

$(BUILD_DIR_APP)/Info.plist: $(INFO_PLIST)
	cp $^ $@
	$(PLBUDDY) -c "add MinimumOSVersion string $(IOS_MIN_VERSION)" $@
	$(PLBUDDY) -c "add UIDeviceFamily array" $@
	$(PLBUDDY) -c "add UIDeviceFamily:0 integer 1" $@
	$(PLBUDDY) -c "add UIDeviceFamily:1 integer 2" $@
	# must be in binary format, otherwise xcrun simctl install rejects it
	$(PLUTIL) -convert binary1 $@

$(BUILD_DIR_APP)/PkgInfo: $(PKGINFO_FILE)
	cp $^ $@


$(FULL_TARGET).ipa: $(BUILD_DIR_APP)/$(TARGET) $(BUILD_DIR_APP)/Assets.car $(BUILD_DIR_APP)/Info.plist $(BUILD_DIR_APP)/PkgInfo
	rm -rf Payload
	mkdir -p Payload
	cp -R $(BUILD_DIR_APP) Payload
	mv Payload/$(FULL_TARGET).app Payload/$(TARGET).app
	zip -r $(FULL_TARGET).ipa Payload


#---------------------------------------------------------------------------------
# object generation
#---------------------------------------------------------------------------------
$(BUILD_DIR_OBJ)/%.o : src/%.c
	$(CC) -arch $(ARCH) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR_OBJ)/%.o : src/ios_modern/%.c
	$(CC) -arch $(ARCH) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR_OBJ)/%.o : src/ios_modern/%.m
	$(CC) -arch $(ARCH) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR_OBJ)/%.o : third_party/bearssl/%.c
	$(CC) -arch $(ARCH) $(CFLAGS) -c $< -o $@


#---------------------------------------------------------------------------------
# Dependency tracking
#---------------------------------------------------------------------------------
$(DEPFILES):

include $(wildcard $(DEPFILES))
