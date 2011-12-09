#
# Copyright (C) 2011 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)

# These can be overridden via the environment or by editing to
# enable/disable certain build configuration.
ART_BUILD_TARGET_NDEBUG ?= true
ART_BUILD_TARGET_DEBUG ?= true
ifeq ($(HOST_OS),linux)
  ART_BUILD_HOST_NDEBUG ?= true
  ART_BUILD_HOST_DEBUG ?= true
else
  ART_BUILD_HOST_NDEBUG ?= false
  ART_BUILD_HOST_DEBUG ?= false
endif

build_path := $(LOCAL_PATH)/build
include $(build_path)/Android.common.mk

include $(build_path)/Android.libart.mk
include $(build_path)/Android.executable.mk

include $(build_path)/Android.oat.mk

include $(build_path)/Android.libarttest.mk
include $(build_path)/Android.gtest.mk
include $(build_path)/Android.oattest.mk

# "m build-art" for quick minimal build
.PHONY: build-art
build-art: \
    $(ART_TARGET_EXECUTABLES) \
    $(ART_TARGET_TEST_EXECUTABLES) \
    $(ART_HOST_EXECUTABLES) \
    $(ART_HOST_TEST_EXECUTABLES)

# "mm test-art" to build and run all tests on host and device
.PHONY: test-art
test-art: test-art-host test-art-target
	@echo test-art PASSED

.PHONY: test-art-gtest
test-art-gtest: test-art-host test-art-target-gtest
	@echo test-art-gtest PASSED

define run-host-tests-with
  $(foreach file,$(sort $(ART_HOST_TEST_EXECUTABLES)),$(1) $(file) &&) true
endef

ART_HOST_DEPENDENCIES   := $(ART_HOST_EXECUTABLES)   $(HOST_OUT_JAVA_LIBRARIES)/core-hostdex.jar
ART_TARGET_DEPENDENCIES := $(ART_TARGET_EXECUTABLES) $(TARGET_OUT_JAVA_LIBRARIES)/core.jar

ART_HOST_TEST_DEPENDENCIES   := $(ART_HOST_DEPENDENCIES)   $(ART_TEST_OAT_FILES)
ART_TARGET_TEST_DEPENDENCIES := $(ART_TARGET_DEPENDENCIES) $(ART_TEST_OAT_FILES)

ART_TARGET_TEST_DEPENDENCIES += $(TARGET_OUT_EXECUTABLES)/oat_process $(TARGET_OUT_EXECUTABLES)/oat_processd

# Switch this to "oat_process" to run an optimized build.
OAT_PROCESS=oat_processd

########################################################################
# host test targets

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: $(ART_HOST_TEST_DEPENDENCIES) $(ART_HOST_TEST_TARGETS)
	@echo test-art-host PASSED

# "mm valgrind-art-host" to build and run all host tests under valgrind.
.PHONY: valgrind-art-host
valgrind-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,"valgrind")
	@echo valgrind-art-host PASSED

# "mm tsan-art-host" to build and run all host tests under tsan.
.PHONY: tsan-art-host
tsan-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,"tsan")
	@echo tsan-art-host PASSED

########################################################################
# target test targets

# "mm test-art-target" to build and run all target tests
.PHONY: test-art-target
test-art-target: test-art-target-gtest test-art-target-oat test-art-target-run-test
	@echo test-art-target PASSED

.PHONY: test-art-target-sync
test-art-target-sync: $(ART_TARGET_TEST_DEPENDENCIES)
	adb remount
	adb sync
	adb shell mkdir -p $(ART_TEST_DIR)

.PHONY: test-art-target-gtest
test-art-target-gtest: $(ART_TARGET_TEST_TARGETS)

.PHONY: test-art-target-oat
test-art-target-oat: $(ART_TEST_OAT_TARGETS)
	@echo test-art-target-oat PASSED

.PHONY: test-art-target-run-test
test-art-target-run-test: test-art-target-run-test-002
	@echo test-art-target-run-test PASSED

.PHONY: test-art-target-run-test-002
test-art-target-run-test-002: test-art-target-sync
	art/test/run-test 002
	@echo test-art-target-run-test-002 PASSED

