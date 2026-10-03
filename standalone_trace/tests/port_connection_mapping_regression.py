"""Connection-domain filtering, native coordinates, and strict feature4 readers."""
import argparse
import json
import struct
import subprocess
import tempfile
from pathlib import Path
from endpoint_dedup_regression import run, read_db
from graph_db_mapping_regression import layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    fixtures = args.source_dir.resolve()/'tests/fixtures/port_connection_mapping'
    queries = {
        'ff_fwd_nogen': [('drivers','top.u_w.d[1]', ['x[11:8] = c']),
                         ('drivers','top.u_w.d', ['x[11:8] = c']),
                         ('loads','top.u_w.q[0]', ['o2 = w[11:8]'])],
        'fe_bitsel': [('drivers','top.L[3].u.d[1]', ['x[15:12] = e']),
                      ('loads','top.L[2].u.q[0]', ['o2 = w[11:8]'])],
        'fi_swap': [('drivers','top.u_sm.d.a', ['s.b = r'])],
    }
    with tempfile.TemporaryDirectory(prefix='rtl_port_mapping_') as directory:
        root = Path(directory)
        count = 0
        for name in (*queries, 'mapping_cases', 'fixed_owner_cases', 'full_local_bridge_cases', 'compact_port_routes'):
            source = fixtures/(name+'.sv')
            top = name if name in ('mapping_cases','fixed_owner_cases','full_local_bridge_cases', 'compact_port_routes') else 'top'
            labels = {line.split('// CHECK ',1)[1]: i for i,line in
                      enumerate(source.read_text().splitlines(),1) if '// CHECK ' in line}
            checks = queries.get(name, [])
            if name == 'mapping_cases':
                # Expected labels follow physical LSB concat order, independent
                # of the public endpoint bitmap or production mapping parser.
                checks = [('drivers','concat.d[0]',['high']), ('drivers','concat.d[7]',['low']),
                          ('drivers','concat.d',['high','low']),
                          ('drivers','repeat_u.d[0]',['high']), ('drivers','repeat_u.d[7]',['high']),
                          ('drivers','repeat_u.d',['high']),
                          ('drivers','asc_owner.d[0]',['ascending_high']),
                          ('drivers','asc_owner.d[7]',['ascending_low']),
                          ('drivers','nonzero.d[11]',['indexed_high']),
                          ('drivers','plus_sel.d[0]',['indexed_low']),
                          ('drivers','array_u.d[3]',['array_one']),
                          ('drivers','constant_u.d[7]',[]),
                          ('drivers','constant_u.d[0]',['high']),
                          ('drivers','mixed_u.d[7]',[]),
                          ('drivers','mixed_u.d',['high']),
                          ('drivers','signed_u.d[7]',[]),
                          ('drivers','signed_u.d[0]',['sign_low']),
                          ('drivers','nested_u.u.d[0]',['low']),
                          ('drivers','nested_u.u.d[7]',['high']),
                          ('loads','concat.q[0]',['out_high']),
                          ('loads','concat.q[7]',['out_low']),
                          ('loads','nested_u.u.q[0]',['nested_high']),
                          ('loads','nested_u.u.q[7]',['nested_low'])]
                checks = [(mode,'mapping_cases.'+target,want) for mode,target,want in checks]
            if name == 'fixed_owner_cases':
                checks = [('loads','a[1][1]',['leaf2']), ('loads','a[1][0]',[]),
                          ('loads','a[0]',['leaf0','leaf3']),
                          ('drivers','z[2][2]',['leaf1']), ('drivers','z[2][0]',[]),
                          ('drivers','reverse_u.d[2]',['r1']),
                          ('loads','reverse_u.q[2]',['w1']),
                          ('loads','en',['boolean_use']), ('loads','sel',['boolean_use']),
                          ('drivers','boolean_u.en',[])]
                checks = [(mode,'fixed_owner_cases.'+target,want) for mode,target,want in checks]
            if name == 'full_local_bridge_cases':
                checks = []
                for owner in ('dyn_u','mixed_u','static_u','complete_u'):
                    for suffix in ('','[3]','[2:1]'):
                        checks.append(('drivers',owner+'.d'+suffix,['chosen_reset','chosen_data']))
                for suffix in ('','[3]'):
                    checks.append(('drivers','other_u.d'+suffix,['other_data']))
                for suffix in ('','[7]','[6:5]'):
                    checks.append(('drivers','nonzero_u.d'+suffix,['chosen_reset','chosen_data']))
                for owner,label in [('dyn_out','consumer0'),('mixed_out','consumer1')]:
                    for suffix in ('','[3]','[2:1]'):
                        checks.append(('loads',owner+'.q'+suffix,[label]))
                checks = [(mode,'full_local_bridge_cases.'+target,want) for mode,target,want in checks]
            if name == 'compact_port_routes':
                checks=[('drivers','rows[1].u.d[1]',['array_driver']),
                        ('drivers','rows[1].u.d[6:3]',['array_driver']),
                        ('loads','rows[1].u.q[1]',['array_low_use']),
                        ('loads','rows[1].u.q[6]',['array_high_use']),
                        ('loads','rows[1].u.q[6:3]',['array_low_use','array_high_use']),
                        ('drivers','swap_u.d[0]',['concat_high']),
                        ('drivers','swap_u.d[7]',['concat_low']),
                        ('drivers','repeat_u.d[0]',['concat_low']),
                        ('drivers','repeat_u.d[4]',['concat_low']),
                        ('loads','swap_u.q[0]',['swapped_high_use']),
                        ('loads','swap_u.q[7]',['swapped_low_use']),
                        ('drivers','asc_u.d[6]',['array_driver']),
                        ('drivers','nz_u.d[5]',['array_driver']),
                        ('drivers','sync_u.u.L[5].u.d',['scalar_data']),
                        ('drivers','sync_u.d[5]',['scalar_data']),
                        ('drivers','struct_u.d.a',['struct_low']),
                        ('drivers','struct_u.d.a[1]',['struct_low']),
                        ('drivers','struct_u.d.b',['struct_high']),
                        ('loads','struct_out_u.q.a[1]',['struct_output_a']),
                        ('loads','struct_out_u.q.b[1]',['struct_output_b'])]
                checks += [('drivers','row_concat_u.d[2]',['row_zero_driver']),
                           ('drivers','row_concat_u.d[10]',['row_one_driver']),
                           ('drivers','row_concat_u.d',['row_zero_driver','row_one_driver']),
                           ('loads','row_concat_u.q[2]',['row_zero_use']),
                           ('loads','row_concat_u.q[10]',['row_one_use']),
                           ('loads','row_concat_u.q',['row_zero_use','row_one_use']),
                           ('drivers','fragment_u.d[0]',['row_zero_driver']),
                           ('drivers','fragment_u.d[1]',['row_one_driver']),
                           ('drivers','fragment_u.d[7]',['row_one_driver']),
                           ('drivers','gap_u.d[0]',['row_zero_driver']),
                           ('drivers','gap_u.d[1]',['row_one_driver']),
                           ('drivers','row_swap_u.d[0]',['row_one_driver']),
                           ('drivers','row_swap_u.d[8]',['row_zero_driver']),
                           ('drivers','row_repeat_u.d[0]',['row_zero_driver']),
                           ('drivers','row_repeat_u.d[8]',['row_zero_driver'])]
                checks=[(mode,'compact_port_routes.'+target,want) for mode,target,want in checks]
            dbs=[]
            for canonical in ('0','1'):
                db = root/(name+canonical+'.db'); dbs.append(db)
                compiled=run(binary, ['compile','--db',db,'--single-unit',source,'--top',top],
                             {'RTL_TRACE_CANONICAL_BODIES':canonical,'RTL_TRACE_CANONICAL_VERIFY':'1'})
                if canonical == '1':
                    assert 'mismatched_lists=0' in compiled.stdout
                assert 'SEMANTICS_EPOCH:17\n' in Path(str(db)+'.meta').read_text()
                for mode,target,want in checks:
                    body=json.loads(run(binary,['trace','--db',db,'--signal',target,
                                                '--mode',mode,'--format','json']).stdout)
                    endpoints=body['endpoints']
                    if labels:
                        fallback_case = name=='mapping_cases' and (target.startswith('mapping_cases.mixed_u.d') or target=='mapping_cases.signed_u.d[7]') or name=='fixed_owner_cases' and target=='fixed_owner_cases.boolean_u.en'
                        if fallback_case:
                            assert endpoints and any(e['bit_map_approximate'] for e in endpoints),body
                            assert any(e['kind']=='port' and e['bit_map_approximate'] for e in endpoints),body
                            if target=='mapping_cases.mixed_u.d[7]':
                                assert not any(e['path']=='mapping_cases.x' for e in endpoints),body
                        else:
                            assert sorted(e['line'] for e in endpoints)==sorted(labels[w] for w in want), body
                    else:
                        assert sorted(e['assignment'] for e in endpoints)==sorted(want), body
                    assert all(not e['bit_map'].startswith(('Q1;','R1;')) for e in endpoints), body
                    if name == 'compact_port_routes':
                        expected_native={
                            'row_concat_u.d[2]':'[0][2]', 'row_concat_u.d[10]':'[1][2]',
                            'row_concat_u.q[2]':'[0][2]', 'row_concat_u.q[10]':'[1][2]',
                            'fragment_u.d[0]':'[0][7]', 'fragment_u.d[1]':'[1][0]',
                            'fragment_u.d[7]':'[1][6]', 'gap_u.d[0]':'[0][0]',
                            'gap_u.d[1]':'[1][0]', 'row_swap_u.d[0]':'[1][0]',
                            'row_swap_u.d[8]':'[0][0]', 'row_repeat_u.d[0]':'[0][0]',
                            'row_repeat_u.d[8]':'[0][0]'}
                        short=target.removeprefix('compact_port_routes.')
                        if short in expected_native:
                            assert [e['bit_map'] for e in endpoints]==[expected_native[short]],body
                        if target.endswith(('rows[1].u.d[1]','rows[1].u.q[1]','asc_u.d[6]','nz_u.d[5]')):
                            assert [e['bit_map'] for e in endpoints] == ['[1][1]'],body
                        if target.endswith('sync_u.u.L[5].u.d') or target.endswith('sync_u.d[5]'):
                            assert len(endpoints)==1 and '.p.p0.dbg' in endpoints[0]['path'],body
                    if 'constant_u.d[7]' in target:
                        assert any(s['reason']=='constant_connection' for s in body['stops']), body
                    if target.startswith('mapping_cases.mixed_u.d') or 'mapping_cases.signed_u.d[7]' in target or 'boolean_u.en' in target:
                        assert any(s['reason']=='unresolved_connection_mapping' for s in body['stops']), body
                    count += 1
            assert dbs[0].read_bytes()==dbs[1].read_bytes(), name
            if name=='full_local_bridge_cases':
                stored=read_db(dbs[0])['lists']
                for owner in ('static_u','complete_u'):
                    rows=stored['full_local_bridge_cases.'+owner+'.d'][0]
                    assert len(rows)==1 and rows[0][7]==1,(owner,rows)
                for owner in ('dyn_u','mixed_u'):
                    rows=stored['full_local_bridge_cases.'+owner+'.d'][0]
                    assert len(rows)==2 and all(row[7]==0 for row in rows),(owner,rows)
            if name=='ff_fwd_nogen':
                malformed_envelopes(binary,dbs[0],root)
            if name=='compact_port_routes':
                malformed_routes(binary,dbs[0],root)
            if name=='mapping_cases':
                assert_marker_refs(dbs[0])
        # Fixed unpacked parent mappings must preserve exact connected rows and bits.
        array_source=root/'array_parent.sv'
        array_source.write_text('''module leaf(input logic [3:0] d, output logic q);
assign q=^d;
endmodule
module array_parent(input logic [3:0] arr[0:2], output logic q);
leaf u(.d({arr[0][3],arr[2][2],arr[1][1],arr[0][0]}),.q(q));
endmodule
''')
        for canonical in ('0','1'):
            db=root/('array_parent'+canonical+'.db')
            run(binary,['compile','--db',db,'--single-unit',array_source,'--top','array_parent'],
                {'RTL_TRACE_CANONICAL_BODIES':canonical,'RTL_TRACE_CANONICAL_VERIFY':'1'})
            for target in ('arr[1][0]','arr[2][0]'):
                body=json.loads(run(binary,['trace','--db',db,'--signal','array_parent.'+target,
                                            '--mode','loads','--format','json']).stdout)
                assert not body['endpoints'],body
                count+=1
        # A whole unpacked formal argument has no supported affine expression
        # map. An empty map must not pass an exact-only active bridge test.
        formal_source=root/'unpacked_formal.sv'
        formal_source.write_text('module leaf(input logic d[0:1],output logic q); assign q=d[0]; endmodule\n'
                                 'module formal_top(input logic a[0:1],output logic y); leaf u(.d(a),.q(y)); endmodule\n')
        for canonical in ('0','1'):
            db=root/('formal'+canonical+'.db')
            run(binary,['compile','--db',db,'--single-unit',formal_source,'--top','formal_top'],
                {'RTL_TRACE_CANONICAL_BODIES':canonical,'RTL_TRACE_CANONICAL_VERIFY':'1'})
            body=json.loads(run(binary,['trace','--db',db,'--signal','formal_top.u.d[0]',
                                        '--mode','drivers','--format','json']).stdout)
            assert body['endpoints'] and all(e['bit_map_approximate'] for e in body['endpoints']),body
            assert any(s['reason']=='unresolved_connection_mapping' for s in body['stops']),body
            count+=1
        from query_reference_identity_regression import check as check_query_refs
        count += check_query_refs(binary, fixtures, root)
        print(f'PASS: {count} exact/constant/unsupported/array/context queries; canonical bytes, VERIFY, epoch17 and strict envelopes')


