"""Export contracts: backend expressions, techmap.v and simcells.v.

Every evaluable type in newcelltypes.h plus the three existing special buffers
has an explicit fixture. Width 1 and unequal wider operands exercise truncation,
extension and signedness. Numeric oracles below cover binary inputs only; in
particular mapping bweqx/eqx is not a proof of four-state equivalence. Tri-state
fixtures keep enable asserted; disabled Z/multiple drivers are outside this test.
"""
import json
import random
import subprocess
from value_consumer import decode_items

UNARY = 'not pos neg reduce_and reduce_or reduce_xor reduce_xnor reduce_bool logic_not'.split()
BINARY = 'and or xor xnor shl shr sshl sshr shift shiftx lt le eq ne eqx nex ge gt add sub mul div mod divfloor modfloor pow logic_and logic_or'.split()
GATES = 'BUF NOT AND NAND OR NOR XOR XNOR ANDNOT ORNOT MUX NMUX MUX4 MUX8 MUX16 AOI3 OAI3 AOI4 OAI4 TBUF'.split()
SPECIAL = 'buf barrier slice concat lut sop mux pmux bmux demux bwmux bweqx fa lcu alu macc macc_v2 tribuf'.split()


def specification(kind, width, signed):
    """Explicit parameter/port contracts; no production support-table calls."""
    if kind.startswith('$_'):
        gate = kind[2:-1]
        pins = dict.fromkeys(('A',) if gate in ('BUF', 'NOT') else
                             ('A', 'E') if gate == 'TBUF' else
                             tuple(chr(65+i) for i in range(int(gate[3:]) if gate[3:].isdigit() else 2)), 1)
        if gate in ('AOI3', 'OAI3'): pins = dict.fromkeys('ABC', 1)
        if gate in ('AOI4', 'OAI4'): pins = dict.fromkeys('ABCD', 1)
        for i in range({'MUX':1, 'NMUX':1, 'MUX4':2, 'MUX8':3, 'MUX16':4}.get(gate, 0)):
            pins['STUV'[i]] = 1
        return {}, pins, {'Y':1}
    name = kind[1:]
    if name in UNARY:
        return dict(A_SIGNED=signed, A_WIDTH=width, Y_WIDTH=width+1), {'A':width}, {'Y':width+1}
    if name in BINARY or name == 'alu':
        bsigned = 0 if name in ('shl','shr','sshl','sshr','pow') else signed
        asign = 0 if name == 'shiftx' else signed
        params = dict(A_SIGNED=asign, B_SIGNED=bsigned, A_WIDTH=width, B_WIDTH=width+1, Y_WIDTH=width+2)
        pins = dict(A=width, B=width+1)
        if name == 'alu': pins.update(CI=1, BI=1)
        return params, pins, dict.fromkeys(('X','Y','CO') if name == 'alu' else ('Y',), width+2)
    if name in ('buf','barrier','tribuf','bwmux','bweqx','fa','lcu'):
        pins = dict(A=width)
        if name in ('bwmux','bweqx','fa'): pins['B'] = width
        if name == 'bwmux': pins['S'] = width
        if name == 'tribuf': pins['EN'] = 1
        if name == 'fa': pins['C'] = width
        if name == 'lcu': pins = dict(P=width,G=width,CI=1)
        return dict(WIDTH=width), pins, dict.fromkeys(('X','Y') if name == 'fa' else ('CO',) if name == 'lcu' else ('Y',), width)
    if name == 'slice': return dict(A_WIDTH=width+2,Y_WIDTH=width,OFFSET=1), dict(A=width+2), dict(Y=width)
    if name == 'concat': return dict(A_WIDTH=width,B_WIDTH=width+1), dict(A=width,B=width+1), dict(Y=2*width+1)
    if name == 'lut':
        lut = sum((i.bit_count()%2)<<i for i in range(1<<width))
        return dict(WIDTH=width,LUT=f"{1<<width}'{lut:0{1<<width}b}"),dict(A=width),dict(Y=1)
    if name == 'sop': return dict(WIDTH=width,DEPTH=1,TABLE=f"{2*width}'{2:0{2*width}b}"),dict(A=width),dict(Y=1)
    if name in ('mux','pmux','bmux','demux'):
        params = dict(WIDTH=width)
        if name != 'mux': params['S_WIDTH'] = 2
        pins = dict(A=4*width if name == 'bmux' else width, S=1 if name == 'mux' else 2)
        if name in ('mux','pmux'): pins['B'] = width*(2 if name == 'pmux' else 1)
        return params,pins,dict(Y=4*width if name == 'demux' else width)
    if name == 'macc':
        bits = width.bit_length()
        config = bits | signed<<4 | width<<6 | width<<(6+bits)
        count = 6+2*bits
        return dict(A_WIDTH=2*width,B_WIDTH=1,Y_WIDTH=2*width+2,CONFIG_WIDTH=count,
                    CONFIG=f"{count}'{config:0{count}b}"),dict(A=2*width,B=1),dict(Y=2*width+2)
    if name == 'macc_v2':
        return dict(NPRODUCTS=1,NADDENDS=1,A_WIDTHS=f"16'{width:016b}",B_WIDTHS=f"16'{width:016b}",C_WIDTHS="16'0000000000000001",
                    Y_WIDTH=2*width+2,PRODUCT_NEGATED="1'0",ADDEND_NEGATED="1'0",
                    A_SIGNED=f"1'{signed}",B_SIGNED=f"1'{signed}",C_SIGNED="1'0"),dict(A=width,B=width,C=1),dict(Y=2*width+2)
    raise AssertionError(kind)