########################################################################
# oat_process test targets

# $(1): jar or apk name
define art-cache-oat
  $(ART_CACHE_OUT)/$(subst /,@,$(patsubst %.apk,%.oat,$(patsubst %.jar,%.oat,$(1))))
endef

ART_CACHE_OATS :=
# $(1): name
define build-art-cache-oat
  $(call build-art-oat,$(PRODUCT_OUT)/$(1),$(call art-cache-oat,$(1)),$(TARGET_BOOT_IMG))
  ART_CACHE_OATS += $(call art-cache-oat,$(1))
endef

.PHONY: test-art-target-oat-process
test-art-target-oat-process: test-art-target-oat-process-am # test-art-target-oat-process-Calculator

$(eval $(call build-art-cache-oat,system/framework/am.jar))
$(eval $(call build-art-cache-oat,system/app/Calculator.apk))


# WORKING for zygote-art
ifeq (1,1)
$(eval $(call build-art-cache-oat,system/app/ApplicationsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/BackupRestoreConfirmation.apk))
$(eval $(call build-art-cache-oat,system/app/Bluetooth.apk))
$(eval $(call build-art-cache-oat,system/app/BooksTablet.apk))
$(eval $(call build-art-cache-oat,system/app/BrowserGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CalendarGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CalendarProvider.apk))
$(eval $(call build-art-cache-oat,system/app/CameraGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CarHomeGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/CertInstaller.apk))
$(eval $(call build-art-cache-oat,system/app/ChromeBookmarksSyncAdapter.apk))
$(eval $(call build-art-cache-oat,system/app/Contacts.apk))
$(eval $(call build-art-cache-oat,system/app/ContactsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/DefaultContainerService.apk))
$(eval $(call build-art-cache-oat,system/app/DeskClockGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Development.apk))
$(eval $(call build-art-cache-oat,system/app/DownloadProvider.apk))
$(eval $(call build-art-cache-oat,system/app/DownloadProviderUi.apk))
$(eval $(call build-art-cache-oat,system/app/DrmProvider.apk))
$(eval $(call build-art-cache-oat,system/app/EmailGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/ExchangeGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/GalleryGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/GenieWidget.apk))
$(eval $(call build-art-cache-oat,system/app/Gmail.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleBackupTransport.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleContactsSyncAdapter.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleEarth.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleFeedback.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleLoginService.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePackageVerifier.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePackageVerifierUpdater.apk))
$(eval $(call build-art-cache-oat,system/app/GooglePartnerSetup.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleQuickSearchBox.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleServicesFramework.apk))
$(eval $(call build-art-cache-oat,system/app/GoogleTTS.apk))
$(eval $(call build-art-cache-oat,system/app/HTMLViewer.apk))
$(eval $(call build-art-cache-oat,system/app/HoloSpiralWallpaper.apk))
$(eval $(call build-art-cache-oat,system/app/KeyChain.apk))
$(eval $(call build-art-cache-oat,system/app/LatinImeDictionaryPack.apk))
$(eval $(call build-art-cache-oat,system/app/LatinImeGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Launcher2.apk))
$(eval $(call build-art-cache-oat,system/app/LiveWallpapers.apk))
$(eval $(call build-art-cache-oat,system/app/LiveWallpapersPicker.apk))
$(eval $(call build-art-cache-oat,system/app/Maps.apk))
$(eval $(call build-art-cache-oat,system/app/MarketUpdater.apk))
$(eval $(call build-art-cache-oat,system/app/MediaProvider.apk))
$(eval $(call build-art-cache-oat,system/app/MediaUploader.apk))
$(eval $(call build-art-cache-oat,system/app/Microbes.apk))
$(eval $(call build-art-cache-oat,system/app/Mms.apk))
$(eval $(call build-art-cache-oat,system/app/Music2.apk))
$(eval $(call build-art-cache-oat,system/app/MusicFX.apk))
$(eval $(call build-art-cache-oat,system/app/NetSpeed.apk))
$(eval $(call build-art-cache-oat,system/app/NetworkLocation.apk))
$(eval $(call build-art-cache-oat,system/app/Nfc.apk))
$(eval $(call build-art-cache-oat,system/app/OneTimeInitializer.apk))
$(eval $(call build-art-cache-oat,system/app/PackageInstaller.apk))
$(eval $(call build-art-cache-oat,system/app/Phone.apk))
$(eval $(call build-art-cache-oat,system/app/Phonesky.apk))
$(eval $(call build-art-cache-oat,system/app/PlusOne.apk))
$(eval $(call build-art-cache-oat,system/app/Settings.apk))
$(eval $(call build-art-cache-oat,system/app/SettingsProvider.apk))
$(eval $(call build-art-cache-oat,system/app/SetupWizard.apk))
$(eval $(call build-art-cache-oat,system/app/SharedStorageBackup.apk))
$(eval $(call build-art-cache-oat,system/app/SoundRecorder.apk))
$(eval $(call build-art-cache-oat,system/app/SpeechRecorder.apk))
$(eval $(call build-art-cache-oat,system/app/StingrayProgramMenu.apk))
$(eval $(call build-art-cache-oat,system/app/StingrayProgramMenuSystem.apk))
$(eval $(call build-art-cache-oat,system/app/Street.apk))
$(eval $(call build-art-cache-oat,system/app/SystemUI.apk))
$(eval $(call build-art-cache-oat,system/app/TagGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Talk.apk))
$(eval $(call build-art-cache-oat,system/app/TelephonyProvider.apk))
$(eval $(call build-art-cache-oat,system/app/Thinkfree.apk))
$(eval $(call build-art-cache-oat,system/app/UserDictionaryProvider.apk))
$(eval $(call build-art-cache-oat,system/app/VideoEditorGoogle.apk))
$(eval $(call build-art-cache-oat,system/app/Videos.apk))
$(eval $(call build-art-cache-oat,system/app/VisualizationWallpapers.apk))
$(eval $(call build-art-cache-oat,system/app/VoiceDialer.apk))
$(eval $(call build-art-cache-oat,system/app/VoiceSearch.apk))
$(eval $(call build-art-cache-oat,system/app/VpnDialogs.apk))
$(eval $(call build-art-cache-oat,system/app/YouTube.apk))
$(eval $(call build-art-cache-oat,system/app/googlevoice.apk))
$(eval $(call build-art-cache-oat,system/app/talkback.apk))
$(eval $(call build-art-cache-oat,system/framework/android.test.runner.jar))
$(eval $(call build-art-cache-oat,system/framework/bmgr.jar))
$(eval $(call build-art-cache-oat,system/framework/bu.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.future.usb.accessory.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.location.provider.jar))
$(eval $(call build-art-cache-oat,system/framework/com.android.nfc_extras.jar))
$(eval $(call build-art-cache-oat,system/framework/com.google.android.maps.jar))
$(eval $(call build-art-cache-oat,system/framework/com.google.android.media.effects.jar))
$(eval $(call build-art-cache-oat,system/framework/ext.jar))
$(eval $(call build-art-cache-oat,system/framework/ime.jar))
$(eval $(call build-art-cache-oat,system/framework/input.jar))
$(eval $(call build-art-cache-oat,system/framework/javax.obex.jar))
$(eval $(call build-art-cache-oat,system/framework/monkey.jar))
$(eval $(call build-art-cache-oat,system/framework/pm.jar))
$(eval $(call build-art-cache-oat,system/framework/send_bug.jar))
$(eval $(call build-art-cache-oat,system/framework/svc.jar))
endif

