ifdef CONFIG_RISCV_ALTERNATIVE_EARLY
CFLAGS_errata.o := -mcmodel=medany
endif

obj-y += errata.o

ifdef CONFIG_KCSAN
KCSAN_SANITIZE_errata.o := n
endif
