clean:
rm -f ghost_loader.elf ghost_tool.elf dist/ghost_tool.elf \
      ghostwr.irx ghostwr.o ghostrd.irx ghostrd.o \
      main.o net.o ui_tex.o ui_tex_table.o ui_sky_preview.o skybox_table.o \
      ghostwr_irx.o ghostwr_irx.s ghostrd_irx.o ghostrd_irx.s \
      iomanX_irx.o  iomanX_irx.s  fileXio_irx.o fileXio_irx.s \
      usbd_irx.o    usbd_irx.s    usbhdfsd_irx.o usbhdfsd_irx.s \
      audsrv_irx.o  audsrv_irx.s  sfx_wav.o sfx_wav.s \
      ps2dev9_irx.o ps2dev9_irx.s \
      netman_irx.o  netman_irx.s \
      smap_irx.o    smap_irx.s \
      ps2ip_irx.o   ps2ip_irx.s
