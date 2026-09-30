# HMI USB test dependency

TinyUSB **0.17.0**, upstream tag archive:
https://github.com/hathach/tinyusb/releases/tag/0.17.0

Only `src/tusb.*`, `src/tusb_option.h`, common/device/CDC/OSAL and Renesas RUSB2 sources are vendored. Upstream files are unmodified; see `LICENSE` (MIT). Host support is disabled. Application configuration is `board/tusb_config.h`; CDC descriptors and lifecycle are in `src/test-usb.c`.

USBFS uses vector 40. No USBHS port is enabled. VID 0xCAFE / PID 0x4001 is an example/test identity, not a production USB identity allocation.

ADC/RTC dependencies elsewhere in the tree are the matching **Renesas FSP v3.5.0** files from https://github.com/renesas/fsp/tree/v3.5.0/ra/fsp (each preserves its Renesas license header):

- `src/r_adc/r_adc.c`, `inc/instances/r_adc.h`, `inc/api/r_adc_api.h`
- `src/r_rtc/r_rtc.c`, `inc/instances/r_rtc.h`, `inc/api/r_rtc_api.h`

The existing project already contains FatFs, lwIP 2.1.2 and D/AVE 2D. The suite reuses those local copies. No RT-Thread version upgrade is performed.
