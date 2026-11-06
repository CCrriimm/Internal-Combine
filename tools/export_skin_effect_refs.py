"""Export observed skin effect references from captured local tables, without running the game."""
import argparse,ast,csv,hashlib,json,os,struct
from collections import defaultdict,Counter
from pathlib import Path


def build_references(rows,upgrades,is_temporary):
    levels,roots={},{}
    for root,group in upgrades.items():
        for level,value in group.items():
            if isinstance(value,dict) and value.get('item_id'):
                iid=int(value['item_id']);levels[iid]=int(level);roots[iid]=int(root)
    groups=defaultdict(list)
    for proto in rows.values():
        if not isinstance(proto,dict) or is_temporary(proto):continue
        iid=int(proto['id']);sub=int(proto.get('sub_type') or 0)
        kind='Character' if sub==300 else 'Gun' if sub in (510,520,530,540,550,560) else 'Knife' if sub in (201,202,203,206,207,210) else None
        if kind is None or (kind!='Character' and int(proto.get('quality') or 0)<4):continue
        template=int((proto.get('guise') or {}).get('template_id') or 0)
        identity=('root',roots[iid]) if iid in roots else ('template',template) if template else ('hero',proto.get('hero_id'),proto.get('hero_skin_id',iid))
        groups[(kind,identity)].append(proto)
    output=[];families=[]
    for (kind,identity),members in sorted(groups.items(),key=lambda x:str(x[0])):
        selected=max(members,key=lambda p:(levels.get(int(p['id']),0),-int(p['id'])))
        base={'kind':kind,'skin':selected.get('name',''),'selected_item_id':int(selected['id']),
              'template_id':int((selected.get('guise') or {}).get('template_id') or 0),
              'selected_level':levels.get(int(selected['id']))}
        family=[]
        for proto in members:
            source={'source_item_id':int(proto['id']),'source_level':levels.get(int(proto['id']))}
            for field,role in [('kill_hud_id','Kill-feed/HUD reference; not a kill VFX or deathbox ID'),
                               ('hit_effect','Hit-effect reference; not evidence of an elimination effect')]:
                if proto.get(field):
                    family.append({**base,**source,'field':field,'reference_id':proto[field],'role':role,'confidence':'observed-field'})
            for pair in proto.get('bind_items') or []:
                if isinstance(pair,(list,tuple)) and pair:
                    family.append({**base,**source,'field':'bind_items','reference_id':int(pair[0]),
                                   'role':'Bundled item; effect/deathbox role unresolved','confidence':'observed-binding-unresolved-role'})
        output.extend(family);families.append({**base,'member_item_ids':sorted(int(p['id']) for p in members),
            'member_template_ids':sorted({int((p.get('guise') or {}).get('template_id') or 0) for p in members}),
            'references':family})
    return families,output