def malformed_envelopes(binary,db,root):
    data=db.read_bytes(); sections=layout(data)
    endpoint_start,endpoint_count,_=sections['endpoints']
    offsets_start,_,_=sections['offsets']; blob,_,_=sections['blob']
    projected=next(endpoint_start+48*i for i in range(endpoint_count)
                   if data[endpoint_start+48*i+47]&4 and not data[endpoint_start+48*i+47]&32)
    string_id=struct.unpack_from('<I',data,projected+12)[0]
    text_start=blob+struct.unpack_from('<I',data,offsets_start+4*string_id)[0]
    assert data[text_start:text_start+3]==b'Q1;'
    mutations=[('missing_feature',20,'<I',3), ('unknown_feature',20,'<I',31),
               ('missing_flag',projected+47,'<B',0), ('conflicting_markers',projected+47,'<B',28),
               ('negative_range',text_start+3,'<B',ord('-')),
               ('bad_version',text_start+1,'<B',ord('2'))]
    for name,offset,fmt,value in mutations:
        bad=bytearray(data); struct.pack_into(fmt,bad,offset,value)
        path=root/(name+'.db');path.write_bytes(bad)
        result=subprocess.run([str(binary),'find','--db',str(path),'--query','top'],capture_output=True,text=True)
        assert result.returncode==1 and not result.stdout,(name,result.stdout,result.stderr)


