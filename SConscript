# for module compiling
import os
Import('RTT_ROOT')
Import('rtconfig')
from building import *

cwd = GetCurrentDir()
src = []
CPPPATH = []
list = os.listdir(cwd)

if rtconfig.PLATFORM in ['iccarm']:
    print("\nThe current project does not support IAR build\n")
    Return('group')
elif rtconfig.PLATFORM in ['gcc', 'armclang']:
    if GetOption('target') != 'mdk5':
        CPPPATH = [cwd]
        src = Glob('./src/*.c')

network_root = os.path.join(cwd, 'rt-thread/components/net/lwip/lwip-2.1.2/src')
CPPPATH += [os.path.join(cwd, 'board/rw007_net'), os.path.join(network_root, 'include')]
CPPPATH += [os.path.join(cwd, p) for p in ['board', 'third_party/tinyusb/src',
    'rt-thread/components/dfs/dfs_v1/filesystems/elmfat', 'ra/tes/dave2d/inc']]
group = DefineGroup('Applications', src, depend = [''], CPPPATH = CPPPATH)
network_src = Glob(os.path.join(network_root, 'core/*.c'))
network_src += Glob(os.path.join(network_root, 'core/ipv4/*.c'))
network_src += [os.path.join(network_root, 'netif/ethernet.c')]
group += DefineGroup('RW007 internet lwIP', network_src, depend=[''], CPPPATH=CPPPATH)
usb_root = os.path.join(cwd, 'third_party/tinyusb/src')
usb_src = [os.path.join(usb_root, p) for p in ['tusb.c', 'common/tusb_fifo.c',
    'device/usbd.c', 'device/usbd_control.c', 'class/cdc/cdc_device.c',
    'portable/renesas/rusb2/dcd_rusb2.c', 'portable/renesas/rusb2/rusb2_common.c']]
group += DefineGroup('USB test CDC', usb_src, depend=[''], CPPPATH=CPPPATH)

for d in list:
    path = os.path.join(cwd, d)
    if os.path.isfile(os.path.join(path, 'SConscript')):
        group = group + SConscript(os.path.join(d, 'SConscript'))

Return('group')