def main():
    project=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser()
    parser.add_argument('--rows',type=Path,default=project/'reference/skin-catalog/skin_rows.json')
    parser.add_argument('--upgrades',type=Path,default=project/'reference/skin-catalog/upgrade_levels.json')
    parser.add_argument('--out',type=Path,default=project/'reports/skin-effect-references')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    rows=json.loads(args.rows.read_text(encoding='utf-8'))
    upgrades=json.loads(args.upgrades.read_text(encoding='utf-8'));upgrades=upgrades.get('data',upgrades)
    tree=ast.parse((project/'pyexec/bs_runtime.py').read_text(encoding='utf-8-sig'))
    node=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='_item_transient_check')
    ns={};exec(compile(ast.Module(body=[node],type_ignores=[]),'skin-filter','exec'),ns)
    families,refs=build_references(rows,upgrades,ns['_item_transient_check'])
    for ref in refs:
        source=rows[str(ref['source_item_id'])]
        assert ref['reference_id']==source[ref['field']] if ref['field']!='bind_items' else any(int(p[0])==ref['reference_id'] for p in source['bind_items'])
    counts=Counter(r['field'] for r in refs)
    unique={field:len({json.dumps(r['reference_id'],sort_keys=True) for r in refs if r['field']==field}) for field in counts}
    active=[]
    local=os.environ.get('LOCALAPPDATA')
    inventory=Path(local)/'CiyE/BloodStrike/skin_inventory.bin' if local else None
    inventory_status='not available'
    if inventory and inventory.is_file():
        raw=inventory.read_bytes()
        if len(raw)>=12:
            magic,version,count=struct.unpack_from('<III',raw)
            if magic==0x42534956 and version==1 and count<=4096 and len(raw)==12+count*64:
                inventory_status='valid'
                for i in range(count):
                    typ,group,selection,on,name=struct.unpack_from('<IIII48s',raw,12+i*64)
                    if not on:continue
                    kind={1:'Character',2:'Gun',3:'Knife'}.get(typ)
                    matches=[f for f in families if f['kind']==kind and (selection in f['member_item_ids'] if typ==1 else selection in f['member_template_ids'])]
                    active.append({'name':name.split(b'\0')[0].decode('utf-8','replace'),'type':kind,'selection_id':selection,'matching_families':matches})
    active_refs=[]
    for selection in active:
        for family in selection['matching_families']:
            for ref in family['references']:
                active_refs.append({'equipped_skin':selection['name'],'equipped_selection_id':selection['selection_id'],**ref})
    source_rows = args.rows.resolve().relative_to(project).as_posix() if args.rows.resolve().is_relative_to(project) else args.rows.name
    metadata={'source_rows':source_rows,'source_sha256':hashlib.sha256(args.rows.read_bytes()).hexdigest(),
              'catalog_families':len(families),'families_with_references':sum(bool(f['references']) for f in families),
              'reference_rows_by_field':dict(counts),'unique_reference_ids_by_field':unique,
              'active_inventory_status':inventory_status,'active_selection_count':len(active),
              'active_selection_matches':sum(bool(a['matching_families']) for a in active),
              'active_reference_rows':len(active_refs),
              'limitations':['Captured catalog only; no effect table names/models or deathbox roles are inferred from numeric prefixes.',
                 'References across upgrade levels are associations, not proof that each effect activates on the highest level.',
                 'Offline export from captured tables; no live process access or visual validation performed by this tool.']}
    (args.out/'references.json').write_text(json.dumps({'metadata':metadata,'families':families,'active_selections':active},ensure_ascii=False,indent=2),encoding='utf-8')
    fields=['kind','skin','selected_item_id','template_id','selected_level','source_item_id','source_level','field','reference_id','role','confidence']
    with (args.out/'references.csv').open('w',encoding='utf-8-sig',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(refs)
    with (args.out/'active-skin-references.csv').open('w',encoding='utf-8-sig',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=['equipped_skin','equipped_selection_id']+fields)
        writer.writeheader();writer.writerows(active_refs)
    examples=[f for f in families if f['references'] and any(word in f['skin'] for word in ('Tiadraco','Eternal Oracle','Inkforge','Guard','Fright','Empyrean'))][:10]
    lines=['# Skin effect reference findings','',
      f"Catalog families: {len(families)}; families with references: {metadata['families_with_references']}.",
      f"Unique IDs: {unique}.",'',
      'kill_hud_id is kill-feed/HUD metadata. hit_effect describes a hit reference. Neither proves a kill VFX or a deathbox model.',
      'bind_items IDs are verified links to bundled items; their roles remain unresolved because the corresponding item/effect tables are absent from this capture.',
      'This offline export does not confirm deathbox effects or modify the game.','',
      '| Skin family | Highest captured item | Observed reference IDs |','|---|---:|---|']
    for family in examples:
        text='; '.join(f"{field}: "+', '.join(map(str,sorted({r['reference_id'] for r in family['references'] if r['field']==field}))) for field in ('kill_hud_id','hit_effect','bind_items') if any(r['field']==field for r in family['references']))
        lines.append(f"| {family['skin']} | {family['selected_item_id']} | {text} |")
    lines+=['','## Current saved Inventory','','| Saved skin | Observed reference IDs |','|---|---|']
    for selection in active:
        refs_for_selection=[r for r in active_refs if r['equipped_selection_id']==selection['selection_id']]
        text='; '.join(f"{field}: "+', '.join(map(str,sorted({r['reference_id'] for r in refs_for_selection if r['field']==field})))
                      for field in ('kill_hud_id','hit_effect','bind_items') if any(r['field']==field for r in refs_for_selection))
        lines.append(f"| {selection['name']} | {text or 'No references in captured fields'} |")
    lines+=['',f"Active Inventory: {len(active)} saved selections; {metadata['active_selection_matches']} matched to captured families (status: {inventory_status}).",
            'Full source item/level traceability is in references.csv and references.json. Sources were not written or changed.']
    (args.out/'README.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(json.dumps(metadata,ensure_ascii=False,indent=2))


if __name__=='__main__':main()
