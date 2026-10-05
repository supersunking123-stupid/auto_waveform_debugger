"""Retain the active unused Input terminal, with exact selected Q1 coverage."""
import argparse
import json
import tempfile
from pathlib import Path
from endpoint_dedup_regression import run


def query(binary, db, signal, mode='drivers'):
    return json.loads(run(binary, ['trace','--db',db,'--mode',mode,'--signal',signal,'--format','json']).stdout)


def exact_ports(result, path):
    return [e for e in result['endpoints'] if e['kind']=='port' and e['path']==path and not e['bit_map_approximate']]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl-trace',type=Path,required=True)
    parser.add_argument('--source-dir',type=Path,required=True)
    parser.add_argument('--baseline-bin',type=Path)
    args=parser.parse_args();binary=args.rtl_trace.resolve()
    source=(args.source_dir/'tests/fixtures/active_boundaries/records.sv').resolve()
    leaf='active_boundary_repro.lut.FOR_LAYER[7].layer.FOR_SLICES[2].slice.req_entry_index'
    used='active_boundary_repro.lut.FOR_LAYER[0].layer.FOR_SLICES[0].slice.req_entry_index'
    checked=0
    with tempfile.TemporaryDirectory(prefix='active_boundary_regression_') as tmp:
        root=Path(tmp)
        baseline=None
        if args.baseline_bin:
            db=root/'baseline.db';run(args.baseline_bin.resolve(),['compile','--db',db,'--single-unit',source,'--top','active_boundary_repro'])
            baseline=exact_ports(query(args.baseline_bin.resolve(),db,leaf),leaf)
            assert len(baseline)==1
        for canon in ('0','1'):
            for cache in ('0','1'):
                db=root/f'records{canon}{cache}.db'
                env={'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':cache,'RTL_TRACE_CANONICAL_VERIFY':'1'}
                c=run(binary,['compile','--db',db,'--single-unit',source,'--top','active_boundary_repro'],env)
                if canon=='1':
                    assert 'mismatched_lists=0' in c.stdout,c.stdout
                    assert ' map_fail=0 ' in c.stdout,c.stdout
                assert 'SEMANTICS_EPOCH:20\n' in Path(str(db)+'.meta').read_text()
                for signal in (leaf,leaf+'[0]',leaf+'[6]',leaf+'[3:1]'):
                    result=query(binary,db,signal);ports=exact_ports(result,leaf)
                    assert len(ports)==1,result
                    p=ports[0]
                    assert p['bit_map']=='' and p['bit_map_encoding']=='unrestricted' and p['assignment']=='' and p['lhs']==p['rhs']==[],p
                    if baseline:assert ports==baseline,(ports,baseline)
                    assert any(e['kind']=='expr' and e['path']=='active_boundary_repro.split.np_wr_req_entry_index' for e in result['endpoints']),result
                    assert not result['diagnostics'];checked+=1
                assert not exact_ports(query(binary,db,used),used)
        body=source.read_text()
        cases={
          'constant':body.replace('req_entry_index[LIDX_WD-1:0]',"9'b0"),
          'zero_extend':body.replace('req_entry_index[LIDX_WD-1:0]','req_entry_index[5:0]'),
          'sign_extend':body.replace('req_entry_index[LIDX_WD-1:0]','$signed(req_entry_index[5:0])'),
          'function_read':body.replace("assign observed=1'b0;",'function automatic logic f(input logic [IDX_WD-1:0] x); return ^x; endfunction\n assign observed=f(req_entry_index);'),
          'system_read':body.replace("assign observed=1'b0;",'always @(*) $display("%h",req_entry_index);\n assign observed=1\'b0;')}
        # Empty active load-index is intentionally stricter than a missing
        # full-width bridge; partial/function/system reads cannot qualify.
        for name,text in cases.items():
            f=root/(name+'.sv');f.write_text(text)
            for canon in ('0','1'):
                db=root/f'{name}{canon}.db'
                run(binary,['compile','--db',db,'--single-unit',f,'--top','active_boundary_repro'],{'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':'1','RTL_TRACE_CANONICAL_VERIFY':'1'})
                assert not exact_ports(query(binary,db,leaf),leaf),name
                checked+=1
        other='''typedef struct packed {logic [3:0] a;logic [3:0] b;} st;
module struct_leaf(input st p); endmodule
module unpacked_leaf(input wire p[0:0]); endmodule
module nonansi_leaf(.p(bus[3:0]));input wire[7:0] bus;endmodule
module exclusions(input wire[7:0] x);
 wire[7:0] driven; assign driven=x;
 struct_leaf s(.p(driven));unpacked_leaf u(.p('{x[0]}));nonansi_leaf n(.p(driven[3:0]));
endmodule'''
        f=root/'exclusions.sv';f.write_text(other)
        for canon in ('0','1'):
            db=root/f'exclusions{canon}.db';run(binary,['compile','--db',db,'--single-unit',f,'--top','exclusions'],{'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':'1','RTL_TRACE_CANONICAL_VERIFY':'1'})
            for path in ('exclusions.s.p','exclusions.u.p[0]','exclusions.n.bus[7]'):
                assert not exact_ports(query(binary,db,path),path.split('[')[0]),path
                checked+=1
        print(f'PASS {checked} active-boundary retention checks; no owned DB retained')


if __name__=='__main__':main()
