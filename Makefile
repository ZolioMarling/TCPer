.PHONY: clean asan tsan

CC := gcc
TARGET := tcper

RWILDCARD = $(foreach d,$(wildcard $1*),$(call RWILDCARD,$d/,$2) $(filter $(subst *,%,$2),$d))
SOURCES := $(call RWILDCARD, ./, %.c)
BASE_OBJECTS := $(SOURCES:.c=.o)

COMMON_FLAGS := -std=c11 -pedantic-errors -Wall -Wextra -Werror -pipe -fPIC
RELEASE_FLAGS := -O3 -pipe -fPIC

ifeq ($(OS),Windows_NT)
TARGET := $(TARGET).exe
RELEASE_FLAGS := -s -flto ${RELEASE_FLAGS}
LINKER_LIBRARIES := -lws2_32 -ladvapi32 -lm
REMOVE = cmd /c del /q
else
TARGET := $(TARGET)
RELEASE_FLAGS := -flto=auto ${RELEASE_FLAGS}
LINKER_LIBRARIES := -pthread -lm
REMOVE = rm -f
endif

SANITIZER := $(filter asan tsan,$(MAKECMDGOALS))

ifeq ($(SANITIZER),asan)
MODE := .asan
BUILD_FLAGS := -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
else ifeq ($(SANITIZER),tsan)
MODE := .tsan
BUILD_FLAGS := -g -O1 -fsanitize=thread,undefined -fno-omit-frame-pointer
else
BUILD_FLAGS := $(RELEASE_FLAGS)
endif

OBJECTS := $(patsubst %.o,%$(MODE).o,$(BASE_OBJECTS))
DEPENDENCIES := $(OBJECTS:.o=.d)
ALL_OBJECTS := $(BASE_OBJECTS) $(patsubst %.o,%.asan.o,$(BASE_OBJECTS)) $(patsubst %.o,%.tsan.o,$(BASE_OBJECTS))
ALL_DEPENDENCIES := $(ALL_OBJECTS:.o=.d)
MODE_STAMP := .release-build-mode$(MODE).d
ALL_MODE_STAMPS := .release-build-mode.d .release-build-mode.asan.d .release-build-mode.tsan.d

ifeq ($(wildcard $(MODE_STAMP)),)
.PHONY: $(TARGET)
endif

$(TARGET): $(MODE_STAMP) $(OBJECTS)
	$(CC) $(COMMON_FLAGS) $(BUILD_FLAGS) $(OBJECTS) -o $@ $(LINKER_LIBRARIES)

asan tsan: $(TARGET)

$(MODE_STAMP):
	@$(REMOVE) $(filter-out $@,$(ALL_MODE_STAMPS))
	@echo $(if $(MODE),$(MODE),) > $@

%$(MODE).o: %.c Makefile
	$(CC) $(COMMON_FLAGS) $(BUILD_FLAGS) -MMD -MP -c $< -o $@

-include $(DEPENDENCIES)

clean:
	-$(REMOVE) $(ALL_OBJECTS)
	-$(REMOVE) $(ALL_DEPENDENCIES)
	-$(REMOVE) $(ALL_MODE_STAMPS)
	-$(REMOVE) $(TARGET)
