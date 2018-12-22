Phoenix Gradle Build
====================

Implements a Gradle build based on the existing phoenix sources.

It is currently only useful for running and debugging the RetroArch frontend in Android Studio.
This is caused by the fact that this build can't support the same older API level that the old Ant 
based build does. The minimum supported API level for this build is 16. Also this will not build the 
mips variant cause support for this architecture has long been removed from the Android NDK.
The only file that had to be duplicated is the AndroidManifest.xml because the modern Android build
won't allow SDK versions defined in this file anymore. It's also easier to change the app name this way.

To get this running follow these steps:

* Install the Android SDK Platform 28
* Install the latest Android NDK
* Import the project into Android Studio
* Make sure to select the appropriate build variant for your device (32 or 64 bit)

Debugging a core
----------------

This is currently only tested for the dosbox-svn core as it can be compiled with the newest ndk. The instructions need to be adapted for other cores.

* Clone dosbox-svn next to your RetroArch repo
* Run `ndk-build NDK_DEBUG=1` in dosbox-svn/libretro/jni
* Copy the libretro.so file for your architecture from dosbox-svn/libretro/libs to /path/to/phoenix-gradle/app/src/main/jniLibs/<your_arch>/libretro.so
* Set a breakpoint in the core code (make sure you refreshed the Gradle project in Android Studio so the code is added to the project)
    * A good example to check if the setup is working is `retro_get_system_info` which is called on core load
* In the Run/Debug Configurations dialog in Android Studio under Debugger -> Symbol Directories make sure to have your core symbols loaded. You should have two directories there (as per your architecture):
    * /path/to/phoenix-gradle/app/build/intermediates/ndkBuild/ra<32 or 64>/debug/obj/local/<your_arch>
    * /path/to/dosbox-svn/libretro/obj/local/<your_arch>
* Start the app with the debug config from Android Studio
* Load the core. The breakpoint should trigger