.PHONY: test-art-target-oat-process-am
test-art-target-oat-process-am: $(call art-cache-oat,system/framework/am.jar) test-art-target-sync
	adb remount
	adb sync
	adb shell sh -c "export CLASSPATH=/system/framework/am.jar && $(OAT_PROCESS) /system/bin/app_process /system/bin com.android.commands.am.Am start http://android.com && touch $(ART_TEST_DIR)/test-art-target-process-am"
	$(hide) (adb pull $(ART_TEST_DIR)/test-art-target-process-am /tmp/ && echo test-art-target-process-am PASSED) || echo test-art-target-process-am FAILED
	$(hide) rm /tmp/test-art-target-process-am

.PHONY: test-art-target-oat-process-Calculator
# Note that using this instead of "adb shell am start" make sure that the /data/art-cache is up-to-date
test-art-target-oat-process-Calculator: $(call art-cache-oat,system/app/Calculator.oat) $(call art-cache-oat,system/framework/am.jar) test-art-target-sync
	mkdir -p $(ART_CACHE_OUT)
	unzip $(TARGET_OUT_APPS)/Calculator.apk classes.dex -d $(TARGET_OUT_DATA)/art-cache
	mv $(TARGET_OUT_DATA)/art-cache/classes.dex $(ART_CACHE_OUT)/system@app@Calculator.apk@classes.dex.`unzip -lv $(TARGET_OUT_APPS)/Calculator.apk classes.dex | grep classes.dex | sed -E 's/.* ([0-9a-f]+)  classes.dex/\1/'` # note this is extracting the crc32 that is needed as the file extension
	adb remount
	adb sync
	if [ "`adb shell getprop wrap.com.android.calculator2 | tr -d '\r'`" = "$(OAT_PROCESS)" ]; then \
	  echo wrap.com.android.calculator2 already set; \
	  adb shell start; \
	else \
	  echo Setting wrap.com.android.calculator2 and restarting runtime; \
	  adb shell setprop wrap.com.android.calculator2 "$(OAT_PROCESS)"; \
	  adb shell stop; \
	  adb shell start; \
	  sleep 30; \
	fi
	adb shell kill `adb shell ps | fgrep com.android.calculator2 | sed -e 's/[^ ]* *\([0-9]*\).*/\1/'`
	adb shell sh -c "export CLASSPATH=/system/framework/am.jar && $(OAT_PROCESS) /system/bin/app_process /system/bin com.android.commands.am.Am start -a android.intent.action.MAIN -n com.android.calculator2/.Calculator && touch $(ART_TEST_DIR)/test-art-target-process-Calculator"
	$(hide) (adb pull $(ART_TEST_DIR)/test-art-target-process-Calculator /tmp/ && echo test-art-target-process-Calculator PASSED) || echo test-art-target-process-Calculator FAILED
	$(hide) rm /tmp/test-art-target-process-Calculator

