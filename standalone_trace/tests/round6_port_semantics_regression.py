"""Non-ANSI/formal frames, conversion fallback, and scalar unpacked indices."""
import argparse
import json
import struct
import subprocess
import tempfile
from pathlib import Path
from endpoint_dedup_regression import run
from graph_db_mapping_regression import layout


def check_marker_provenance(binary, db, root):
    data=db.read_bytes(); begin,count,_=layout(data)['endpoints']
    marker=next(begin+48*i for i in range(count) if data[begin+48*i+47]&16 and data[begin+48*i+46])
    start,end=struct.unpack_from('<II',data,marker+20)
    assert start<end
    for name,offset,fmt,value in [('empty_span',24,'<I',start),('reversed_span',24,'<I',start-1),
                                  ('missing_line',16,'<I',0),('missing_file',4,'<I',0xffffffff),
                                  ('absent_span_flag',46,'<B',0),('logic_marker',44,'<B',0),
                                  ('marker_refs',32,'<I',1)]:
        changed=bytearray(data);struct.pack_into(fmt,changed,marker+offset,value)
        path=root/(name+'.db');path.write_bytes(changed)
        result=subprocess.run([str(binary),'find','--db',str(path),'--query','dyn'],capture_output=True,text=True)
        assert result.returncode==1 and not result.stdout,(name,result.stdout,result.stderr)


def query(binary, db, mode, signal):
    body = json.loads(run(binary, ['trace', '--db', db, '--mode', mode,
                                  '--signal', signal, '--format', 'json']).stdout)
    assert not body['diagnostics'], body
    return body


