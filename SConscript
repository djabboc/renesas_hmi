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
group = DefineGroup('Applications', src, depend = [''], CPPPATH = CPPPATH)
network_src = Glob(os.path.join(network_root, 'core/*.c'))
network_src += Glob(os.path.join(network_root, 'core/ipv4/*.c'))
network_src += [os.path.join(network_root, 'netif/ethernet.c')]
group += DefineGroup('RW007 internet lwIP', network_src, depend=[''], CPPPATH=CPPPATH)

for d in list:
    path = os.path.join(cwd, d)
    if os.path.isfile(os.path.join(path, 'SConscript')):
        group = group + SConscript(os.path.join(d, 'SConscript'))

Return('group')