########################################################################
# zygote targets
#
# zygote-artd will change to use art to boot the device with a debug build
# zygote-art will change to use art to boot the device with a production build
# zygote-dalvik will restore to booting with dalvik
#
# zygote-artd-target-sync will just push a new artd in place of dvm
# zygote-art-target-sync will just push a new art in place of dvm

# $(1): "d" for debug build, "" for ndebug build
define define-zygote-art-targets
.PHONY: zygote-art$(1)-target-sync
zygote-art$(1)-target-sync: $(ART_TARGET_DEPENDENCIES) $(TARGET_BOOT_OAT) $(ART_CACHE_OATS)
	cp $(TARGET_OUT_SHARED_LIBRARIES)/libart$(1).so $(TARGET_OUT_SHARED_LIBRARIES)/libdvm.so
	cp $(TARGET_OUT_SHARED_LIBRARIES_UNSTRIPPED)/libart$(1).so $(TARGET_OUT_SHARED_LIBRARIES_UNSTRIPPED)/libdvm.so
	cp $(TARGET_OUT_EXECUTABLES)/oatopt$(1) $(TARGET_OUT_EXECUTABLES)/dexopt
	cp $(TARGET_OUT_EXECUTABLES_UNSTRIPPED)/oatopt$(1) $(TARGET_OUT_EXECUTABLES_UNSTRIPPED)/dexopt
	mkdir -p $(TARGET_OUT_DATA)/property
	echo -n 1 > $(TARGET_OUT_DATA)/property/persist.sys.strictmode.disable
	adb remount
	adb sync

