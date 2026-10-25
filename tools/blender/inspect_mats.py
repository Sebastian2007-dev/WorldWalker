import bpy
for m in bpy.data.materials:
    if not m.node_tree: continue
    imgs=[]
    for n in m.node_tree.nodes:
        if n.type=='TEX_IMAGE' and n.image:
            outs=[(l.to_node.type,l.to_socket.name) for o in n.outputs for l in o.links]
            imgs.append((n.image.name, n.image.packed_file is not None, outs[0] if outs else None))
    print("  MAT", m.name, imgs)
