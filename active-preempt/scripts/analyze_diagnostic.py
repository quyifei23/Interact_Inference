#!/usr/bin/env python3
"""Nsight 2024.6 / SQLite 3.16.1 diagnostic only. Never infer residency or causality.

Schema was inspected in the real Phase7 D0 export before this adapter was added.
Keep the original report/SQLite locally; every derived event retains its rowid.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import sqlite3
from run_paired import control_facts,validate_run

DOMAIN='nsys_session_ns'
REQUIRED={
    'META_DATA_EXPORT':{'name','value'},
    'GPU_CONTEXT_SWITCH_EVENTS':{'tag','vmId','seqNo','contextId','timestamp','globalPid','gpuId'},
    'ENUM_GPU_CTX_SWITCH':{'id','name','label'},
    'TARGET_INFO_GPU':{'id','uuid','vmId'},
    'TARGET_INFO_CUDA_CONTEXT_INFO':{'contextId','hwId','processId','deviceId'},
    'NVTX_EVENTS':{'start','end','text','textId','globalTid'},
    'CUPTI_ACTIVITY_KIND_RUNTIME':{'start','end','globalTid','correlationId','nameId'},
    'CUPTI_ACTIVITY_KIND_KERNEL':{'start','end','deviceId','contextId','correlationId','globalPid','demangledName'},
    'StringIds':{'id','value'},
    'DIAGNOSTIC_EVENT':{'timestamp','text','severity','globalPid'},
    'ENUM_DIAGNOSTIC_SEVERITY_LEVEL':{'id','name'},
}

def write(path,obj):path.write_text(json.dumps(obj,indent=2)+'\n')
def pid(global_id):
    # NVIDIA 2024.6 User Guide, SQLite GlobalId/PID example. Not a TSG ID.
    return (int(global_id)//0x1000000)%0x1000000 if global_id is not None and int(global_id)>=0 else None
def uuid_hex(text):return str(text).removeprefix('GPU-').replace('-','').lower()
def same_clock_delta(a,domain_a,b,domain_b):
    if domain_a!=domain_b:raise ValueError('Uncalibrated clock domains; ordering_ambiguous')
    return int(a)-int(b) # subtract integers before display conversions
def quote(name):return '"'+name.replace('"','""')+'"'

def inspect_schema(c):
    result={}
    for row in c.execute("select name,sql from sqlite_master where type='table'").fetchall():
        name=row['name'];result[name]={'sql':row['sql'],'columns':[dict(r) for r in c.execute('pragma table_info('+quote(name)+')')],
                                    'count':c.execute('select count(*) from '+quote(name)).fetchone()[0]}
    return result

def reviewed_schema(c,schema):
    missing={t:sorted(fields-{x['name'] for x in schema.get(t,{}).get('columns',[])}) for t,fields in REQUIRED.items()}
    missing={k:v for k,v in missing.items() if v}
    if missing:return False,{'missing_fields':missing}
    meta=dict(c.execute('select name,value from META_DATA_EXPORT'))
    return (meta.get('EXPORT_PRODUCT_VERSION')=='2024.6.2.225' and meta.get('EXPORT_SCHEMA_VERSION')=='3.16.1'),meta

def associate_switch(record,identities,trial_contexts,gpu_id):
    """Scoped association, not a numeric equality between profiler and RM IDs."""
    owner=next((r for r,v in identities.items() if pid(record['globalPid'])==v['pid']),None)
    if record['contextId']==0 or pid(record['globalPid']) in (None,0):return None,'anonymous_or_unknown'
    if owner is None or record['gpuId']!=gpu_id:return None,'outside_this_run_scope'
    window=identities[owner].get('switch_association_window_ns')
    if window and not window[0]<=record['timestamp']<=window[1]:return owner,'outside_workload_association_window'
    if not identities[owner]['scoped_association_valid']:return owner,'context_or_binding_ambiguous'
    if len(trial_contexts.get(owner,set()))!=1:return owner,'multiple_switch_contexts_in_trial'
    if record['contextId'] not in trial_contexts[owner]:return owner,'outside_trial_context_lifecycle'
    return owner,'current_PID_GPU_trial_workload_context_and_unique_compute_group; scoped_inference_not_ID_equivalence'

def log_limits(diagnostics,severity_names):
    severe=[d for d in diagnostics if severity_names.get(d['severity'])=='Error']
    lost=[d for d in diagnostics if any(s in d['text'].lower() for s in ('dropped','overflow','truncat','lost records','data loss'))]
    return severe,lost

def select_marker(nvtx,role,name,expected_pid):
    choices=[v for v in nvtx if v['label'].split('/')[3]==role and v['label'].endswith('/'+name)]
    if len(choices)!=1:raise ValueError('Missing/ambiguous '+role+' '+name+' marker (parent-only trace is insufficient)')
    if pid(choices[0]['globalTid'])!=expected_pid:raise ValueError('Marker PID mismatch')
    return choices[0]

def analyze(directory,output):
    output.mkdir(parents=True,exist_ok=False)
    assessment={'run_kind':'diagnostic','backend_ready_for_d1':False,'status':'OBSERVABILITY_LIMITED',
                'ordering_relative_to_actual_rm':'ordering_ambiguous',
                'exact_preempt_completion':'unmeasured','context_save_duration':'unmeasured','BG_resume_latency':'unmeasured',
                'dropped_records':None,'drop_count_reason':'No authoritative dropped-record count exported; not assumed zero'}
    events=[];identities={};problems=[]
    def event(source,rowid,kind,timestamp,domain=DOMAIN,role=None,context=None,method=None,interpretation='',uncertainty='',column='timestamp'):
        i=identities.get(role,{})
        events.append(dict(run_id=directory.name,run_kind='diagnostic',source=source,source_record_id=rowid,source_column=column,
            raw_event_type=kind,raw_timestamp=timestamp,clock_domain=domain,
            normalized_timestamp=int(timestamp) if domain==DOMAIN and timestamp is not None else None,
            gpu_uuid=i.get('gpu_uuid'),pid=i.get('pid'),profiler_context_id=context,associated_role=role,
            rm_group=i.get('group_handle') if method and 'scoped_inference' in method else None,
            hardware_tsg_id=i.get('hardware_tsg_id') if method and 'scoped_inference' in method else None,
            association_method=method,interpretation=interpretation,uncertainty=uncertainty))
    try:
        db=directory/'trace.sqlite'
        if not db.is_file():
            log=(directory/'collection.log').read_text() if (directory/'collection.log').exists() else ''
            assessment['status']='permission_denied' if 'permission' in log.lower() else 'export_unavailable'
            raise ValueError('No SQLite export; no event availability inferred from tool exit')
        c=sqlite3.connect(db.resolve().as_uri()+'?mode=ro',uri=True);c.row_factory=sqlite3.Row
        schema=inspect_schema(c);write(output/'actual_schema.json',schema)
        valid,meta=reviewed_schema(c,schema);write(output/'export_metadata.json',meta)
        if not valid:assessment['status']='unknown_export_schema';raise ValueError('Adapter schema/version mismatch')
        execution=json.loads((directory/'execution.json').read_text())
        if execution['returncode']!=0 or execution['timed_out'] or execution['termination']:
            problems.append('Collection failed/timed out; owner cleanup cannot be inferred from report generation')
        pair=json.loads((directory/'worker/group_pair.json').read_text())
        gpu=[dict(x) for x in c.execute('select * from TARGET_INFO_GPU') if uuid_hex(x['uuid'])==pair['gpu_uuid']]
        if len(gpu)!=1:raise ValueError('GPU UUID mapping missing/ambiguous')
        gpu_id=gpu[0]['id'];assessment['target_gpu']=gpu[0]
        assessment['trace_origin']=[dict(x) for x in c.execute('select * from TARGET_INFO_SESSION_START_TIME')] if 'TARGET_INFO_SESSION_START_TIME' in schema else None
        assessment['trace_span']=[dict(x) for x in c.execute('select * from ANALYSIS_DETAILS')] if 'ANALYSIS_DETAILS' in schema else None
        assessment['clock_contract']='Session-relative exported ns with TIME_NORMALIZE=false/TIME_SHIFT=0; origin retained. RAW/globaltimer are NOT converted.'
        if meta.get('EXPORT_PARAM_TIME_NORMALIZE')!='false' or meta.get('EXPORT_PARAM_TIME_SHIFT')!='0':raise ValueError('Unexpected timestamp export mode')
        strings=dict(c.execute('select id,value from StringIds'))
        nvtx=[dict(x) for x in c.execute('select rowid,* from NVTX_EVENTS')]
        for v in nvtx:v['label']=v['text'] or strings.get(v['textId'],'')
        nvtx=[v for v in nvtx if v['label'].startswith('AP/phase7/'+directory.name+'/')]
        def mark(role,name):
            return select_marker(nvtx,role,name,pair[role.lower()+'_pid'])
        trial=mark('INT','trial');interaction=mark('INT','interaction')
        if not trial['end'] or trial['end']<=trial['start']:raise ValueError('Truncated trial range')
        assessment['trial_window_ns']=[trial['start'],trial['end']];assessment['interaction_trace_ns']=interaction['start']
        kernels=[dict(k) for k in c.execute('select rowid,* from CUPTI_ACTIVITY_KIND_KERNEL') if pid(k['globalPid']) in (pair['bg_pid'],pair['int_pid'])]
        for k in kernels:k['name']=strings.get(k['demangledName'],'unknown')
        ctxinfo=[dict(r) for r in c.execute('select rowid,* from TARGET_INFO_CUDA_CONTEXT_INFO') if r['processId'] in (pair['bg_pid'],pair['int_pid'])]
        raw_switches=[dict(r) for r in c.execute('select rowid,* from GPU_CONTEXT_SWITCH_EVENTS order by timestamp')]
        switches=[r for r in raw_switches if pid(r['globalPid']) in (pair['bg_pid'],pair['int_pid']) and r['gpuId']==gpu_id]
        trial_switches=[r for r in switches if trial['start']<=r['timestamp']<=trial['end']]
        assessment['switch_records_total']=len(raw_switches);assessment['own_switch_records']=len(switches);assessment['trial_switch_records']=len(trial_switches)
        if not switches:assessment['status']='zero_events';raise ValueError('No target-owner context-switch records')
        workload={};submissions={};selected_api=[]
        for role,submit_name in [('BG','bg_submit'),('INT','int_submit')]:
            owner_pid=pair[role.lower()+'_pid'];submit=mark(role,submit_name)
            submissions[role]=submit
            api=[dict(a) for a in c.execute('select rowid,* from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?',(submit['start'],submit['end'])) if pid(a['globalTid'])==owner_pid]
            selected_api.extend(api)
            correlations={a['correlationId'] for a in api}
            matched=[k for k in kernels if pid(k['globalPid'])==owner_pid and k['correlationId'] in correlations and k['name'].startswith('ap::arithmetic(')]
            ownctx=[r for r in ctxinfo if r['processId']==owner_pid and r['deviceId']==gpu_id]
            binding=json.loads((directory/'worker'/f'{role.lower()}_group_identity.json').read_text())
            state=json.loads((directory/'worker'/f'{role.lower()}_profile_state.json').read_text())
            cuda_address=re.search(r'\bcontext=(\d+)',(directory/'worker'/f'{role.lower()}_identity.txt').read_text())
            good=len(matched)==1 and len(ownctx)==1 and matched[0]['contextId']==ownctx[0]['contextId'] and binding['owner_pid']==owner_pid and binding['gpu_uuid_hex']==pair['gpu_uuid'] and state['group_get_info_verified']
            identities[role]={'pid':owner_pid,'gpu_uuid':pair['gpu_uuid'],'group_handle':binding['group']['handle'],
                'group_generation':binding['group']['generation'],'hardware_tsg_id':pair[role.lower()+'_hardware_tsg_id'],
                'captured_members':len(binding['members']),'CUDA_context_records':ownctx,
                'CUDA_workload_context_id':matched[0]['contextId'] if len(matched)==1 else None,
                'CUcontext_address_in_owner':int(cuda_address[1]) if cuda_address else None,
                'scoped_association_valid':good,
                'method':'NVTX submit range -> same-PID CUDA API correlationId -> arithmetic activity context; same-PID/same-GPU current unique compute-group GET_INFO',
                'limitation':'No numerical equivalence of CUDA context ID, switch context ID, RM handle or TSG ID. Context-info hwId is recorded literally, not used as a guessed join. Not all CUDA resources.'}
            if not good:problems.append('Missing '+role+' kernel or ambiguous CUDA context/group mapping')
            else:workload[role]=matched[0]
            for a in api:
                event('CUPTI_ACTIVITY_KIND_RUNTIME',a['rowid'],strings.get(a['nameId'],'unknown')+'_begin',a['start'],role=role,column='start',interpretation='host CUDA API call in submission annotation')
        write(output/'identities.json',identities)
        for v in nvtx:
            role=v['label'].split('/')[3]
            if role not in identities:continue
            if pid(v['globalTid'])!=identities[role]['pid']:
                problems.append('Annotation from unexpected PID');continue
            event('NVTX_EVENTS',v['rowid'],v['label']+'_begin',v['start'],role=role,column='start',interpretation='host annotation; PREEMPT envelope is not pure hardware time')
            if v['end'] is not None:event('NVTX_EVENTS',v['rowid'],v['label']+'_end',v['end'],role=role,column='end')
        # Initialization may still be switching out when BG starts. Only the
        # owner's submit->trial-end window is eligible for its workload mapping;
        # earlier switch IDs remain in events.csv, explicitly unassociated.
        trial_contexts={role:{r['contextId'] for r in trial_switches if pid(r['globalPid'])==v['pid'] and r['timestamp']>=submissions[role]['start']} for role,v in identities.items()}
        enums={r['id']:dict(r) for r in c.execute('select * from ENUM_GPU_CTX_SWITCH')};write(output/'switch_enum.json',enums)
        for role,ids in trial_contexts.items():
            identities[role]['trial_switch_context_ids']=sorted(ids)
            identities[role]['switch_association_window_ns']=[submissions[role]['start'],trial['end']]
            if len(ids)!=1:problems.append(role+' has missing/multiple switch context IDs in trial')
        for r in switches:
            role,method=associate_switch(r,identities,trial_contexts,gpu_id)
            tag=enums.get(r['tag'],{}).get('name','UNKNOWN_TAG_'+str(r['tag']))
            event('GPU_CONTEXT_SWITCH_EVENTS',r['rowid'],tag,r['timestamp'],role=role,context=r['contextId'],method=method,
                  interpretation='raw Nsight switch operation; no SM residency or register-save duration inferred',
                  uncertainty='scoped process/workload association; native CUDA<->switch hardware-ID mapping unavailable')
            if tag.startswith('UNKNOWN') or tag in ('INVALID_TIMESTAMP','ENGINE_RESET'):problems.append('Invalid/unreviewed switch operation '+tag)
        write(output/'identities.json',identities)
        for k in kernels:
            role=next(role for role,v in identities.items() if v['pid']==pid(k['globalPid']))
            for endpoint in ('start','end'):
                event('CUPTI_ACTIVITY_KIND_KERNEL',k['rowid'],k['name']+'_'+endpoint,k[endpoint],role=role,context=k['contextId'],column=endpoint,
                      interpretation='kernel lifetime endpoint; not continuous SM occupancy')
        severity=dict(c.execute('select id,name from ENUM_DIAGNOSTIC_SEVERITY_LEVEL'))
        diagnostics=[dict(d) for d in c.execute('select rowid,* from DIAGNOSTIC_EVENT') if pid(d['globalPid']) in (pair['bg_pid'],pair['int_pid'],None,0)]
        write(output/'tool_diagnostics.json',diagnostics);errors,loss=log_limits(diagnostics,severity)
        # Shareable raw-row subset: exact field values and original rowids,
        # without opaque reports' inherited environment or unrelated processes.
        # Original report/SQLite remain unchanged and locally retained.
        subset={'GPU_CONTEXT_SWITCH_EVENTS':switches,'CUPTI_ACTIVITY_KIND_KERNEL':kernels,
                'CUPTI_ACTIVITY_KIND_RUNTIME':selected_api,'NVTX_EVENTS':nvtx,
                'TARGET_INFO_CUDA_CONTEXT_INFO':ctxinfo,'DIAGNOSTIC_EVENT':diagnostics}
        for table in ('ENUM_GPU_CTX_SWITCH','ENUM_DIAGNOSTIC_SEVERITY_LEVEL','META_DATA_EXPORT','TARGET_INFO_SESSION_START_TIME','ANALYSIS_DETAILS'):
            if table in schema:subset[table]=[dict(r) for r in c.execute('select rowid,* from '+quote(table))]
        subset['TARGET_INFO_GPU']=[dict(r) for r in c.execute('select rowid,* from TARGET_INFO_GPU where id=?',(gpu_id,))]
        ids={a['nameId'] for a in selected_api}
        ids.update(k[key] for k in kernels for key in ('demangledName','shortName','mangledName'))
        ids.update(v['textId'] for v in nvtx if v['textId'] is not None)
        subset['StringIds']=[dict(r) for r in c.execute('select rowid,* from StringIds') if r['id'] in ids]
        with (output/'raw_records.jsonl').open('w') as f:
            for table,records in subset.items():
                fields={r['name'] for r in schema[table]['columns']}
                for record in records:
                    # Remove analyzer-added convenience fields (label/name).
                    data={k:v for k,v in record.items() if k in fields}
                    f.write(json.dumps({'source':'trace.sqlite','table':table,'rowid':record.get('rowid'),'data':data})+'\n')
        if errors or loss:problems.append('Tool error/loss/truncation reported')
        assessment['tool_warnings']=[d for d in diagnostics if severity.get(d['severity'])=='Warning']
        assessment['known_loss_records']=loss
        assessment['effective_timestamp_precision_ns']=None
        if switches:
            seq=sorted(r['seqNo'] for r in raw_switches if r['gpuId']==gpu_id)
            assessment['observed_seq_gaps']=[(a,b) for a,b in zip(seq,seq[1:]) if b!=a+1]
            if assessment['observed_seq_gaps']:problems.append('Context-switch sequence gap')
        facts=control_facts(directory/'worker');assessment['project_controls']={k:v for k,v in facts.items() if k not in ('events','owners')}
        mode=json.loads((directory/'worker/configuration.json').read_text())['mode']
        row=validate_run(directory/'worker',mode,facts);assessment['worker_complete_correct_cleanup']=True
        if row['run_kind']!='diagnostic':raise ValueError('Performance samples cannot be analyzed as diagnostic runs')
        assessment['application_dimensions']={k:row[k] for k in ('application_valid','control_status','ordering_relative_to_rm','gpu_overlap','correctness_status','trial_state')}
        assessment['workload_kernels']=workload
        for key,value in row.items():
            if key.startswith('T_') and value:
                domain='GPU_globaltimer' if key.endswith('_gpu_ns') else 'CLOCK_MONOTONIC_RAW'
                role='BG' if key.startswith(('T_bg','T_owner','T_rm')) or key=='T_ipc_received' else 'INT'
                event('worker/raw.csv',2,key,int(value),domain=domain,role=role,column=key,
                      interpretation='block0/thread0 marker proxy or host observation; not a hardware context-switch timestamp')
        assessment['marker_consistency']={role:{'entry_node':row.get('int_entry_node') if role=='INT' else 'main',
            'launch_id':row.get('int_launch_id') if role=='INT' else row.get('trial'),
            'trace_arithmetic_duration_ns':k['end']-k['start'],
            'marker_entry_to_done_globaltimer_ns':int(row['T_int_graph_done_gpu_ns'])-int(row['T_int_graph_entry_gpu_ns']) if role=='INT' else int(row['T_bg_done_gpu_ns'])-int(row['T_bg_main_gpu_ns']),
            'clock_comparison':'durations in their own domains only; marker is inside kernel, not exact activity start; no epoch subtraction'} for role,k in workload.items()}
        # Journal remains in its RAW clock. No subtraction from Nsight ns.
        for e in facts['events']:
            for field in ('call_begin_ns','call_end_ns'):
                if e.get(field) is not None:event(e['recorded_owner']+'_control_events.jsonl',e['operation_seq'],str(e.get('command'))+'_'+field,e[field],domain='CLOCK_MONOTONIC_RAW',role=e['recorded_owner'].upper(),column=field)
        # Preserve local RAW marker brackets and their exact operation sequence;
        # link by full label, never infer RAW offset from a common "ns" unit.
        labelmap={v['label']:v for v in nvtx}
        links=[]
        for role in identities:
            env=json.loads((directory/'worker'/f'{role}_diagnostic_environment.json').read_text())
            if env['marker_overflow']:problems.append('Local marker buffer overflow')
            for line in (directory/'worker'/f'{role}_diagnostic_markers.jsonl').read_text().splitlines():
                m=json.loads(line);v=labelmap.get(m['label'])
                links.append(dict(raw_marker=m,nvtx_rowid=v['rowid'] if v else None,clock_mapping='not applied',
                    model_boundary='NVTX trace timestamp is expected inside recorded RAW API-call bracket; affine interpolation would need explicit drift/error assumptions'))
                if not v:problems.append('Missing exported NVTX annotation')
        write(output/'marker_links.json',links)
        for e in facts['events']:
            if e.get('attempted') and e.get('command')==0xa06c0105:
                match=[v for v in nvtx if v['label'].endswith('/'+str(e['operation_seq'])+'/preempt_ioctl_envelope') and pid(v['globalTid'])==pair['bg_pid']]
                if len(match)!=1:problems.append('PREEMPT journal/marker mismatch')
                else:assessment['preempt_envelope']={'nvtx_rowid':match[0]['rowid'],'operation_seq':e['operation_seq'],'start':match[0]['start'],'end':match[0]['end'],
                    'meaning':'annotation brackets the synchronous call plus small host bookkeeping; exact syscall timestamps remain in RAW journal'}
        if 'BG' in workload and 'INT' in workload:
            bg=workload['BG'];it=workload['INT']
            assessment['BG_original_lifetime_contains_INT']=bg['start']<it['start']<it['end']<bg['end']
            assessment['INT_start_after_interaction_trace_ns']=same_clock_delta(it['start'],DOMAIN,interaction['start'],DOMAIN)
            assessment['original_BG_end_after_INT_end_trace_ns']=same_clock_delta(bg['end'],DOMAIN,it['end'],DOMAIN)
            envelope=assessment.get('preempt_envelope')
            if envelope:
                assessment['request_associated_observations']={
                    'switch_rowids_in_ioctl_envelope':[r['rowid'] for r in trial_switches if envelope['start']<=r['timestamp']<=envelope['end']],
                    'INT_kernel_start_in_envelope':envelope['start']<=it['start']<=envelope['end'],
                    'BG_switch_rowids_after_INT_end_before_original_BG_end':[r['rowid'] for r in trial_switches if pid(r['globalPid'])==pair['bg_pid'] and it['end']<r['timestamp']<bg['end']],
                    'interpretation':'observed temporal association, not exclusive causality or exact syscall/hardware duration'}
            elif mode=='group-preempt-wait':
                assessment['request_association']='NO_ACTIVE_REQUEST_IN_CAPTURE'
        assessment['status']='OBSERVED_AND_ASSOCIATED' if not problems else 'OBSERVABILITY_LIMITED'
        assessment['backend_ready_for_d1']=not problems
        assessment['association_strength']='scoped process/GPU/workload + unique current compute group; no native switch-ID to TSG-ID equality'
    except (OSError,ValueError,KeyError,sqlite3.DatabaseError) as e:
        problems.append(str(e))
    finally:
        assessment['limitations']=problems;write(output/'assessment.json',assessment)
        if events:
            with (output/'events.csv').open('w') as f:
                w=csv.DictWriter(f,fieldnames=list(events[0]));w.writeheader();w.writerows(events)
        # A text timeline displays endpoints and original operation names only.
        lines=['Phase7 diagnostic timeline. No residency bars. Times are integer ns in each named domain.','']
        for domain in sorted({e['clock_domain'] for e in events}):
            lines.append('Clock: '+domain)
            for e in sorted((e for e in events if e['clock_domain']==domain),key=lambda e:e['raw_timestamp'] or 0):
                window=assessment.get('trial_window_ns')
                if domain==DOMAIN and window and not window[0]<=e['raw_timestamp']<=window[1]:continue
                lines.append(f"{e['raw_timestamp']}  {e['associated_role'] or 'unknown'}  {e['raw_event_type']}  [{e['source']} row {e['source_record_id']} {e['source_column']}]")
            lines.append('')
        (output/'timeline.txt').write_text('\n'.join(lines)+'\n')
        artifacts={str(p.relative_to(directory)):{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for p in directory.glob('trace.*') if p.is_file()}
        write(output/'local_raw_artifacts.json',{'artifacts':artifacts,'originals_retained_locally':True,
              'sharing':'Original Nsight files include inherited environment/system metadata; excluded from git. events.csv contains this experiment only.'})
    return assessment

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    r=analyze(a.run,a.output);print(json.dumps(r,indent=2));raise SystemExit(0 if r['backend_ready_for_d1'] else 2)
