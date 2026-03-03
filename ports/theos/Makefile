include ../../py/mkenv.mk

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h

# Keep compressed error strings to reduce binary size.
MICROPY_ROM_TEXT_COMPRESSION ?= 1

# include py core make definitions
include $(TOP)/py/py.mk

THEOS_LIBC_INC ?= $(TOP)/../../Libraries/LibC/Includes
THEOS_USERLIBC ?=

INC += -I.
INC += -I$(TOP)
INC += -I$(BUILD)
INC += -I$(THEOS_LIBC_INC)

CWARN = -Wall -Wextra -Wno-unused-parameter
CFLAGS += $(INC) $(CWARN) -std=c99 $(COPT)
CFLAGS += -fno-pie -ffreestanding -nostdlib -nostdinc -static

# Tune for size but keep symbols for debugging.
CSUPEROPT = -Os
CFLAGS += -g
ifeq ($(DEBUG),1)
CFLAGS += -O0
else
CFLAGS += -Os -DNDEBUG
CFLAGS += -fdata-sections -ffunction-sections
endif

LDFLAGS += -nostdlib -static -n -no-pie
LDFLAGS += -Wl,-Map=$@.map,--cref -Wl,--gc-sections
LDFLAGS += -Wl,-T,$(CURDIR)/linker.ld

LIB += $(THEOS_USERLIBC)

SRC_C = \
	main.c \
	theos_mphal.c \
	shared/readline/readline.c \
	shared/runtime/pyexec.c \
	shared/runtime/interrupt_char.c \
	shared/runtime/stdout_helpers.c \

SRC_S = \
	crt0.S \

SRC_QSTR += main.c shared/readline/readline.c shared/runtime/pyexec.c

OBJ += $(PY_CORE_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_S:.S=.o))

PROG = TheMicroPython

include $(TOP)/py/mkrules.mk
