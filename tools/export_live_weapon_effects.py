"""Read current menu gun skins, upgrade families and linked cosmetic items."""
import argparse
import json
from pathlib import Path

import frida
from probe_custom_room import BRIDGE


PROBE = r'''
import builtins,json,struct,sys,time
from pathlib import Path
started=time.perf_counter()
m=sys.modules.get('m_view_pipeline') or sys.modules.get('bs_runtime')
if m is None:raise RuntimeError('Runtime catalog is not available')
def table(name):
    module=sys.modules.get('gclient.data.'+name)
    return getattr(module,'data',{}) if module is not None else {}
def plain(value,depth=0):
    if value is None or isinstance(value,(str,bool,int,float)):return value
    if depth>10:return {'unresolved_type':type(value).__name__}
    if hasattr(value,'items'):return {str(k):plain(v,depth+1) for k,v in value.items()}
    if isinstance(value,(list,tuple,set)):return [plain(v,depth+1) for v in value]
    return {'unresolved_type':type(value).__name__}
lobby=table('lobby_item_data')
templates=table('gun_skin_template_data')
features=table('item_feature_data')
icons=table('feature_icon_data')
upgrades=table('lobby_item_upgrade_data')
roots={}
for root,group in upgrades.items():
    for level,entry in group.items():
        if entry.get('item_id'):roots[int(entry['item_id'])]=(int(root),int(level))
raw=Path(m.CATALOG_PATH).read_bytes()
magic,version,hero_count,weapon_count=struct.unpack_from('<IIII',raw)
if magic!=0x42534354 or len(raw)!=16+(hero_count+weapon_count)*64:
    raise RuntimeError('Runtime catalog format mismatch')
skins=[];source_items={};bound_ids=set();feature_rows={};icon_rows={};template_rows={}
cache=getattr(m._skin_selection,'template_items',{})
for index in range(weapon_count):
    template_id,gun_id,quality,kind,name=struct.unpack_from('<IIII48s',raw,16+(hero_count+index)*64)
    if kind!=1:continue
    proto=cache.get((2,template_id))
    item_id=int(proto.get('id') or 0) if proto is not None else 0
    root,level=roots.get(item_id,(0,0))
    members=[]
    if root:
        for member_level,entry in upgrades.get(root,{}).items():
            if entry.get('item_id'):members.append(int(entry['item_id']))
    if item_id and item_id not in members:members.append(item_id)
    skins.append({'skin':name.split(b'\0')[0].decode('utf-8','replace'),
                  'template_id':template_id,'gun_id':gun_id,'quality':quality,
                  'item_id':item_id,'upgrade_root':root,'upgrade_level':level,
                  'family_item_ids':members})
    template_rows[str(template_id)]=plain(templates.get(template_id))
    for iid in members:
        source=lobby.get(iid)
        if source is None:continue
        source_items[str(iid)]=plain(source)
        member_template_id=int((source.get('guise') or {}).get('template_id') or 0)
        if member_template_id and str(member_template_id) not in template_rows:
            template_rows[str(member_template_id)]=plain(templates.get(member_template_id))
        feature_rows[str(iid)]=plain(features.get(iid))
        icon_rows[str(iid)]=plain(icons.get(iid))
        for pair in source.get('bind_items') or ():
            if pair:bound_ids.add(int(pair[0]))
bound={}
for iid in sorted(bound_ids):
    bound[str(iid)]={'item':plain(lobby.get(iid)),
                     'feature':plain(features.get(iid))}
effect_ids=set();model_ids=set()
def add_ids(value,target):
    if isinstance(value,int) and not isinstance(value,bool) and value>0:target.add(value)
    elif hasattr(value,'values'):
        for child in value.values():add_ids(child,target)
    elif isinstance(value,(list,tuple)):
        for child in value:add_ids(child,target)
for proto in template_rows.values():
    if not isinstance(proto,dict):continue
    for field in ('kill_fx','team_dead_fx','kill_avatar_effect_sfx'):
        add_ids(proto.get(field),effect_ids)
    for field in ('kill_team_extra_model_id','kill_single_extra_model_id','kill_team_extra_model_id_list'):
        add_ids(proto.get(field),model_ids)
for row in bound.values():
    proto=row['item'] or {}
    for field in ('model_id','item_ground_model_id'):add_ids(proto.get(field),model_ids)
hall_util=sys.modules.get('gshare.hall_item_util')
deathbox_subtype=int(hall_util.consts.ItemSubType.DeathBox) if hall_util else None
g=getattr(builtins,'genv',None)
equipped_deathbox={}
for name in ('player','avatar'):
    entity=getattr(g,name,None)
    value=getattr(entity,'death_box_skin_id',None)
    if value is None or isinstance(value,(int,str)):equipped_deathbox[name]=value
equipped_deathbox_items={str(i):plain(lobby.get(i)) for i in equipped_deathbox.values() if isinstance(i,int) and i>0}
for proto in equipped_deathbox_items.values():
    if proto:add_ids(proto.get('model_id'),model_ids)
effects=table('effect_data');models=table('unit_model_data')
model_rows={str(i):plain(models.get(i)) for i in sorted(model_ids)}
for proto in model_rows.values():
    if proto:add_ids(proto.get('effect_ids'),effect_ids)
effect_rows={str(i):plain(effects.get(i)) for i in sorted(effect_ids)}
data_modules=[]
for name,module in list(sys.modules.items()):
    if name.startswith('gclient.data.') and any(t in name.lower() for t in ('effect','feature','drop','box','death','kill_','model','weapon')):
        data_modules.append(name)
result={'status':{'captured_at_unix':time.time(),'capture_seconds':time.perf_counter()-started,
                  'catalog_path':'ipc/catalog.bin','gun_skins':len(skins),
                  'killfx_enabled':m._flag(m._read_cfg(),getattr(m,'F_FORCE_KILLFX',0)),
                  'killfx_filled':getattr(m,'_killfx_filled',None)},
        'skins':skins,'source_items':source_items,'templates':template_rows,
        'features':feature_rows,'feature_icons':icon_rows,'bound_items':bound,
        'effect_resources':effect_rows,'model_resources':model_rows,
        'deathbox_subtype':deathbox_subtype,'equipped_deathbox':equipped_deathbox,
        'equipped_deathbox_items':equipped_deathbox_items,
        'candidate_data_modules':sorted(data_modules)}
with open(OUTPUT,'w',encoding='utf-8') as f:json.dump(result,f,indent=2,ensure_ascii=False)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError('Choose a new evidence output')
    output.parent.mkdir(parents=True, exist_ok=True)
    source = PROBE.replace('OUTPUT', repr(str(output)))
    compile(source, '<weapon-effects-export>', 'exec')
    session = frida.attach(args.pid)
    try:
        script = session.create_script(BRIDGE)
        script.load()
        result = script.exports_sync.collect('exec(%r,{"__name__":"__weapon_effects_export__"})' % source)
        if result:
            raise RuntimeError('Embedded Python export failed: %d' % result)
    finally:
        session.detach()
    data = json.loads(output.read_text(encoding='utf-8'))
    assert len(data['skins']) == data['status']['gun_skins']
    assert all(s['template_id'] and s['gun_id'] for s in data['skins'])
    print(json.dumps(data['status'], indent=2))
    print('Source items=%d, bound items=%d' % (len(data['source_items']), len(data['bound_items'])))


if __name__ == '__main__':
    main()