def lines(body):
    return sorted(e['line'] for e in body['endpoints'] if e['kind'] == 'expr')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl-trace', required=True, type=Path)
    parser.add_argument('--source-dir', required=True, type=Path)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    fixtures = args.source_dir.resolve() / 'tests/fixtures/round6_ports'
    count = 0
    with tempfile.TemporaryDirectory(prefix='rtl_round6_ports_') as temporary:
        root = Path(temporary)
        for name in ('portexpr', 'owide', 'dyn', 'gv4', 'unpk2', 'scalar_mirror', 'scalar_layers', 'fallback_contract', 'fallback_frames', 'multiple_fallbacks'):
            top = 'top' if name == 'unpk2' else name
            dbs = []
            for canonical in ('0', '1'):
                db = root / (name + canonical + '.db'); dbs.append(db)
                compiled = run(binary, ['compile', '--db', db, '--single-unit', fixtures/(name+'.sv'), '--top', top],
                               {'RTL_TRACE_CANONICAL_BODIES': canonical, 'RTL_TRACE_CANONICAL_VERIFY': '1',
                                'RTL_TRACE_BODY_CACHE': '1'})
                assert 'SEMANTICS_EPOCH:17\n' in Path(str(db)+'.meta').read_text()
                if canonical == '1':
                    assert 'mismatched_lists=0' in compiled.stdout, compiled.stdout
                    assert 'map_fail=' in compiled.stdout, compiled.stdout
                checks = []
                if name == 'portexpr':
                    checks = [('drivers','u.w[3]',[]),('drivers','u.w[7]',[11]),('drivers','u.w[4]',[11]),
                              ('loads','x[4]',[3]),('loads','x[0]',[6]),('loads','u.r[4]',[14]),
                              ('loads','u.r[0]',[]),('drivers','c.a',[10]),('drivers','c.b[0]',[10]),('drivers','y',[3])]
                elif name == 'owide':
                    checks = [('drivers','w8[1]',[2]),('drivers','w8[3]',[3]),('drivers','w8[6]',[]),
                              ('drivers','w4[3]',[6]),('drivers','ws[7]',[10]),('drivers','ws[1]',[10]),
                              ('drivers','wc[7]',[2]),('drivers','wc[1]',[3]),('drivers','wc[3]',[])]
                elif name == 'dyn':
                    checks = [('drivers','u.d',[6]),('drivers','u.d[0]',[6]),('drivers','u2.d',[10,12]),
                              ('drivers','u2.d[1]',[10,12]),('drivers','u3.d',[16])]
                elif name == 'gv4':
                    checks = [('loads','x',[3]),('loads','u0.d',[])]
                elif name == 'unpk2':
                    checks = [('loads',f'FOR_LAYER[{i}].u.req_index_atop',[12]) for i in range(8)]
                elif name == 'scalar_mirror':
                    checks = [('loads',f'FOR_LAYER[{i}].u.q',[8]) for i in range(8)]
                elif name == 'scalar_layers':
                    checks = [('loads',f'FOR_LAYER[{i}].u.q',[8,10]) for i in range(8)]
                    checks += [('loads',f'FOR_LAYER[{i}].ua.q',[11]) for i in range(8)]
                for mode, target, expected in checks:
                    body = query(binary, db, mode, top+'.'+target)
                    assert lines(body) == expected, (name, target, body)
                    fallback = name == 'owide' and target.startswith('ws[') or name == 'dyn' and target in ('u.d','u.d[0]','u3.d')
                    unresolved = [s for s in body['stops'] if s['reason'] == 'unresolved_connection_mapping']
                    if fallback:
                        assert unresolved and all(e['bit_map_approximate'] for e in body['endpoints']), body
                        assert all('connection ' in s['detail'] and '.sv:' in s['detail'] for s in unresolved), body
                        if name == 'dyn':
                            assert any(e['kind']=='port' and e['path']==top+'.'+target.split('[')[0]
                                       for e in body['endpoints']), body
                    else:
                        assert not unresolved, body
                        assert all(not e['bit_map_approximate'] for e in body['endpoints']), body
                    if name in ('unpk2','scalar_mirror','scalar_layers'):
                        index = int(target.split('[')[1].split(']')[0])
                        assert all(e['bit_map']==f'[{index}]' and e['bit_map_encoding']=='declared_axes'
                                   for e in body['endpoints']), body
                    count += 1
                if name == 'dyn':
                    check_marker_provenance(binary,db,root)
                    # Packed-array members remain explicitly unsupported. Keep
                    # A's literal boundary instead of a falsely exact source.
                    for target in ('u4.d','m.l.d'):
                        body = query(binary,db,'drivers',top+'.'+target)
                        assert body['endpoints'] and all(e['bit_map_approximate'] for e in body['endpoints']), body
                        assert any(e['kind']=='port' and e['path']==top+'.'+target for e in body['endpoints']), body
                        assert any(s['reason']=='unresolved_connection_mapping' and 'connection ' in s['detail']
                                   for s in body['stops']), body
                        count += 1
                if name == 'gv4':
                    # Feature 7 was an experimental, rejected writer. Header
                    # rejection must be clean before exposing any endpoint.
                    rejected=root/('feature7_'+canonical+'.db')
                    data=bytearray(db.read_bytes());struct.pack_into('<I',data,20,7);rejected.write_bytes(data)
                    failed=subprocess.run([str(binary),'find','--db',str(rejected),'--query','gv4'],capture_output=True,text=True)
                    assert failed.returncode == 1 and 'unsupported v6 DB feature flags 7' in failed.stderr, failed.stderr
                if name == 'fallback_contract':
                    body = query(binary,db,'drivers','fallback_contract.u.a')
                    retained=[e for e in body['endpoints'] if e['kind']=='expr' and e['line']==15]
                    assert len(retained)==1 and retained[0]['bit_map']=='[7:0]' and retained[0]['bit_map_encoding']=='flat_bits',body
                    assert all(e['bit_map_approximate'] for e in body['endpoints']),body
                    assert any(s['reason']=='unresolved_connection_mapping' and 'arr[idx]' in s['detail'] for s in body['stops']),body
                    body=query(binary,db,'loads','fallback_contract.b')
                    assert any(e['kind']=='expr' and e['line']==6 and e['bit_map_approximate'] for e in body['endpoints']),body
                    assert any(s['reason']=='unresolved_connection_mapping' and 'choose ? a : b' in s['detail'] for s in body['stops']),body
                    count+=2
                if name == 'fallback_frames':
                    for instance,left in (('descending',7),('ascending',0),('nonzero',11)):
                        body=query(binary,db,'drivers',f'fallback_frames.{instance}.u.d')
                        retained=[e for e in body['endpoints'] if e['kind']=='expr' and e['line'] in (8,9)]
                        assert [(e['line'],e['bit_map'],e['bit_map_encoding']) for e in retained]==[(8,'[0]','flat_bits'),(9,'[7]','flat_bits')],body
                        assert all(e['bit_map_approximate'] for e in body['endpoints']),body
                        body=query(binary,db,'drivers',f'fallback_frames.{instance}.arr[{left}]')
                        assert len(body['endpoints'])==1 and body['endpoints'][0]['line']==8,body
                        assert body['endpoints'][0]['bit_map']==f'[{left}]' and body['endpoints'][0]['bit_map_encoding']=='declared_axes' and not body['endpoints'][0]['bit_map_approximate'],body
                        count+=2
                if name == 'multiple_fallbacks':
                    for owner,sources in (('two',('y','x')),('three',('z','y','x')),
                                          ('neighbor',('z','y','x')),('used',('z','y','x'))):
                        for suffix in ('',*[f'[{i}]' for i in range(len(sources))]):
                            body=query(binary,db,'drivers',f'multiple_fallbacks.{owner}.d{suffix}')
                            assert body['endpoints'],body
                            selected=None if not suffix else int(suffix[1:-1])
                            failed=selected is None or owner in ('two','three') or selected>0
                            if failed:
                                assert any(e['kind']=='port' and e['path']==f'multiple_fallbacks.{owner}.d' and e['bit_map_approximate'] for e in body['endpoints']),body
                                assert any(s['reason']=='unresolved_connection_mapping' and '.sv:' in s['detail'] for s in body['stops']),body
                                if selected is not None:
                                    source=sources[selected]; control={'x':'ai','y':'bi','z':'ci'}[source]
                                    allowed={source,control,{'x':'a','y':'b','z':'c'}[source],f'{owner}.d'}
                                    # Used ports must also keep A's coarse
                                    # whole-parent logic answer. New connection
                                    # rows and port boundaries stay chunk-bound.
                                    assert all((owner=='used' and e['kind']=='expr' and e['line'] in (9,10,11)) or
                                               e['path'].removeprefix('multiple_fallbacks.') in allowed for e in body['endpoints']),body
                            else:
                                assert not any(s['reason']=='unresolved_connection_mapping' for s in body['stops']),body
                                assert all(not e['bit_map_approximate'] and e['path'] in ('multiple_fallbacks.z',f'multiple_fallbacks.{owner}.d') for e in body['endpoints']),body
                            count+=1
            assert dbs[0].read_bytes() == dbs[1].read_bytes(), name
        print(f'PASS: {count} round6 queries, exact and approximate source contracts; canonical identity, VERIFY, cache1, epoch17')


if __name__ == '__main__':
    main()
