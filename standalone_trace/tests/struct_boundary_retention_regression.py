"""Exact active ANSI struct terminal retention; source writer and constants remain."""
import argparse,json,tempfile,subprocess
from pathlib import Path
from endpoint_dedup_regression import run

def check_packed_union(binary, source_dir, tmp, query):
 source=(source_dir/'tests/fixtures/struct_boundaries/packed_union.sv').resolve()
 expected={'st.u_un.d':{12},'st.u_un2.d':{13},'st.un':{12,13}}
 assignments={12:'un.p.h = i3',13:'un.p.l = i0'}
 count=0
 for canon in ('0','1'):
  for cache in ('0','1'):
   db=tmp/f'packed_union{canon}{cache}.db'
   compiled=run(binary,['compile','--db',db,'--single-unit',source,'--top','st'],
                {'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':cache,'RTL_TRACE_CANONICAL_VERIFY':'1'})
   if canon=='1':assert 'mismatched_lists=0' in compiled.stdout and ' map_fail=0 ' in compiled.stdout,compiled.stdout
   for signal,lines in expected.items():
    result=query(binary,db,signal)
    endpoints=result['endpoints']
    assert len(endpoints)==len(lines) and {e['line'] for e in endpoints}==lines,(canon,cache,signal,result)
    assert not result['diagnostics'],result
    assert all(stop['reason']=='cone_limit' for stop in result['stops']),result
    for endpoint in endpoints:
     assert endpoint['kind']=='expr' and endpoint['path']==('st.un.p.h' if endpoint['line']==12 else 'st.un.p.l') and not endpoint['bit_map_approximate'],result
     assert endpoint['bit_map']==('[7:4]' if endpoint['line']==12 else '[3:0]') and endpoint['bit_map_encoding']=='flat_bits',result
     assert endpoint['assignment']==assignments[endpoint['line']],result
     assert endpoint['lhs']==['st.un'] and endpoint['rhs']==['st.i3' if endpoint['line']==12 else 'st.i0'],result
    count+=1
 return count

