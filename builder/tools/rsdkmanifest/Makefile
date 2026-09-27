# Host (macOS/Linux) build of the RSDKv4 sprite-manifest tool (PS1 port, docs/30 phase 3.1), as Sonic CD's.
# The engine's own stage loader + bytecode VM (PS1 configuration: RETRO_PS1_HOST_TOOL) run each stage's startups.
ENGINE = ../../RSDKv4
CXX ?= clang++
CXXFLAGS = -std=c++17 -O1 -g -DRETRO_PS1_HOST_TOOL -DRSDK_AUTOBUILD=1 -DRETRO_USE_MOD_LOADER=0 -DRETRO_USE_NETWORKING=0 -I$(ENGINE) \
           -I$(ENGINE)/NativeObjects -Wno-everything
SRCS = main.cpp host_stubs.cpp $(ENGINE)/Scene.cpp $(ENGINE)/Script.cpp $(ENGINE)/Object.cpp $(ENGINE)/Animation.cpp \
       $(ENGINE)/Reader.cpp $(ENGINE)/String.cpp $(ENGINE)/Math.cpp $(ENGINE)/Sprite.cpp

rsdkmanifest: $(SRCS) $(wildcard $(ENGINE)/*.hpp)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -rf rsdkmanifest rsdkmanifest.dSYM
.PHONY: clean
