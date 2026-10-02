"""Connection-domain filtering, native coordinates, and strict feature4 readers."""
import argparse
import json
import struct
import subprocess
import tempfile
from pathlib import Path
from endpoint_dedup_regression import run
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
        for name in (*queries, 'mapping_cases', 'fixed_owner_cases'):
            source = fixtures/(name+'.sv')
            top = name if name in ('mapping_cases','fixed_owner_cases') else 'top'
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
            dbs=[]
            for canonical in ('0','1'):
                db = root/(name+canonical+'.db'); dbs.append(db)
                compiled=run(binary, ['compile','--db',db,'--single-unit',source,'--top',top],
                             {'RTL_TRACE_CANONICAL_BODIES':canonical,'RTL_TRACE_CANONICAL_VERIFY':'1'})
                if canonical == '1':
                    assert 'mismatched_lists=0' in compiled.stdout
                assert 'SEMANTICS_EPOCH:14\n' in Path(str(db)+'.meta').read_text()
                for mode,target,want in checks:
                    body=json.loads(run(binary,['trace','--db',db,'--signal',target,
                                                '--mode',mode,'--format','json']).stdout)
                    endpoints=body['endpoints']
                    if labels:
                        assert sorted(e['line'] for e in endpoints)==sorted(labels[w] for w in want), body
                    else:
                        assert sorted(e['assignment'] for e in endpoints)==sorted(want), body
                    assert all(not e['bit_map'].startswith('Q1;') for e in endpoints), body
                    if 'constant_u.d[7]' in target:
                        assert any(s['reason']=='constant_connection' for s in body['stops']), body
                    if 'mixed_u.d' in target or 'signed_u.d[7]' in target or 'boolean_u.en' in target:
                        assert any(s['reason']=='unresolved_connection_mapping' for s in body['stops']), body
                    count += 1
            assert dbs[0].read_bytes()==dbs[1].read_bytes(), name
            if name=='ff_fwd_nogen':
                malformed_envelopes(binary,dbs[0],root)
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
            assert not body['endpoints'] and any(s['reason']=='unresolved_connection_mapping'
                                                for s in body['stops']),body
            count+=1
        print(f'PASS: {count} exact/constant/unsupported/array queries; canonical bytes, VERIFY, epoch14 and strict envelopes')


def malformed_envelopes(binary,db,root):
    data=db.read_bytes(); sections=layout(data)
    endpoint_start,endpoint_count,_=sections['endpoints']
    offsets_start,_,_=sections['offsets']; blob,_,_=sections['blob']
    projected=next(endpoint_start+48*i for i in range(endpoint_count)
                   if data[endpoint_start+48*i+47]&4)
    string_id=struct.unpack_from('<I',data,projected+12)[0]
    text_start=blob+struct.unpack_from('<I',data,offsets_start+4*string_id)[0]
    assert data[text_start:text_start+3]==b'Q1;'
    mutations=[('missing_feature',20,'<I',3), ('unknown_feature',20,'<I',15),
               ('missing_flag',projected+47,'<B',0), ('conflicting_markers',projected+47,'<B',28),
               ('negative_range',text_start+3,'<B',ord('-')),
               ('bad_version',text_start+1,'<B',ord('2'))]
    for name,offset,fmt,value in mutations:
        bad=bytearray(data); struct.pack_into(fmt,bad,offset,value)
        path=root/(name+'.db');path.write_bytes(bad)
        result=subprocess.run([str(binary),'find','--db',str(path),'--query','top'],capture_output=True,text=True)
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