.PHONY: zygote-art$(1)
zygote-art$(1): zygote-art$(1)-target-sync
	sed -e 's/--start-system-server/--start-system-server --no-preload/' -e 's/art-cache 0771/art-cache 0777/' < system/core/rootdir/init.rc > $(ANDROID_PRODUCT_OUT)/root/init.rc
	adb shell rm -f $(ART_CACHE_DIR)
	rm -f $(ANDROID_PRODUCT_OUT)/boot.img
	unset ONE_SHOT_MAKEFILE && $(MAKE) showcommands bootimage
	adb reboot bootloader
	fastboot flash boot $(ANDROID_PRODUCT_OUT)/boot.img
	fastboot reboot
endef

$(eval $(call define-zygote-art-targets,d))
$(eval $(call define-zygote-art-targets,))

.PHONY: zygote-dalvik
zygote-dalvik:
	cp $(TARGET_OUT_INTERMEDIATE_LIBRARIES)/libdvm.so $(TARGET_OUT_SHARED_LIBRARIES)/libdvm.so
	cp $(call intermediates-dir-for,SHARED_LIBRARIES,libdvm)/LINKED/libdvm.so $(TARGET_OUT_SHARED_LIBRARIES_UNSTRIPPED)/libdvm.so
	cp $(call intermediates-dir-for,EXECUTABLES,dexopt)/dexopt $(TARGET_OUT_EXECUTABLES)/dexopt
	cp $(call intermediates-dir-for,EXECUTABLES,dexopt)/LINKED/dexopt $(TARGET_OUT_EXECUTABLES_UNSTRIPPED)/dexopt
	rm -f $(TARGET_OUT_DATA)/property/persist.sys.strictmode.disable
	adb shell rm /data/property/persist.sys.strictmode.disable
	adb remount
	adb sync
	cp system/core/rootdir/init.rc $(ANDROID_PRODUCT_OUT)/root/init.rc
	rm -f $(ANDROID_PRODUCT_OUT)/boot.img
	unset ONE_SHOT_MAKEFILE && $(MAKE) showcommands bootimage
	adb reboot bootloader
	fastboot flash boot $(ANDROID_PRODUCT_OUT)/boot.img
	fastboot reboot

########################################################################
# oatdump targets

.PHONY: dump-oat
dump-oat: dump-oat-core dump-oat-boot dump-oat-Calculator

.PHONY: dump-oat-core
dump-oat-core: $(TARGET_CORE_OAT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_CORE_IMG) --host-prefix=$(PRODUCT_OUT) --output=/tmp/core.oatdump.txt
	@echo Output in /tmp/core.oatdump.txt

.PHONY: dump-oat-boot
dump-oat-boot: $(TARGET_BOOT_OAT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_BOOT_IMG) --host-prefix=$(PRODUCT_OUT) --output=/tmp/boot.oatdump.txt
	@echo Output in /tmp/boot.oatdump.txt

.PHONY: dump-oat-Calculator
dump-oat-Calculator: $(call art-cache-oat,system/app/Calculator.oat) $(TARGET_BOOT_OAT) $(OATDUMP)
	$(OATDUMP) --oat=$< --boot-image=$(TARGET_BOOT_IMG) --host-prefix=$(PRODUCT_OUT) --output=/tmp/Calculator.oatdump.txt
	@echo Output in /tmp/Calculator.oatdump.txt


########################################################################
# clean-oat target
#

.PHONY: clean-oat
clean-oat:
	rm -f $(ART_TEST_OUT)/*.oat
	rm -f $(ART_TEST_OUT)/*.art
	rm -f $(ART_CACHE_OUT)/*.oat
	rm -f $(ART_CACHE_OUT)/*.art
	adb shell rm $(ART_TEST_DIR)/*.oat
	adb shell rm $(ART_TEST_DIR)/*.art
	adb shell rm $(ART_CACHE_DIR)/*.oat
	adb shell rm $(ART_CACHE_DIR)/*.art

########################################################################
# cpplint target

# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	./art/tools/cpplint.py \
	    --filter=-whitespace/comments,-whitespace/line_length,-build/include,-build/header_guard,-readability/streams,-readability/todo,-runtime/references \
	    $(ANDROID_BUILD_TOP)/art/src/*.h $(ANDROID_BUILD_TOP)/art/src/*.cc

########################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
