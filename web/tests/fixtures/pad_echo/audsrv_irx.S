/* audsrv.irx from the pinned ps2sdk image, embedded so the ELF can load it into the IOP
 * with SifExecModuleBuffer. The SIF DMA that copies it needs a 16 byte aligned source. */
	.section .rodata
	.balign 64
	.global audsrv_irx
audsrv_irx:
	.incbin "audsrv.irx"
	.global audsrv_irx_end
audsrv_irx_end:
