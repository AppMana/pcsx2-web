EE_BIN = pad_echo.elf
EE_OBJS = main.o audsrv_irx.o
EE_LIBS = -lpad -laudsrv -lpatches
# audsrv_irx.S pulls the SDK's audsrv.irx in with .incbin, 64 byte aligned for the SIF DMA.
EE_CFLAGS += -Wa,-I$(PS2SDK)/iop/irx

include ../common/fixture.mk