def main():
 p=argparse.ArgumentParser();p.add_argument('--rtl-trace',type=Path,required=True);p.add_argument('--source-dir',type=Path,required=True);p.add_argument('--baseline-bin',type=Path);p.add_argument('--previous-bin',type=Path);a=p.parse_args()
 source=(a.source_dir/'tests/fixtures/struct_boundaries/struct_boundary.sv').resolve();binary=a.rtl_trace.resolve();root='struct_boundary_repro.sva.port_in__gen[0].in_port_dbgchk.k_controls';count=0
 with tempfile.TemporaryDirectory(prefix='struct_boundary_regression_') as tmp:
  tmp=Path(tmp);baseline=None
  def query(b,db,s):return json.loads(run(b,['trace','--db',db,'--mode','drivers','--signal',s,'--format','json']).stdout)
  union_count=check_packed_union(binary,a.source_dir,tmp,query);count+=union_count
  if a.baseline_bin:
   db=tmp/'A.db';run(a.baseline_bin.resolve(),['compile','--db',db,'--single-unit',source,'--top','struct_boundary_repro']);baseline=query(a.baseline_bin.resolve(),db,root)['endpoints']
  for canon in ('0','1'):
   for cache in ('0','1'):
    db=tmp/f'{canon}{cache}.db';env={'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':cache,'RTL_TRACE_CANONICAL_VERIFY':'1'}
    c=run(binary,['compile','--db',db,'--single-unit',source,'--top','struct_boundary_repro'],env)
    if canon=='1':assert 'mismatched_lists=0' in c.stdout and ' map_fail=0 ' in c.stdout,c.stdout
    for suffix in ('','[0]','[3]','[5]','[4]'):
     r=query(binary,db,root+suffix);ports=[e for e in r['endpoints'] if e['kind']=='port' and e['path']==root and not e['bit_map_approximate']]
     assert len(ports)==1,r
     port=ports[0];assert port['bit_map']=='' and port['bit_map_encoding']=='unrestricted' and not port['bit_map_approximate'] and port['lhs']==port['rhs']==[] and port['assignment']==''
     if baseline:assert ports==baseline,(ports,baseline)
     writers=[e for e in r['endpoints'] if e['kind']=='expr'];assert bool(writers)==(suffix in ('','[0]','[3]')),r
     if suffix in ('','[4]','[5]'):assert any(s['reason']=='constant_connection' for s in r['stops']),r
     assert not r['diagnostics'];count+=1
    q=subprocess.run([str(binary),'trace','--db',str(db),'--mode','drivers','--signal',root+'[32]','--format','json'],capture_output=True,text=True,timeout=10);assert q.returncode==1 and 'axis_out_of_bounds' in q.stdout
  body=source.read_text()
  cases={'unknown_constant':body.replace("26'h0","26'hx"),'unknown_ternary':body.replace('localparam BUFFER_WRITE__VARIANT=1;','wire BUFFER_WRITE__VARIANT=input_pattern[7];'),'union':body.replace(body[:body.index('} hlsif_chk_dbg_k_controls_t;')], 'typedef union packed {\n'+''.join(' logic [31:0] '+name+';\n' for name in ('lm_insert_enderror_for_pkt_in_flight_at_ld','reserved','expect_enderror_wo_end','expect_discarded_end','expect_bus_valid_without_tlp','expect_gaps_before_start','valid_can_go_down_in_between_a_packet','vaid_data_can_change_without_ready','valid_may_not_get_ready','data_can_be_x_without_valid','expect_masked_sop_to_discard_pkt'))),'nonansi':body.replace('module struct_leaf(input logic clk, input logic rst_n, input hlsif_chk_dbg_k_controls_t k_controls,\n output logic observed);','module struct_leaf(clk,rst_n,k_controls,observed);\n input logic clk,rst_n; input hlsif_chk_dbg_k_controls_t k_controls;output logic observed;')}
  # Non-ANSI identity may reach the original conservative fallback; it must not
  # acquire the NEW precise struct root port alongside its mapped writer.
  for name,body in cases.items():
   f=tmp/(name+'.sv');f.write_text(body)
   for canon in ('0','1'):
    db=tmp/f'{name}{canon}.db';run(binary,['compile','--db',db,'--single-unit',f,'--top','struct_boundary_repro'],{'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':'1','RTL_TRACE_CANONICAL_VERIFY':'1'})
    r=query(binary,db,root);ports=[e for e in r['endpoints'] if e['kind']=='port' and e['path']==root and not e['bit_map_approximate']]
    writers=[e for e in r['endpoints'] if e['kind']=='expr'];assert not (ports and writers),(name,r)
    count+=1
  # Same leaf canonical body, distinct actual expressions and parent contexts.
  typedef=source.read_text().split('module struct_leaf')[0]
  leaf=source.read_text().split('module struct_leaf')[1].split('module struct_boundary_repro')[0]
  leaf='module struct_leaf'+leaf
  def conn(variant,high="26'h0"):
   return "{"+high+",("+variant+" ? 1'b1 : 1'b0),("+variant+" ? 1'b1 : 1'b0),arr[0][2],1'b0,1'b0,~arr[0][3]}"
  parent="module param_parent #(parameter B=1)(input logic clk,rst_n,input wire[7:0] x,output wire y);wire[7:0] arr[0:0];assign arr[0]=x;struct_leaf leaf(.clk(clk),.rst_n(rst_n),.observed(y),.k_controls("+conn('B')+"));endmodule\n"
  for reverse in (False,True):
   names=['known','unknown','xconst']
   if reverse:names.reverse()
   items={n:"struct_leaf "+n+"(.clk(clk),.rst_n(rst_n),.observed(y["+str(i)+"]),.k_controls("+conn("1'b1" if n!='unknown' else 'sel',"26'hx" if n=='xconst' else "26'h0")+"));" for i,n in enumerate(names)}
   text=typedef+leaf+parent+"module struct_mixed(input logic clk,rst_n,sel,input wire[7:0] x,output wire[4:0] y);wire[7:0] arr[0:0];assign arr[0]=x;"+''.join(items[n] for n in names)+"param_parent #(.B(0)) p0(clk,rst_n,x,y[3]);param_parent #(.B(1)) p1(clk,rst_n,x,y[4]);endmodule"
   f=tmp/f'mixed{reverse}.sv';f.write_text(text)
   for canon in ('0','1'):
    for cache in ('0','1'):
     db=tmp/f'mixed{reverse}{canon}{cache}.db';c=run(binary,['compile','--db',db,'--single-unit',f,'--top','struct_mixed'],{'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':cache,'RTL_TRACE_CANONICAL_VERIFY':'1'})
     if canon=='1':assert 'mismatched_lists=0' in c.stdout and ' map_fail=0 ' in c.stdout,c.stdout
     for name in ('known','unknown','xconst','p0.leaf','p1.leaf'):
      path='struct_mixed.'+name+'.k_controls'
      for suffix in ('','[5]','[4]'):
       r=query(binary,db,path+suffix);precise=[e for e in r['endpoints'] if e['kind']=='port' and e['path']==path and not e['bit_map_approximate']]
       assert bool(precise)==(name in ('known','p0.leaf','p1.leaf')),(reverse,canon,cache,name,suffix,r)
       if precise:assert precise[0]['bit_map']=='' and precise[0]['bit_map_encoding']=='unrestricted'
       count+=1
  # Repeated parent images exercise distinct traversal frames without changing
  # the actual certificate inputs. This checks path/Q1 parity, not entry counts.
  hot=typedef+leaf+parent+"module struct_hot(input logic clk,rst_n,input wire[7:0] x,output wire[15:0] y);for(genvar i=0;i<16;i++)begin:clone param_parent #(.B(1)) parent(clk,rst_n,x,y[i]);end endmodule"
  f=tmp/'hot.sv';f.write_text(hot)
  for canon in ('0','1'):
   for cache in ('0','1'):
    db=tmp/f'hot{canon}{cache}.db';c=run(binary,['compile','--db',db,'--single-unit',f,'--top','struct_hot'],{'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':cache,'RTL_TRACE_CANONICAL_VERIFY':'1'})
    if canon=='1':assert 'mismatched_lists=0' in c.stdout and ' map_fail=0 ' in c.stdout,c.stdout
    previous_db=None
    if a.previous_bin:
     previous_db=tmp/f'hot_previous{canon}{cache}.db';run(a.previous_bin.resolve(),['compile','--db',previous_db,'--single-unit',f,'--top','struct_hot'],{'RTL_TRACE_CANONICAL_BODIES':canon,'RTL_TRACE_BODY_CACHE':cache,'RTL_TRACE_CANONICAL_VERIFY':'1'})
    for i in range(16):
     owner=f'struct_hot.clone[{i}].parent';path=owner+'.leaf.k_controls'
     for suffix in ('','[0]','[3]','[4]','[5]'):
      r=query(binary,db,path+suffix)
      if previous_db:assert r==query(a.previous_bin.resolve(),previous_db,path+suffix),(canon,cache,i,suffix,r)
      precise=[e for e in r['endpoints'] if e['kind']=='port' and e['path']==path and not e['bit_map_approximate']]
      assert len(precise)==1 and precise[0]['bit_map']=='' and precise[0]['bit_map_encoding']=='unrestricted',(i,suffix,r)
      writers=[e for e in r['endpoints'] if e['kind']=='expr']
      assert bool(writers)==(suffix in ('','[0]','[3]')),(i,suffix,r)
      assert all(e['path']==owner+'.arr' for e in writers),(i,suffix,r)
      if suffix in ('','[4]','[5]'):assert any(s['reason']=='constant_connection' for s in r['stops'])
      count+=1
 print(f'PASS {count} struct-boundary checks; packed union query checks={union_count}; hot previous parity comparisons={320 if a.previous_bin else 0}; cache cardinality not measured; no owned DB retained')
if __name__=='__main__':main()