def oracle(kind, width, signed, pins, outputs, p):
    mask = (1 << outputs.get('Y', width))-1
    def integer(value, size, sign): return value-(1<<size) if sign and value>>(size-1) else value
    a,b,c = p.get('A',0),p.get('B',0),p.get('C',0)
    n = kind[1:]
    if kind.startswith('$_'):
        g=kind[2:-1]
        y={'BUF':lambda:a,'NOT':lambda:~a,'AND':lambda:a&b,'NAND':lambda:~(a&b),'OR':lambda:a|b,'NOR':lambda:~(a|b),
           'XOR':lambda:a^b,'XNOR':lambda:~(a^b),'ANDNOT':lambda:a&~b,'ORNOT':lambda:a|~b,
           'MUX':lambda:b if p['S'] else a,'NMUX':lambda:~(b if p['S'] else a),
           'AOI3':lambda:~((a&b)|c),'OAI3':lambda:~((a|b)&c),'AOI4':lambda:~((a&b)|(c&p['D'])),
           'OAI4':lambda:~((a|b)&(c|p['D'])),'TBUF':lambda:a}
        if g in ('MUX4','MUX8','MUX16'):
            choice=sum(p[k]<<i for i,k in enumerate('STUV'[:{'MUX4':2,'MUX8':3,'MUX16':4}[g]]))
            return {'Y':p[chr(65+choice)]}
        return {'Y':y[g]() & 1}
    if n == 'fa': return dict(Y=a^b^c,X=(a&b)|(a&c)|(b&c))
    if n == 'lcu':
        carry=p['CI']; out=0
        for i in range(width):
            carry=((p['G']>>i)&1)|(((p['P']>>i)&1)&carry); out |= carry<<i
        return dict(CO=out)
    if n == 'alu':
        aa=integer(a,pins['A'],signed)&mask; bb=(integer(b,pins['B'],signed)&mask) ^ (mask if p['BI'] else 0)
        carry=p['CI']; co=0
        for i in range(outputs['Y']):
            carry=(((aa>>i)&1)+((bb>>i)&1)+carry)>>1; co |= carry<<i
        return dict(X=aa^bb,Y=(aa+bb+p['CI'])&mask,CO=co)
    if n in ('macc','macc_v2'):
        aa=a&((1<<width)-1); bb=a>>width if n=='macc' else b
        return dict(Y=(integer(aa,width,signed)*integer(bb,width,signed)+(b if n=='macc' else c))&mask)
    if n in ('buf','barrier','tribuf'): y=a
    elif n=='slice': y=a>>1
    elif n=='concat': y=a|(b<<pins['A'])
    elif n=='lut': y=a.bit_count()%2
    elif n=='sop': y=a&1
    elif n=='mux': y=b if p['S'] else a
    elif n=='pmux': y=(b>>((p['S'].bit_length()-1)*width)) if p['S'] else a
    elif n=='bmux': y=a>>(p['S']*width)
    elif n=='demux': y=a<<(p['S']*width)
    elif n=='bwmux': y=(a&~p['S'])|(b&p['S'])
    elif n=='bweqx': y=~(a^b)
    else:
        aa=integer(a,pins['A'],signed if n!='shiftx' else 0)
        bb=integer(b,pins.get('B',1),signed if n not in ('shl','shr','sshl','sshr','pow') else 0)
        if n=='shiftx':
            return {'Y': ''.join(str((a>>i)&1) if 0<=i<pins['A'] else 'x' for i in reversed(range(bb,bb+outputs['Y'])))}
        # Division by zero has undefined bits that mapping may refine. Keep
        # export/gold coverage, but make no numeric assertion for that domain.
        if n in ('div','mod','divfloor','modfloor') and bb==0: return {}
        trunc = (abs(aa)//abs(bb)) * (-1 if (aa<0)!=(bb<0) else 1) if bb else 0
        functions={'not':lambda:~aa,'pos':lambda:aa,'neg':lambda:-aa,'reduce_and':lambda:int(a==(1<<pins['A'])-1),
                   'reduce_or':lambda:int(bool(a)),'reduce_bool':lambda:int(bool(a)),'reduce_xor':lambda:a.bit_count()%2,
                   'reduce_xnor':lambda:1-a.bit_count()%2,'logic_not':lambda:int(not a),
                   'and':lambda:aa&bb,'or':lambda:aa|bb,'xor':lambda:aa^bb,'xnor':lambda:~(aa^bb),
                   'shl':lambda:aa<<bb,'sshl':lambda:aa<<bb,'shr':lambda:(aa&mask)>>bb,'sshr':lambda:aa>>bb,
                   'shift':lambda:((aa&mask)>>bb) if bb>=0 else aa<<-bb,
                   'lt':lambda:aa<bb,'le':lambda:aa<=bb,'eq':lambda:aa==bb,'eqx':lambda:aa==bb,
                   'ne':lambda:aa!=bb,'nex':lambda:aa!=bb,'ge':lambda:aa>=bb,'gt':lambda:aa>bb,
                   'add':lambda:aa+bb,'sub':lambda:aa-bb,'mul':lambda:aa*bb,'div':lambda:trunc,
                   'mod':lambda:aa-trunc*bb,'divfloor':lambda:aa//bb,'modfloor':lambda:aa%bb,'pow':lambda:aa**bb,
                   'logic_and':lambda:bool(a) and bool(b),'logic_or':lambda:bool(a) or bool(b)}
        y=functions[n]()
    return dict(Y=int(y)&mask)


def check_export_matrix(suite, work):
    kinds=['$'+name for name in UNARY+BINARY+SPECIAL]+['$_'+name+'_' for name in GATES]
    suite.run(f'test_cellfuzz_type_plan comb-types {work}/comb-types.json')
    official=set(json.loads((work/'comb-types.json').read_text()))
    assert set(kinds)==official|{'$barrier','$tribuf','$_TBUF_'}, (set(kinds)-official,official-set(kinds))
    lines=['autoidx 1','attribute \\top 1','module \\comb_matrix']; cases=[]; port_id=0
    for number,kind in enumerate(kinds):
        for width,sign in ((1,0),(3,0),(3,1)) if not kind.startswith('$_') else ((1,0),):
            params,pins,outputs=specification(kind,width,sign)
            stem=f'c{number}_{width}_{sign}'
            names={pin:stem+'_'+pin for pin in pins|outputs}
            for pin,size in (pins|outputs).items():
                port_id+=1
                lines.append(f'  wire width {size} '+('input' if pin in pins else 'output')+f' {port_id} \\{names[pin]}')
            lines.append(f'  cell {kind} \\{stem}')
            lines += [f'    parameter \\{key} {value}' for key,value in params.items()]
            lines += [f'    connect \\{pin} \\{name}' for pin,name in names.items()]
            lines += ['  end']; cases.append((kind,width,sign,pins,outputs,names))
    lines+=['end']; source=work/'comb-matrix.il'; source.write_text('\n'.join(lines)+'\n')
    # Raw CellTypes::eval clears both signs for mixed-sign $pow. Export the
    # reference too; the independent mathematical oracle still checks semantics.
    case=suite.prepare('comb-matrix',f'read_rtlil {source}','comb_matrix',options='-chunk-width 33',gold_verilog=True)
    expected_outputs = {('\\'+names[pin].rsplit('_',1)[0], '\\'+pin)
                        for _,_,_,_,outputs,names in cases for pin in outputs}
    observed_outputs = {(item['cell'], item['port']): item for item in case[1]['items'] if item['role']=='output'}
    assert set(observed_outputs)==expected_outputs, ('complete output observations',expected_outputs-set(observed_outputs))
    assert len(observed_outputs)==sum(item['role']=='output' for item in case[1]['items']), 'duplicate output observation'
    expected_selectors = {}
    for kind,_,_,pins,_,names in cases:
        selectors = ('S',) if kind in ('$mux','$pmux','$bmux','$demux','$bwmux','$_MUX_','$_NMUX_') else \
                    tuple('STUV'[:{'$_MUX4_':2,'$_MUX8_':3,'$_MUX16_':4}.get(kind,0)])
        for pin in selectors:
            expected_selectors['\\'+names[pin].rsplit('_',1)[0], '\\'+pin] = names[pin],pins[pin]
    observed_selectors = {(item['cell'],item['port']):item for item in case[1]['items'] if item['role']=='mux_select'}
    assert set(observed_selectors)==set(expected_selectors), 'complete mux selector observations'
    assert len(observed_selectors)==sum(item['role']=='mux_select' for item in case[1]['items'])
    rng=random.Random(0xf2); vectors=[]
    for step in range(40):
        vector={}
        for kind,width,sign,pins,outputs,names in cases:
            values={pin:(0 if step==0 else (1<<size)-1 if step==1 else rng.getrandbits(size)) for pin,size in pins.items()}
            if kind=='$pmux': values['S']=(0,1,2)[step%3]
            if kind in ('$tribuf','$_TBUF_'): values['EN' if kind=='$tribuf' else 'E']=1
            vector.update({names[pin]:value for pin,value in values.items()})
        vectors.append(vector)
    samples,output_names=suite.simulate(case,vectors,'tribuf -logic;')
    # tribuf -logic is simulation-only lowering after final Verilog validation.
    # All enables are asserted; no disabled-Z equivalence is claimed.
    for timestamp,values in samples:
        decoded=decode_items(case[1],{p['name']:values[f'a_block{i}'] for i,p in enumerate(case[1]['transport']['ports'])})
        for key,(name,width) in expected_selectors.items():
            item=observed_selectors[key]
            assert item['width']==width and decoded[item['monitor_id']]==values['stim_'+name], ('independent selector',key)
        for kind,width,sign,pins,outputs,names in cases:
            stimulus={pin:int(values['stim_'+names[pin]],2) for pin in pins}
            for pin,value in oracle(kind,width,sign,pins,outputs,stimulus).items():
                bits=value if isinstance(value,str) else format(value,f'0{outputs[pin]}b')
                actual=values[f'a_func{output_names.index(names[pin])}']
                assert all(e=='x' or a==e for a,e in zip(actual,bits)) and len(actual)==len(bits), ('independent export value',kind,width,sign,pin,timestamp,actual,bits)
                item=observed_outputs['\\'+names[pin].rsplit('_',1)[0], '\\'+pin]
                assert item['width']==outputs[pin] and len(decoded[item['monitor_id']])==len(bits)
                assert all(e=='x' or a==e for a,e in zip(decoded[item['monitor_id']],bits)), ('independent observation',kind,pin)
    print(f'Export matrix: {len(kinds)} types, {len(cases)} parameter cases, 40 binary vectors; both mapping omissions rejected.',flush=True)
