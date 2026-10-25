import bpy, sys
for o in bpy.data.objects:
    if o.type != 'MESH': 
        print("  OBJ", o.name, o.type); continue
    me = o.data
    tris = sum(len(p.vertices)-2 for p in me.polygons)
    print(f"  MESH {o.name} tris={tris} verts={len(me.vertices)} dims={tuple(round(d,2) for d in o.dimensions)} mats={[m.name for m in me.materials if m]} mods={[m.type for m in o.modifiers]} parent={o.parent.name if o.parent else None}")
for m in bpy.data.materials:
    if m.use_nodes:
        nodes = [n.type for n in m.node_tree.nodes]
        print("  MAT", m.name, "blend", getattr(m,'blend_method',None), sorted(set(nodes)))
