# Linux / POSIX build of the Rocket Arena 2 game module.
#
# The Windows binary comes out of rerelease/game.sln with MSVC (see README.md);
# this Makefile is the other half, and is what .github/workflows/release.yml
# uses for the Linux artifact.
#
#   make                    game_x64.so, next to this file
#   make CXXFLAGS=-O0 -j8   same, unoptimised
#   make clean
#
# The only external dependency is jsoncpp, which g_save.cpp includes as
# <json/json.h> (Debian/Ubuntu: libjsoncpp-dev). fmtlib is not needed: this
# builds as C++20 and q_std.h then formats through std::format. Override
# JSONCPP_CFLAGS / JSONCPP_LIBS for a jsoncpp that is not on the default paths.

NAME     := game_x64.so
SRCDIR   := rerelease
BUILDDIR := build

CXX      ?= c++
CXXFLAGS ?= -O2

# Not negotiable, and kept out of CXXFLAGS so that overriding that on the
# command line cannot break the build.
REQFLAGS := -std=c++20 -fPIC -fvisibility=hidden -fno-strict-aliasing -fwrapv \
            -pthread -MMD -MP

# id's flag enums do arithmetic across enum types, which C++20 deprecated;
# that alone is 780 warnings, and none of them are actionable here.
WARNFLAGS := -Wall -Wno-deprecated-enum-enum-conversion -Wno-unused-variable \
             -Wno-unused-but-set-variable -Wno-unused-function \
             -Wno-sign-compare -Wno-parentheses

# The set rerelease/game.vcxproj defines, minus the MSVC-only ones.
# USE_CPP20_FORMAT has to be explicit: q_std.h otherwise probes
# __cpp_lib_format, which libstdc++ only defines once <version> has been
# included, and would fall back to fmtlib.
DEFINES  := -DKEX_Q2_GAME -DKEX_Q2GAME_EXPORTS -DKEX_Q2GAME_DYNAMIC \
            -DNO_FMT_SOURCE -DUSE_CPP20_FORMAT=1 -DNDEBUG

JSONCPP_CFLAGS ?= $(or $(shell pkg-config --cflags jsoncpp 2>/dev/null),\
                       $(if $(wildcard /usr/include/jsoncpp/json/json.h),-I/usr/include/jsoncpp))
JSONCPP_LIBS   ?= $(or $(shell pkg-config --libs jsoncpp 2>/dev/null),-ljsoncpp)

LDFLAGS ?=

SRCS := $(wildcard $(SRCDIR)/*.cpp)              \
        $(wildcard $(SRCDIR)/bots/*.cpp)         \
        $(wildcard $(SRCDIR)/ctf/*.cpp)          \
        $(wildcard $(SRCDIR)/rocketarena2/*.cpp) \
        $(wildcard $(SRCDIR)/rogue/*.cpp)        \
        $(wildcard $(SRCDIR)/xatrix/*.cpp)
OBJS := $(SRCS:%.cpp=$(BUILDDIR)/%.o)
DEPS := $(OBJS:.o=.d)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) -shared -o $@ $(OBJS) $(LDFLAGS) $(JSONCPP_LIBS)

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(REQFLAGS) $(WARNFLAGS) $(CXXFLAGS) $(DEFINES) $(JSONCPP_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR) $(NAME)

-include $(DEPS)

.PHONY: all clean
