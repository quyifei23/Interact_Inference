#!/usr/bin/env python3
"""Offline x86-64 ABI/metadata comparison; does not enable a runtime driver profile."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

LAYOUTS={
 'NVOS00_PARAMETERS':'hRoot hObjectParent hObjectOld status',
 'NVOS21_PARAMETERS':'hRoot hObjectParent hObjectNew hClass pAllocParms paramsSize status',
 'NVOS64_PARAMETERS':'hRoot hObjectParent hObjectNew hClass pAllocParms pRightsRequested paramsSize flags status',
 'NVOS54_PARAMETERS':'hClient hObject cmd flags params paramsSize status',
 'NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS':'hObjectError hObjectEccError hVASpace engineType bIsCallingContextVgpuPlugin',
 'NVA06C_CTRL_PREEMPT_PARAMS':'bWait bManualTimeout timeoutUs',
 'NVA06C_CTRL_MAKE_REALTIME_PARAMS':'bRealtime',
 'NVA06F_CTRL_RESTART_RUNLIST_PARAMS':'bForceRestart bBypassWait',
 'NVA06C_CTRL_TIMESLICE_PARAMS':'timesliceUs',
 'NVA06C_CTRL_GET_INFO_PARAMS':'tsgID',
 'NVA06F_CTRL_BIND_PARAMS':'engineType',
 'NV2080_CTRL_GR_GET_CTXSW_MODES_PARAMS':'hChannel zcullMode pmMode smpcMode cilpPreemptMode gfxpPreemptMode',
 'NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS':'bDisable bOnlyDisableScheduling bRewindGpPut numChannels hClientList hChannelList pRunlistPreemptEvent',
}
COMMANDS=['NVA06C_CTRL_CMD_PREEMPT','NVA06C_CTRL_CMD_MAKE_REALTIME','NVA06F_CTRL_CMD_RESTART_RUNLIST','NVA06C_CTRL_CMD_SET_TIMESLICE','NVA06C_CTRL_CMD_GET_TIMESLICE','NVA06C_CTRL_CMD_GET_INFO','NVA06C_CTRL_CMD_BIND','NVA06F_CTRL_CMD_BIND','NV2080_CTRL_CMD_GR_GET_CTXSW_MODES','NV2080_CTRL_CMD_FIFO_DISABLE_CHANNELS']
PATHS=['src/common/sdk/nvidia/inc/ctrl/ctrla06c.h','src/common/sdk/nvidia/inc/ctrl/ctrla06f/ctrla06fgpfifo.h',
       'src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gr.h','src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080fifo.h','src/common/sdk/nvidia/inc/nvos.h',
       'src/nvidia/src/kernel/rmapi/client.c','src/nvidia/src/kernel/rmapi/client_resource.c','src/nvidia/src/kernel/rmapi/resource.c','src/nvidia/src/kernel/rmapi/control.c',
       'src/nvidia/arch/nvalloc/unix/src/escape.c','src/nvidia/inc/kernel/rmapi/control.h','src/common/sdk/nvidia/inc/rs_access.h',
       'kernel-open/nvidia/os-interface.c','src/nvidia/src/kernel/gpu/fifo/kernel_channel.c','src/nvidia/src/kernel/gpu/fifo/kernel_channel_group_api.c']

def audit(source):
    source=source.resolve();commit=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    cpp=['#include <nvos.h>','#include <nv_escape.h>','#include <ctrl/ctrla06c.h>','#include <ctrl/ctrl2080/ctrl2080fifo.h>','#include <ctrl/ctrl2080/ctrl2080gr.h>','#include <cstddef>','#include <iostream>','int main(){']
    for typename,fields in LAYOUTS.items():
        cpp.append(f'std::cout << "{typename} size " << sizeof({typename}) << " align " << alignof({typename}) << "\\n";')
        for field in fields.split():cpp.append(f'std::cout << "{typename}.{field} " << offsetof({typename},{field}) << " " << sizeof((({typename}*)nullptr)->{field}) << "\\n";')
    for command in COMMANDS:cpp.append(f'std::cout << "{command} " << {command} << "\\n";')
    cpp.append('}')
    with tempfile.TemporaryDirectory(prefix='ap-abi-audit-') as tmp:
        p=Path(tmp);(p/'abi.cpp').write_text('\n'.join(cpp)+'\n')
        args=['c++','-std=c++17',str(p/'abi.cpp'),'-o',str(p/'abi')]
        for inc in ('src/common/sdk/nvidia/inc','src/common/inc','src/nvidia/arch/nvalloc/unix/include'):args+=['-I',str(source/inc)]
        subprocess.run(args,check=True,capture_output=True,text=True)
        output=subprocess.check_output([str(p/'abi')],text=True)
    layouts={};commands={}
    for line in output.splitlines():
        fields=line.split()
        if len(fields)==5:layouts[fields[0]]={'sizeof':int(fields[2]),'alignof':int(fields[4]),'members':{}}
        elif '.' in fields[0]:t,m=fields[0].split('.');layouts[t]['members'][m]={'offset':int(fields[1]),'size':int(fields[2])}
        else:commands[fields[0]]=int(fields[1])
    definitions=(source/'src/nvidia/inc/kernel/rmapi/control.h').read_text()
    flags={n:int(v,16) for n,v in re.findall(r'#define\s+(RMCTRL_FLAGS_\w+)\s+(0x[0-9a-fA-F]+)',definitions)}
    metadata={}
    for filename in ('g_kernel_channel_group_api_nvoc.c','g_kernel_channel_nvoc.c','g_subdevice_nvoc.c'):
        text=(source/'src/nvidia/generated'/filename).read_text()
        for block in re.split(r'\n    \{',text):
            match=re.search(r'/\*methodId=\*/\s*(0x[0-9a-fA-F]+)u',block)
            if not match or int(match[1],16) not in commands.values():continue
            f=re.search(r'/\*flags=\*/\s*(0x[0-9a-fA-F]+)u',block);rights=re.search(r'/\*accessRight=\*/\s*(0x[0-9a-fA-F]+)u',block)
            fn=re.search(r'/\*func=\*/\s*"([^"]+)"',block)
            if not f or not rights:raise ValueError('unparsed NVOC metadata')
            value=int(f[1],16);metadata[hex(int(match[1],16))]={'file':'src/nvidia/generated/'+filename,'function':fn[1] if fn else None,'flags':hex(value),'accessRight_mask':hex(int(rights[1],16)),
                'decoded_flags':[n for n,v in flags.items() if v and v&(v-1)==0 and value&v]}
    paths=PATHS+(['src/nvidia/kernel/vgpu/nv/rpc.c'] if (source/'src/nvidia/kernel/vgpu/nv/rpc.c').exists() else ['src/nvidia/src/kernel/vgpu/rpc.c'])
    hashes={p:hashlib.sha256((source/p).read_bytes()).hexdigest() for p in paths}
    return {'commit':commit,'host_abi':'Linux x86-64','layouts':layouts,'commands':commands,'metadata':metadata,'source_sha256':hashes}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baseline',type=Path,required=True);p.add_argument('--comparison',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    b,c=audit(a.baseline),audit(a.comparison)
    result={'baseline_550_120':b,'comparison_595_58_03':c,'layout_changes':{k:{'baseline':v,'comparison':c['layouts'][k]} for k,v in b['layouts'].items() if v!=c['layouts'][k]},
            'control_id_changes':{k:[v,c['commands'][k]] for k,v in b['commands'].items() if v!=c['commands'][k]},'runtime_profile_enabled_by_this_audit':False}
    with a.output.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
    print('layout_changes:',list(result['layout_changes']));print('control_id_changes:',result['control_id_changes'])

if __name__=='__main__':main()