def malformed_routes(binary, db, root):
    data=db.read_bytes(); sections=layout(data)
    offsets_start,offset_count,_=sections['offsets']; blob,blob_count,_=sections['blob']
    offsets=list(struct.unpack_from('<'+'I'*offset_count,data,offsets_start))
    strings=[data[blob+offsets[i]:blob+offsets[i+1]].decode() for i in range(offset_count-1)]
    signal_start,signal_count,_=sections['signals']; endpoint_start,_,_=sections['endpoints']
    owner='compact_port_routes.rows[1].u.d'
    signal=next(struct.unpack_from('<8I',data,signal_start+32*i) for i in range(signal_count)
                if strings[struct.unpack_from('<I',data,signal_start+32*i)[0]]==owner)
    assert signal[2]==1
    ep=endpoint_start+48*signal[1]
    record=struct.unpack_from('<11I4B',data,ep)
    assert record[14]==36 and record[11]==1
    bitmap_id=record[3]; text=strings[bitmap_id]
    assert text.startswith('R1;0:7:') and text.endswith(':8|'),text
    sid=text.split(':')[2]
    def encoded(new):
        lo,hi=offsets[bitmap_id:bitmap_id+2]
        raw=new.encode();delta=len(raw)-(hi-lo)
        changed=bytearray(data[:blob]+data[blob:blob+lo]+raw+data[blob+hi:])
        struct.pack_into('<Q',changed,32,blob_count+delta)
        for i in range(bitmap_id+1,offset_count):struct.pack_into('<I',changed,offsets_start+4*i,offsets[i]+delta)
        return changed
    mutations=[]
    for name,offset,fmt,value in [
        ('missing_route_feature',20,'<I',7),('route_without_coverage',ep+47,'<B',32),
        ('route_flag_removed',ep+47,'<B',4),('route_constant',ep+47,'<B',44),
        ('route_approximate',ep+45,'<B',1),('route_not_port',ep+44,'<B',0),
        ('route_assignment_range',ep+46,'<B',1),('route_assignment_offset',ep+20,'<I',1),
        ('route_ordinary_refs',ep+28,'<I',1),
        ('route_opposite_direction',ep+8,'<I',strings.index('output'))]:
        changed=bytearray(data);struct.pack_into(fmt,changed,offset,value);mutations.append((name,changed))
    texts={
        'route_native_payload':text+'[1]',
        'route_leading_zero':text.replace('R1;0:','R1;00:'),
        'route_negative_ordinal':text.replace('R1;0:','R1;-1:'),
        'route_incomplete_child':text.replace('R1;0:7:','R1;0:6:'),
        'route_target_out_of_bounds':f'R1;0:7:{sid}:24|',
        'route_target_not_signal':f'R1;0:7:{record[1]}:8|',
        'route_missing_target':f'R1;0:7:4294967295:8|',
        'route_offset_overflow':f'R1;0:7:{sid}:2147483647|',
        'route_overlap':f'R1;0:4:{sid}:8;4:7:{sid}:8|',
        'route_gap':f'R1;0:2:{sid}:8;4:7:{sid}:8|',
        'route_not_normalized':f'R1;0:3:{sid}:8;4:7:{sid}:12|',
        'route_piece_cap':'R1;'+';'.join(f'{i}:{i}:{sid}:8' for i in range(257))+'|',
    }
    mutations.extend((name,encoded(value)) for name,value in texts.items())
    for name,changed in mutations:
        path=root/(name+'.db');path.write_bytes(changed)
        result=subprocess.run([str(binary),'trace','--db',str(path),'--signal',owner,
                               '--mode','drivers','--format','json'],capture_output=True,text=True)
        assert result.returncode==1 and not result.stdout,(name,result.stdout,result.stderr)


def assert_marker_refs(db):
    data=db.read_bytes(); sections=layout(data)
    strings=[]
    offsets,number,_=sections['offsets']; blob,_,_=sections['blob']
    for i in range(number-1):
        lo,hi=struct.unpack_from('<II',data,offsets+4*i)
        strings.append(data[blob+lo:blob+hi].decode())
    signal_begin,signals,_=sections['signals']
    constant_owner=next(i for i in range(signals) if
                        strings[struct.unpack_from('<I',data,signal_begin+32*i)[0]]=='mapping_cases.constant_u.d')
    # Exact lower-half drivers name x. The constant upper-half marker names
    # the owner and must not create a reverse driver-reference bridge.
    start,count,_=sections['driver_ranges']; refs,_,_=sections['driver_ids']
    owner_path=struct.unpack_from('<I',data,signal_begin+32*constant_owner)[0]
    for i in range(count):
        path,begin,size=struct.unpack_from('<III',data,start+12*i)
        if path==owner_path:
            ids=struct.unpack_from('<'+str(size)+'I',data,refs+4*begin)
            assert constant_owner not in ids,ids


if __name__=='__main__':
    main()
