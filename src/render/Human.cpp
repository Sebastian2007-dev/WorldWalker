#include "render/Human.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <glm/gtx/quaternion.hpp>
#include <stb_image.h>

namespace render {
namespace {
template<class T> void read(std::ifstream& f, T* p, size_t n) {
    if (n > 20000000 || !f.read(reinterpret_cast<char*>(p), std::streamsize(n * sizeof(T))))
        throw std::runtime_error("Incomplete human data");
}
uint32_t count(std::ifstream& f) { uint32_t n; read(f, &n, 1); if(n > 2000000) throw std::runtime_error("Invalid human count"); return n; }
template<class T> void array(std::ifstream& f, std::vector<T>& v, size_t n) { v.resize(n); read(f, v.data(), n); }
glm::vec3 vec(const nlohmann::json& a) { return {a.at(0).get<float>(),a.at(1).get<float>(),a.at(2).get<float>()}; }
float value(const nlohmann::json& j, const std::string& key, float fallback) {
    if (!j.contains(key) || !j[key].is_number()) return fallback;
    float v=j[key].get<float>(); return std::isfinite(v) ? std::clamp(v,0.0f,1.0f) : fallback;
}
float toLinear(float c) { return c<=0.04045f ? c/12.92f : std::pow((c+0.055f)/1.055f,2.4f); }
bool validColor(const nlohmann::json& c) {
    if(!c.is_array() || c.size()!=3) return false;
    for(auto& x:c) if(!x.is_number() || !std::isfinite(x.get<float>())) return false;
    return true;
}
}
const std::vector<Human::HairShade>& Human::hairShades() {
    static const std::vector<HairShade> shades={
        {"Schwarz",{0.04f,0.035f,0.03f}},{"Dunkelbraun",{0.17f,0.11f,0.07f}},{"Braun",{0.33f,0.21f,0.12f}},
        {"Hellbraun",{0.52f,0.37f,0.22f}},{"Kastanie",{0.42f,0.17f,0.08f}},{"Kupfer",{0.70f,0.30f,0.12f}},
        {"Dunkelblond",{0.62f,0.50f,0.32f}},{"Blond",{0.85f,0.72f,0.47f}},{"Platin",{0.92f,0.88f,0.78f}},
        {"Grau",{0.55f,0.54f,0.52f}},{"Weiß",{0.88f,0.88f,0.86f}},
        {"Nachtblau",{0.12f,0.2f,0.5f}},{"Rosé",{0.85f,0.4f,0.55f}},{"Smaragd",{0.1f,0.45f,0.3f}}};
    return shades;
}
glm::vec3 Human::meanColor(const std::string& texture) {
    if(auto it=meanColors_.find(texture); it!=meanColors_.end()) return it->second;
    glm::vec3 result(0.2f); float lut[256];
    for(int i=0;i<256;++i) lut[i]=toLinear(float(i)/255.0f);
    int w=0,h=0,n=0;
    if(stbi_uc* px=stbi_load((directory_/texture).string().c_str(),&w,&h,&n,4)) {
        glm::dvec3 sum(0); size_t count=0;
        for(size_t i=0;i<size_t(w)*size_t(h);i+=3) {
            const stbi_uc* p=px+i*4; if(p[3]<128) continue;
            sum+=glm::dvec3(lut[p[0]],lut[p[1]],lut[p[2]]); ++count;
        }
        if(count) result=glm::vec3(sum/double(count));
        stbi_image_free(px);
    }
    return meanColors_[texture]=glm::max(result,glm::vec3(0.002f));
}
bool Human::load(const std::filesystem::path& directory) {
    other_.reset();
    if(!std::filesystem::exists(directory/"female"/"human.json")) return loadDirectory(directory);
    other_=std::make_unique<Human>();
    if(!other_->loadDirectory(directory/"female") || !loadDirectory(directory/"male")) { other_.reset(); base_.clear(); return false; }
    return true;
}
void Human::swapData(Human& o) {
    std::swap(directory_,o.directory_); std::swap(meta_,o.meta_); std::swap(base_,o.base_); std::swap(render_,o.render_);
    std::swap(triangles_,o.triangles_); std::swap(joints_,o.joints_); std::swap(weights_,o.weights_);
    std::swap(stencilStart_,o.stencilStart_); std::swap(stencil_,o.stencil_); std::swap(facePoints_,o.facePoints_);
    std::swap(neighborStart_,o.neighborStart_); std::swap(neighbors_,o.neighbors_); std::swap(targets_,o.targets_);
    std::swap(realism_,o.realism_); std::swap(assets_,o.assets_); std::swap(meanColors_,o.meanColors_);
    std::swap(smoothRest_,o.smoothRest_); std::swap(around_,o.around_);
}
void Human::activate(bool female) {
    if(other_ && (meta_.value("sex",std::string())=="female")!=female) swapData(*other_);
}
void Human::completeSelection() {
    if(!state.contains("assets") || !state["assets"].is_array()) return;
    std::set<std::string> present;
    for(auto& a:meta_["assets"]) for(auto& s:state["assets"]) if(s==a["name"]) present.insert(a["kind"].get<std::string>());
    for(auto& a:meta_["assets"]) {
        std::string kind=a["kind"];
        if(kind=="clothes" || kind=="hair" || kind=="genitals" || present.count(kind)) continue;
        state["assets"].push_back(a["name"]); present.insert(kind);
    }
    int skin=state.value("skin",0);
    if(skin<0 || size_t(skin)>=meta_["skins"].size()) {
        std::string wanted=state.value("template",std::string("male"))=="female"?"young_caucasian_female":"young_caucasian_male";
        state["skin"]=0;
        for(size_t i=0;i<meta_["skins"].size();++i) if(meta_["skins"][i]["name"]==wanted) state["skin"]=i;
    }
}
bool Human::loadDirectory(const std::filesystem::path& directory) {
    try {
        directory_=directory;
        std::ifstream jf(directory / "human.json"); jf >> meta_;
        std::ifstream f(directory / "base.bin", std::ios::binary);
        array(f,base_,count(f));
        array(f,render_,count(f));
        array(f,triangles_,size_t(count(f))*3);
        array(f,joints_,base_.size()); array(f,weights_,base_.size());
        for (auto& v:render_) if(v.base>=base_.size()) throw std::runtime_error("Invalid body vertex");
        for (auto i:triangles_) if(i>=render_.size()) throw std::runtime_error("Invalid body triangle");
        std::ifstream tf(directory / "targets.bin",std::ios::binary);
        targets_.resize(count(tf));
        for(auto& t:targets_) {
            auto n=count(tf); array(tf,t.indices,n); array(tf,t.delta,n);
            for(auto i:t.indices) if(i>=base_.size()) throw std::runtime_error("Invalid morph vertex");
        }
        realism_.clear();
        if(std::ifstream rf(directory / "realism.bin",std::ios::binary); rf) {
            realism_.resize(count(rf));
            for(auto& t:realism_) {
                auto n=count(rf); array(rf,t.indices,n); array(rf,t.delta,n);
                for(auto i:t.indices) if(i>=base_.size()) throw std::runtime_error("Invalid realism vertex");
            }
        }
        for(auto& r:meta_.value("realism",nlohmann::json::array()))
            if(r.value("target",-1)<0 || size_t(r.value("target",-1))>=realism_.size()) throw std::runtime_error("Invalid realism entry");
        auto validTarget=[&](const nlohmann::json& t) { return t.is_number_integer() && t.get<int>()>=0 && size_t(t.get<int>())<targets_.size(); };
        for(auto& m:meta_.value("modifiers",nlohmann::json::array())) for(auto& t:m.at("targets"))
            if(t.is_object() ? !std::all_of(t.begin(),t.end(),validTarget) : !validTarget(t)) throw std::runtime_error("Invalid modifier target");
        assets_.clear();
        for(auto& a:meta_["assets"]) {
            Asset asset; std::ifstream af(directory/a.at("file").get<std::string>(),std::ios::binary);
            array(af,asset.vertices,count(af)); array(af,asset.indices,size_t(count(af))*3);
            uint8_t scaled; read(af,&scaled,1); asset.scaled=scaled!=0;
            for(int k=0;k<3;++k) { read(af,&asset.scaleA[k],1); read(af,&asset.scaleB[k],1); read(af,&asset.reference[k],1); }
            array(af,asset.deleted,count(af));
            // Optional morphs (tools/blender/bake_genitals.py): name, then sparse offset deltas in metres.
            if(af.peek()!=std::char_traits<char>::eof()) for(uint32_t m=count(af);m>0;--m) {
                std::string name(count(af),'\0'); read(af,name.data(),name.size());
                auto& t=asset.morphs[name]; auto n=count(af); array(af,t.indices,n); array(af,t.delta,n);
                // "body:<x>" morphs move base vertices (applied while the asset is shown, see "bodyMorphs").
                size_t limit=name.rfind("body:",0)==0?base_.size():asset.vertices.size();
                for(auto i:t.indices) if(i>=limit) throw std::runtime_error("Invalid asset morph vertex");
            }
            if(a.contains("rig")) {
                std::ifstream rf(directory/a.at("rig").get<std::string>(),std::ios::binary);
                array(rf,asset.rig,count(rf)); array(rf,asset.rigWeights,count(rf));
                if(asset.rigWeights.size()!=asset.vertices.size()) throw std::runtime_error("Invalid rig weights");
                for(size_t k=0;k<asset.rig.size();++k) {
                    if(asset.rig[k].parent<-1 || asset.rig[k].parent>=int(k)) throw std::runtime_error("Invalid rig bone");
                    for(auto* q:{&asset.rig[k].head,&asset.rig[k].tail}) for(auto i:q->refs) if(i>=base_.size()) throw std::runtime_error("Invalid rig bone");
                }
                for(auto& w:asset.rigWeights) for(int k=0;k<4;++k) if(w.weights[k] && w.bones[k]>=asset.rig.size()) throw std::runtime_error("Invalid rig weight");
            }
            for(auto& v:asset.vertices) for(auto i:v.refs) if(i>=base_.size()) throw std::runtime_error("Invalid proxy vertex");
            for(auto i:asset.indices) if(i>=asset.vertices.size()) throw std::runtime_error("Invalid proxy triangle");
            for(auto i:asset.deleted) if(i>=base_.size()) throw std::runtime_error("Invalid hidden vertex");
            for(int k=0;k<3;++k) if(asset.scaleA[k]>=base_.size() || asset.scaleB[k]>=base_.size() || std::abs(asset.reference[k])<1e-6f)
                throw std::runtime_error("Invalid proxy scale");
            assets_.push_back(std::move(asset));
        }
        if(meta_["bones"].empty() || meta_["bones"].size()>Character::kMaxBones) throw std::runtime_error("Invalid human skeleton");
        for(size_t b=0;b<meta_["bones"].size();++b) {
            auto& bone=meta_["bones"][b]; int p=bone.at("parent");
            if(p < -1 || p>=int(b)) throw std::runtime_error("Invalid bone parent");
            for(auto key:{"head","tail"}) for(auto& i:bone.at(key)) if(i.get<size_t>()>=base_.size()) throw std::runtime_error("Invalid joint");
        }
        subdivide(); smoothRest_.clear(); around_.clear(); reset(); return true;
    } catch(const std::exception& e) { std::fprintf(stderr,"Human: %s\n",e.what()); base_.clear(); return false; }
}
void Human::subdivide() {
    // The bake fans every quad into two consecutive triangles (a,b,c),(a,c,d); join them back into quads.
    std::vector<std::vector<uint32_t>> faces;
    for(size_t i=0;i<triangles_.size();i+=3) {
        const uint32_t* t=&triangles_[i];
        if(i+5<triangles_.size() && t[3]==t[0] && t[4]==t[2]) { faces.push_back({t[0],t[1],t[2],t[5]}); i+=3; }
        else faces.push_back({t[0],t[1],t[2]});
    }
    const uint32_t V=uint32_t(base_.size());
    auto key=[](uint32_t a,uint32_t b) { return a<b?uint64_t(a)<<32|b:uint64_t(b)<<32|a; };
    std::map<uint64_t,uint32_t> edgeIds;                     // base edge -> index
    std::vector<std::pair<uint32_t,uint32_t>> edges;
    std::vector<std::vector<uint32_t>> edgeFaces;
    std::vector<std::vector<uint32_t>> vertexFaces(V), vertexEdges(V);
    for(uint32_t f=0;f<faces.size();++f) for(size_t k=0;k<faces[f].size();++k) {
        uint32_t a=render_[faces[f][k]].base, b=render_[faces[f][(k+1)%faces[f].size()]].base;
        auto [it,added]=edgeIds.try_emplace(key(a,b),uint32_t(edges.size()));
        if(added) { edges.push_back({a,b}); edgeFaces.emplace_back(); vertexEdges[a].push_back(it->second); vertexEdges[b].push_back(it->second); }
        edgeFaces[it->second].push_back(f);
        vertexFaces[a].push_back(f);
    }
    const uint32_t E=uint32_t(edges.size());
    facePoints_=V+E;
    // Stencils as weights over base vertices.
    using Weights=std::map<uint32_t,float>;
    auto addFace=[&](Weights& w,uint32_t f,float s) { for(auto r:faces[f]) w[render_[r].base]+=s/float(faces[f].size()); };
    stencilStart_.assign(1,0); stencil_.clear();
    auto emit=[&](const Weights& w) { for(auto& [i,x]:w) stencil_.push_back({i,x}); stencilStart_.push_back(uint32_t(stencil_.size())); };
    for(uint32_t v=0;v<V;++v) {
        Weights w;
        std::vector<uint32_t> border;
        for(auto e:vertexEdges[v]) if(edgeFaces[e].size()!=2) border.push_back(edges[e].first==v?edges[e].second:edges[e].first);
        float n=float(vertexEdges[v].size());
        if(vertexFaces[v].empty() || (!border.empty() && border.size()!=2)) w[v]=1;  // loose helper or corner
        else if(border.size()==2) { w[v]=0.75f; w[border[0]]+=0.125f; w[border[1]]+=0.125f; }
        else {
            // (F + 2R + (n-3)P) / n with F the mean face point and R the mean edge midpoint.
            for(auto f:vertexFaces[v]) addFace(w,f,1.0f/(n*float(vertexFaces[v].size())));
            for(auto e:vertexEdges[v]) { w[edges[e].first]+=1.0f/(n*n); w[edges[e].second]+=1.0f/(n*n); }
            w[v]+=(n-3)/n;
        }
        emit(w);
    }
    for(uint32_t e=0;e<E;++e) {
        Weights w;
        if(edgeFaces[e].size()==2) {
            w[edges[e].first]+=0.25f; w[edges[e].second]+=0.25f;
            addFace(w,edgeFaces[e][0],0.25f); addFace(w,edgeFaces[e][1],0.25f);
        } else { w[edges[e].first]=0.5f; w[edges[e].second]=0.5f; }
        emit(w);
    }
    for(uint32_t f=0;f<faces.size();++f) { Weights w; addFace(w,f,1); emit(w); }
    // Skin weights of the new points: mean of the stencil's source vertices, strongest four kept.
    for(uint32_t v=V;v<stencilStart_.size()-1;++v) {
        std::map<uint32_t,float> influences;
        for(uint32_t s=stencilStart_[v];s<stencilStart_[v+1];++s) {
            auto [i,x]=stencil_[s]; x=std::abs(x);
            for(int j=0;j<4;++j) influences[(joints_[i]>>(j*8))&255]+=x*float((weights_[i]>>(j*8))&255);
        }
        std::vector<std::pair<float,uint32_t>> sorted;
        for(auto [joint,w]:influences) if(w>0) sorted.push_back({w,joint});
        std::sort(sorted.rbegin(),sorted.rend()); if(sorted.size()>4) sorted.resize(4);
        float sum=0; for(auto [w,j]:sorted) sum+=w;
        uint32_t js=0,ws=0; int remaining=255;
        for(size_t k=0;k<sorted.size();++k) {
            int w=k+1==sorted.size()?remaining:std::clamp(int(std::round(sorted[k].first/std::max(sum,1e-5f)*255)),0,remaining);
            remaining-=w; js|=sorted[k].second<<(k*8); ws|=uint32_t(w)<<(k*8);
        }
        joints_.push_back(js); weights_.push_back(ws);
    }
    // Render vertices: corners keep theirs, UV seams split edge points; UVs are interpolated linearly.
    std::vector<RenderVertex> render=render_;
    std::map<uint64_t,uint32_t> renderEdges;
    auto edgePoint=[&](uint32_t a,uint32_t b) {
        auto [it,added]=renderEdges.try_emplace(key(a,b),uint32_t(render.size()));
        if(added) render.push_back({V+edgeIds.at(key(render_[a].base,render_[b].base)),(render_[a].uv+render_[b].uv)*0.5f});
        return it->second;
    };
    std::vector<uint32_t> triangles;
    for(uint32_t f=0;f<faces.size();++f) {
        auto& c=faces[f]; size_t n=c.size();
        glm::vec2 uv(0); for(auto r:c) uv+=render_[r].uv/float(n);
        uint32_t center=uint32_t(render.size()); render.push_back({facePoints_+f,uv});
        for(size_t k=0;k<n;++k) {
            uint32_t next=edgePoint(c[k],c[(k+1)%n]), prev=edgePoint(c[(k+n-1)%n],c[k]);
            triangles.insert(triangles.end(),{c[k],next,center, c[k],center,prev});
        }
    }
    render_=std::move(render); triangles_=std::move(triangles);
    std::vector<std::set<uint32_t>> adjacent(stencilStart_.size()-1);
    for(size_t i=0;i<triangles_.size();i+=3) for(int k=0;k<3;++k) {
        uint32_t a=render_[triangles_[i+k]].base, b=render_[triangles_[i+(k+1)%3]].base;
        adjacent[a].insert(b); adjacent[b].insert(a);
    }
    neighborStart_.assign(1,0); neighbors_.clear();
    for(auto& s:adjacent) { neighbors_.insert(neighbors_.end(),s.begin(),s.end()); neighborStart_.push_back(uint32_t(neighbors_.size())); }
}
void Human::bindSkinAsset(size_t a) {
    if(assets_[a].bound) return;
    assets_[a].bound=true;
    // Skin assets, and assets fitted to the body's surface by a transfer ("smooth", bake_mblab_proxies.py).
    if(!meta_["assets"][a].value("skin",false) && !meta_["assets"][a].value("smooth",false)) return;
    // Rest pose of the smoothed body and the triangles around each of its vertices.
    auto& rest=smoothRest_;
    auto& around=around_;
    if(rest.empty()) {
        rest.assign(stencilStart_.size()-1,glm::vec3(0));
        for(size_t v=0;v<rest.size();++v) for(uint32_t s=stencilStart_[v];s<stencilStart_[v+1];++s) rest[v]+=base_[stencil_[s].first]*stencil_[s].second;
        around.resize(rest.size());
        for(uint32_t t=0;t<triangles_.size();t+=3) for(int k=0;k<3;++k) around[render_[triangles_[t+k]].base].push_back(t);
    }
    auto closest=[](glm::vec3 p,glm::vec3 a,glm::vec3 b,glm::vec3 c) {
        // Barycentric weights of the point of triangle abc closest to p (Ericson, Real-Time Collision Detection 5.1.5).
        glm::vec3 ab=b-a, ac=c-a, ap=p-a;
        float d1=glm::dot(ab,ap), d2=glm::dot(ac,ap);
        if(d1<=0 && d2<=0) return glm::vec3(1,0,0);
        glm::vec3 bp=p-b; float d3=glm::dot(ab,bp), d4=glm::dot(ac,bp);
        if(d3>=0 && d4<=d3) return glm::vec3(0,1,0);
        float vc=d1*d4-d3*d2;
        if(vc<=0 && d1>=0 && d3<=0) { float v=d1/(d1-d3); return glm::vec3(1-v,v,0); }
        glm::vec3 cp=p-c; float d5=glm::dot(ab,cp), d6=glm::dot(ac,cp);
        if(d6>=0 && d5<=d6) return glm::vec3(0,0,1);
        float vb=d5*d2-d1*d6;
        if(vb<=0 && d2>=0 && d6<=0) { float w=d2/(d2-d6); return glm::vec3(1-w,0,w); }
        float va=d3*d6-d5*d4;
        if(va<=0 && d4-d3>=0 && d5-d6>=0) { float w=(d4-d3)/((d4-d3)+(d5-d6)); return glm::vec3(0,1-w,w); }
        float s=1/(va+vb+vc); return glm::vec3(1-(vb+vc)*s,vb*s,vc*s);
    };
    {
        // The point of the coarse body each vertex hangs from moves to the nearest point of the smoothed body; the
        // offset from there stays, so the shape is kept and vertices on the skin stay on it.
        for(auto& v:assets_[a].vertices) {
            glm::vec3 p(0); for(int k=0;k<3;++k) p+=base_[v.refs[k]]*v.weights[k];
            std::set<uint32_t> near(std::begin(v.refs),std::end(v.refs));
            for(int ring=0;ring<2;++ring) {
                std::set<uint32_t> grown=near;
                for(auto n:near) grown.insert(neighbors_.begin()+neighborStart_[n],neighbors_.begin()+neighborStart_[n+1]);
                near=std::move(grown);
            }
            float best=std::numeric_limits<float>::max();
            for(auto n:near) for(auto t:around[n]) {
                uint32_t i[3]; for(int k=0;k<3;++k) i[k]=render_[triangles_[t+k]].base;
                glm::vec3 w=closest(p,rest[i[0]],rest[i[1]],rest[i[2]]);
                float d=glm::distance(p,rest[i[0]]*w.x+rest[i[1]]*w.y+rest[i[2]]*w.z);
                if(d<best) { best=d; for(int k=0;k<3;++k) { v.refs[k]=i[k]; v.weights[k]=w[k]; } }
            }
        }
    }
}
void Human::reset() {
    applyTemplate(Template::Male);
}
void Human::applyTemplate(Template body) {
    const bool female=body==Template::Female;
    activate(female);
    state={{"version",1},{"enabled",true},{"template",female?"female":"male"},
           {"gender",female?0.0f:1.0f},{"age",0.5f},{"muscle",female?0.42f:0.62f},{"weight",female?0.46f:0.49f},
           {"height",female?0.48f:0.56f},{"proportions",0.65f},{"cupsize",0.44f},{"firmness",0.6f},
           {"african",0.0f},{"asian",0.0f},{"caucasian",1.0f},{"details",nlohmann::json::object()},
           {"assets",nlohmann::json::array()},{"skin",0},{"eyes",0},{"skinTone",0.5f},{"skinWarmth",0.5f},{"hairVolume",0.45f},
           {"underwear",true},{"definition",0.6f}};
    // The bob's texture is a pale ash blond; a warm brown reads better as the starting look.
    if(female) { auto& c=hairShades()[2].rgb; state["hairColor"]={c[0],c[1],c[2]}; }
    std::map<std::string,bool> selected;
    // The template's hairstyle: the first of these the catalog has (MakeHuman's, then the fitted packs').
    std::string hair;
    for(auto name:female?std::initializer_list<const char*>{"hair_bob02","hair_cc_bun","hair_cc_ponytail"}
                        :std::initializer_list<const char*>{"hair_short01"})
        for(auto& a:meta_["assets"]) if(hair.empty() && a["name"]==name) hair=name;
    for(size_t i=0;i<meta_["skins"].size();++i)
        if(meta_["skins"][i]["name"]==(female?"young_caucasian_female":"young_caucasian_male")) state["skin"]=i;
    for(auto& a:meta_["assets"]) {
        std::string kind=a["kind"], name=a["name"];
        bool use=kind!="clothes" && kind!="hair" && !selected[kind];
        if(kind=="hair") use=name==hair;
        if(kind=="clothes") use=name==(female?"clothes_female_casualsuit01":"clothes_male_casualsuit01") || name=="clothes_shoes01";
        // Genitals: the first asset made for this sex (bake_genitals.py), none if there is no such asset.
        if(kind=="genitals") use=!selected[kind] && a.value("sex",std::string())==(female?"female":"male");
        if(use) { state["assets"].push_back(name); selected[kind]=true; }
    }
}
std::string Human::clothingSlot(const nlohmann::json& asset) {
    if(asset.value("kind",std::string())!="clothes") return {};
    if(asset.contains("slot") && asset["slot"].is_string()) return asset["slot"];
    // Bakes from before the slots: MakeHuman's system clothes by name.
    std::string name=asset.value("name",std::string());
    return name.rfind("clothes_shoes",0)==0?"shoes":name.rfind("clothes_fedora",0)==0?"hat":"outfit";
}
void Human::randomize(uint32_t seed) {
    std::mt19937 rng(seed);
    auto uniform=[&](float lo,float hi) { return std::uniform_real_distribution<float>(lo,hi)(rng); };
    auto pick=[&](size_t n) { return std::uniform_int_distribution<size_t>(0,n-1)(rng); };
    const bool female=rng()&1;
    applyTemplate(female?Template::Female:Template::Male);
    state["gender"]=female?uniform(0.0f,0.2f):uniform(0.8f,1.0f);
    state["age"]=uniform(0.5f,0.9f); state["muscle"]=uniform(0.3f,0.8f); state["weight"]=uniform(0.3f,0.7f);
    state["height"]=uniform(0.3f,0.75f); state["proportions"]=uniform(0.45f,0.85f);
    state["cupsize"]=uniform(0.3f,0.7f); state["firmness"]=uniform(0.4f,0.8f);
    // One main ancestry with a share of another one.
    const char* races[]={"african","asian","caucasian"};
    size_t main=pick(3), blend=pick(3); float share=uniform(0.0f,0.4f);
    for(size_t r=0;r<3;++r) state[races[r]]=(r==main?1.0f-share:0.0f)+(r==blend?share:0.0f);
    state["skinTone"]=uniform(0.4f,0.6f); state["skinWarmth"]=uniform(0.4f,0.6f); state["hairVolume"]=uniform(0.3f,0.65f);
    state["definition"]=uniform(0.3f,0.8f);
    // Subtle facial variation: large detail values quickly look deformed. Eye morphs are left out because they
    // easily push the eyeball through the lids.
    static const std::set<std::string> face={"head","forehead","eyebrows","nose","mouth","cheek","chin","ears"};
    for(auto& s:meta_["sliders"]) {
        if(!face.count(s["group"].get<std::string>()) || rng()%3) continue;
        bool oneSided=s.value("neg",0)<0;
        state["details"][s["group"].get<std::string>()+"/"+s["name"].get<std::string>()]=oneSided?uniform(0.0f,0.2f):uniform(-0.3f,0.3f);
    }
    float age=state["age"];
    std::string skin=std::string(age<0.65f?"young":age<0.8f?"middleage":"old")+"_"+races[main]+(female?"_female":"_male");
    for(size_t i=0;i<meta_["skins"].size();++i) if(meta_["skins"][i]["name"]==skin) state["skin"]=i;
    auto eye=[&](const char* name) {
        for(size_t i=0;i<meta_["eyeMaterials"].size();++i) if(meta_["eyeMaterials"][i]["name"]==name) state["eyes"]=i;
    };
    if(main!=2) eye(rng()%3?"brown":"brownlight");
    else if(!meta_["eyeMaterials"].empty()) state["eyes"]=pick(meta_["eyeMaterials"].size());
    // Replaces the template's choice for one slot with a random candidate that exists in the catalog.
    auto replace=[&](auto matches,std::vector<std::string> candidates) {
        auto& selection=state["assets"];
        for(auto it=selection.begin();it!=selection.end();) if(matches(it->template get<std::string>())) it=selection.erase(it); else ++it;
        std::erase_if(candidates,[&](const std::string& c) {
            for(auto& a:meta_["assets"]) if(a["name"]==c) return false;
            return true;
        });
        if(!candidates.empty()) selection.push_back(candidates[pick(candidates.size())]);
    };
    auto prefix=[](const char* p) { return [p](const std::string& name) { return name.rfind(p,0)==0; }; };
    // Natural looking styles only; the stylised ones (anime, coloured) stay a deliberate choice in the creator.
    if(female) replace(prefix("hair_"),{"hair_bob01","hair_bob02","hair_long01","hair_ponytail01","hair_braid01","hair_afro01",
        "hair_bob_blunt","hair_bob_blunt_bangs","hair_bob_curled_under","hair_bob_curled_under_bangs","hair_bob_inverted",
        "hair_bob_inverted_bangs","hair_littleright_hair_bobcut","hair_fhair01","hair_o4saken_long01",
        "hair_hair_bun_brown","hair_double_mhbraid","hair_frenchbraid1mh01","hair_rev_fr_braidbun1","hair_unkempt_mhbraidfr",
        "hair_cornrowsofelv5","hair_curly","hair_curly2","hair_elv_50supdo1","hair_elvs_adrienne_hair1",
        "hair_elvs_ashley_mayhair1","hair_elvs_braid_bun_q1","hair_elvs_daisyhair1","hair_elvs_hazel_hair",
        "hair_elvs_inverted_curlybob1","hair_elvs_island_princess1","hair_elvs_katherinehair1","hair_elvs_ladies_hippyhair1",
        "hair_elvs_laracroft_hair1a","hair_elvs_mickey_afro","hair_elvs_short_daisyh1","hair_elvs_that80sbabe_m1",
        "hair_elvscurly_bob1","hair_hair_07","hair_hair_08","hair_long2_alpha7","hair_long_alpha7","hair_o4saken_chinesebob01",
        "hair_o4saken_curly01","hair_witchybob_elv2","hair_cc_long","hair_cc_ponytail","hair_cc_bun"});
    else replace(prefix("hair_"),{"hair_short01","hair_short02","hair_short03","hair_short04","hair_afro01","hair_mhair02",
        "hair_hair_05","hair_hair1","hair_short_messy","hair_elvs_grumphair","hair_elvs_keylth_hair_m1",
        "hair_elvs_thejournalist1","hair_hair_03","hair_hair_04","hair_maxwell_hair_mh"});
    std::vector<std::string> brows, outfits, shoes;
    for(auto& a:meta_["assets"]) {
        std::string name=a["name"];
        if(a["kind"]=="eyebrows") brows.push_back(name);
        if(name.rfind(female?"clothes_female_":"clothes_male_",0)==0) outfits.push_back(name);
        if(name.rfind("clothes_shoes",0)==0) shoes.push_back(name);
    }
    replace(prefix("eyebrows_"),brows);
    replace([](const std::string& n) { return n.rfind("clothes_",0)==0 && n.rfind("clothes_shoes",0)!=0; },outfits);
    replace(prefix("clothes_shoes"),shoes);
    if(rng()%6==0) state["assets"].push_back(rng()&1?"clothes_fedora":"clothes_fedora_cocked");
    // Natural hair colours that fit ancestry and age; sometimes the texture's own colour.
    if(rng()%4) {
        size_t shade=age>0.84f?9+pick(2):main!=2?pick(3):pick(9);
        auto& c=hairShades()[shade].rgb; state["hairColor"]={c[0],c[1],c[2]};
    } else state.erase("hairColor");
}
bool Human::loadPreset(const std::filesystem::path& file) {
    try {
        std::ifstream f(file); if(!f) return false;
        auto j=nlohmann::json::parse(f);
        if(!j.is_object() || j.value("version",0)!=1) return false;
        if(!j.value("details",nlohmann::json::object()).is_object() || !j.value("assets",nlohmann::json::array()).is_array()) return false;
        for(auto& a:j.value("assets",nlohmann::json::array())) if(!a.is_string()) return false;
        for(auto key:{"skin","eyes"}) if(j.contains(key) && !j[key].is_number_integer()) return false;
        for(auto key:{"enabled","underwear"}) if(j.contains(key) && !j[key].is_boolean()) return false;
        if(j.contains("template") && (!j["template"].is_string() || (j["template"]!="male" && j["template"]!="female"))) return false;
        if(j.contains("hairColor") && !j["hairColor"].is_null() && !validColor(j["hairColor"])) return false;
        auto next=state;
        next.merge_patch(j);
        for(auto key:{"gender","age","muscle","weight","height","proportions","cupsize","firmness","african","asian","caucasian",
                      "skinTone","skinWarmth","hairVolume","definition"})
            next[key]=value(next,key,value(state,key,0.5f));
        // A preset without a hair colour keeps the hair texture's own colour.
        if(!j.contains("hairColor") || j["hairColor"].is_null()) next.erase("hairColor");
        else for(auto& c:next["hairColor"]) c=std::clamp(c.get<float>(),0.0f,1.0f);
        if(!j.contains("template")) next["template"]=next["gender"].get<float>()<0.5f?"female":"male";
        for(auto it=next["details"].begin();it!=next["details"].end();++it) {
            if(!it.value().is_number()) return false;
            float v=it.value().get<float>(); if(!std::isfinite(v)) return false;
            it.value()=std::clamp(v,-1.0f,1.0f);
        }
        state=std::move(next); activate(state["template"]=="female"); completeSelection(); return true;
    } catch(...) { return false; }
}
bool Human::savePreset(const std::filesystem::path& file) const {
    try {
        auto temp=file; temp += ".tmp";
        { std::ofstream f(temp); f << state.dump(2) << '\n'; f.close(); if(!f) return false; }
        // Windows rename cannot replace an existing file; copy only after a complete temporary write.
        std::filesystem::copy_file(temp,file,std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove(temp); return true;
    } catch(...) { return false; }
}
void Human::applyModifiers(std::vector<glm::vec3>& positions, const std::map<std::string,float>& factors) {
    if(!meta_.contains("modifiers")) return;
    // Properties: neutral 0.5; a detail slider (-1..1) spans 0..1.
    std::map<std::string,float> props;
    for(auto& p:meta_["properties"]) props[p.get<std::string>()]=0.5f;
    auto& details=state["details"];
    for(auto& s:meta_["sliders"]) {
        if(!s.contains("prop")) continue;
        std::string key=s["group"].get<std::string>()+"/"+s["name"].get<std::string>();
        float v=details.contains(key)&&details[key].is_number()?details[key].get<float>():0.0f;
        if(std::isfinite(v)) props[s["prop"].get<std::string>()]=0.5f+0.5f*std::clamp(v,-1.0f,1.0f);
    }
    // Body macros as MB-Lab's age, mass and tone (-1..1): each shifts the properties containing a table entry's name
    // (MB-Lab humanoid.calculate_transformation).
    const std::pair<const char*,float> meta[]={{"age_data",(value(state,"age",0.5f)-0.5f)*2},
        {"fat_data",value(state,"weight",0.5f)*2-1},{"muscle_data",value(state,"muscle",0.5f)*2-1}};
    for(auto [table,x]:meta) {
        float below=std::max(-x,0.0f), above=std::max(x,0.0f);
        for(auto& row:meta_["transformations"].value(table,nlohmann::json::array())) {
            std::string part=row.at(0);
            for(auto& [name,v]:props) if(name.find(part)!=std::string::npos) v+=row.at(1).get<float>()*below+row.at(2).get<float>()*above;
        }
    }
    auto shift=[&](const char* prop,float d) { if(auto it=props.find(prop); it!=props.end()) it->second+=d; };
    shift("Torso_BreastMass",value(state,"cupsize",0.5f)-0.5f);
    shift("Torso_BreastTone",value(state,"firmness",0.5f)-0.5f);
    shift("Body_Size",value(state,"height",0.5f)-0.5f);
    // Smart combo (MB-Lab algorithms.smart_combo): every combination of the modifier's property ends weighs the sum of
    // its ends' strengths; combinations within a third of the best one share the strongest property's strength.
    for(auto& m:meta_["modifiers"]) {
        auto& names=m["props"];
        size_t n=names.size(); if(n==0 || n>8) continue;
        std::vector<std::array<float,2>> ends(n);  // strength towards "max", towards "min"
        float factor=0;
        for(size_t k=0;k<n;++k) {
            auto it=props.find(names[k].get<std::string>()); float v=it!=props.end()?it->second:0.5f;
            ends[k]={v>0.5f?2*v-1:0.0f,v<0.5f?1-2*v:0.0f};
            factor=std::max({factor,ends[k][0],ends[k][1]});
        }
        if(factor<=0) continue;
        std::vector<float> w(size_t(1)<<n,0.0f);
        float best=0, sum=0;
        for(size_t c=0;c<w.size();++c) {
            for(size_t k=0;k<n;++k) w[c]+=ends[k][(c>>(n-1-k))&1];
            best=std::max(best,w[c]);
        }
        for(auto& x:w) { x=std::max(0.0f,x-best/1.5f); sum+=x; }
        if(sum<=0) continue;
        for(size_t c=0;c<w.size();++c) {
            if(w[c]<=0) continue;
            std::string tags;
            for(size_t k=0;k<n;++k) tags+=std::string(k?"-":"")+(((c>>(n-1-k))&1)?"min":"max");
            auto t=m["targets"].find(tags);
            if(t==m["targets"].end()) continue;
            float weight=factor*w[c]/sum;
            auto apply=[&](int index,float x) {
                auto& target=targets_[size_t(index)];
                for(size_t i=0;i<target.indices.size();++i) positions[target.indices[i]]+=target.delta[i]*x;
            };
            // Face morphs differ per ancestry: blended by the ancestry mix.
            if(t->is_object()) { for(auto& [race,index]:t->items()) if(auto f=factors.find(race); f!=factors.end()) apply(index,weight*f->second); }
            else apply(t->get<int>(),weight);
        }
    }
}
bool Human::rebuild(Character& character, bool hideClothes) {
    if(!ready()) return false;
    activate(state.value("template",std::string("male"))=="female");
    completeSelection();
    try {
        std::vector<bool> active(assets_.size());
        for(auto& selected:state["assets"])
            for(size_t a=0;a<assets_.size();++a) if(meta_["assets"][a]["name"]==selected) bindSkinAsset(a);
        bool outfit=false, underwearWorn=false, groinCovered=false;
        for(size_t a=0;a<assets_.size();++a) {
            std::string name=meta_["assets"][a]["name"];
            for(auto& selected:state["assets"]) if(selected==name) active[a]=true;
            if(hideClothes && meta_["assets"][a]["kind"]=="clothes") active[a]=false;
            std::string slot=active[a]?clothingSlot(meta_["assets"][a]):std::string();
            outfit|=slot=="outfit" || slot=="top" || slot=="bottom";
            underwearWorn|=slot=="underwear" || slot=="undertop";
            groinCovered|=slot=="outfit" || slot=="bottom" || slot=="underwear";
        }
        // The plain underwear covers the body while no clothes and no underwear asset are worn; genitals show only when
        // nothing covers the groin (they would poke through).
        const bool underwear=!outfit && !underwearWorn && state.value("underwear",true);
        for(size_t a=0;a<assets_.size();++a)
            if(meta_["assets"][a]["kind"]=="genitals" && (groinCovered || underwear)) active[a]=false;
        auto positions=base_;
        // Body morphs that come with a shown asset (genitals flatten the male body's moulded bulge).
        for(size_t a=0;a<assets_.size();++a) if(active[a])
            for(auto& [name,w]:meta_["assets"][a].value("bodyMorphs",nlohmann::json::object()).items()) {
                auto morph=assets_[a].morphs.find(name);
                if(morph==assets_[a].morphs.end() || !w.is_number()) continue;
                for(size_t i=0;i<morph->second.indices.size();++i) positions[morph->second.indices[i]]+=morph->second.delta[i]*w.get<float>();
            }
        auto apply=[&](int index,float weight) {
            if(index<0 || size_t(index)>=targets_.size() || weight==0) return;
            auto& t=targets_[size_t(index)];
            for(size_t i=0;i<t.indices.size();++i) positions[t.indices[i]] += t.delta[i]*weight;
        };
        std::map<std::string,float> factors;
        for(auto it=meta_["macro"].begin();it!=meta_["macro"].end();++it) {
            float x=value(state,it.key(),0.5f);
            if(it.key()=="age") x=std::max(x,0.1875f);
            for(auto& p:it.value()["parts"]) {
                if(x<p["lowest"].get<float>() || x>p["highest"].get<float>()) continue;
                // Published range endpoints include tolerances. Use the true interpolation anchors.
                float lo=0, hi=1;
                std::string low=p["low"], high=p["high"];
                if(it.value()["parts"].size()>1) {
                    if(low=="child") lo=0.1875f;
                    else if(low=="young" || (low.find("average")==0) || low.empty()) lo=0.5f;
                    if(high=="child") hi=0.1875f;
                    else if(high=="young" || high.find("average")==0 || high.empty()) hi=0.5f;
                }
                float t=std::clamp((x-lo)/(hi-lo),0.0f,1.0f);
                if(!low.empty()) factors[low]=1-t;
                if(!high.empty()) factors[high]=t;
                break;
            }
        }
        float raceSum=0;
        for(auto r:{"african","asian","caucasian"}) raceSum+=value(state,r,0);
        for(auto r:{"african","asian","caucasian"}) factors[r]=raceSum>0 ? value(state,r,0)/raceSum : (std::string(r)=="caucasian" ? 1.0f:0.0f);
        for(auto it=meta_["macroTargets"].begin();it!=meta_["macroTargets"].end();++it) {
            std::string name=it.key().substr(it.key().find_last_of('/')+1);
            float w=1; size_t start=0;
            while(start<name.size()) { auto end=name.find('-',start); auto token=name.substr(start,end-start);
                auto found=factors.find(token); w*=found!=factors.end()?found->second:0;
                if(end==std::string::npos) break; start=end+1;
            }
            apply(it.value().get<int>(),w);
        }
        for(auto& s:meta_["sliders"]) {
            if(s.contains("prop")) continue;
            std::string key=s["group"].get<std::string>()+"/"+s["name"].get<std::string>();
            auto& details=state["details"];
            float v=details.contains(key)&&details[key].is_number()?details[key].get<float>():0;
            if(!std::isfinite(v)) v=0;
            v=std::clamp(v,s["neg"].get<int>()<0?0.0f:-1.0f,1.0f);
            apply(s[v<0?"neg":"pos"].get<int>(),std::abs(v));
        }
        applyModifiers(positions,factors);
        // Sculpted anatomy (bake_realism.py), blended by sex.
        const float gender=value(state,"gender",0.5f), definition=value(state,"definition",0.6f);
        for(auto& r:meta_.value("realism",nlohmann::json::array())) {
            float w=definition*(r.value("sex",std::string())=="male"?gender:1.0f-gender);
            auto& t=realism_[size_t(r["target"].get<int>())];
            if(w>0) for(size_t i=0;i<t.indices.size();++i) positions[t.indices[i]]+=t.delta[i]*w;
        }
        // Smooth body: one Catmull-Clark step. Base vertices keep their index, so bones and fitted assets follow.
        std::vector<glm::vec3> smooth(stencilStart_.size()-1,glm::vec3(0));
        for(size_t v=0;v<smooth.size();++v)
            for(uint32_t s=stencilStart_[v];s<stencilStart_[v+1];++s) smooth[v]+=positions[stencil_[s].first]*stencil_[s].second;
        positions=std::move(smooth);
        float ground=1e10f;
        for(auto& r:render_) ground=std::min(ground,positions[r.base].z);
        for(auto& p:positions) p.z-=ground;
        std::vector<SkeletonBone> bones;
        // A joint is the mean of its vertices, plus an optional fixed offset (MB-Lab).
        auto joint=[&](const nlohmann::json& b,const char* key) {
            glm::vec3 p(0); for(auto& i:b[key]) p+=positions[i.get<size_t>()];
            p/=float(b[key].size());
            std::string offset=std::string(key)+"Offset";
            return b.contains(offset)?p+vec(b[offset]):p;
        };
        for(auto& b:meta_["bones"]) {
            auto q=b["rot"]; glm::quat rot(q[3].get<float>(),q[0].get<float>(),q[1].get<float>(),q[2].get<float>());
            glm::vec3 oldDir=vec(b["neutralTail"])-vec(b["neutralHead"]), newDir=joint(b,"tail")-joint(b,"head");
            if(glm::length(oldDir)>1e-5f && glm::length(newDir)>1e-5f) rot=glm::rotation(glm::normalize(oldDir),glm::normalize(newDir))*rot;
            bones.push_back({b["name"],b["parent"],glm::normalize(rot),joint(b,"head")});
        }
        std::vector<bool> hidden(positions.size(),false);
        for(size_t a=0;a<assets_.size();++a) if(active[a]) for(auto i:assets_[a].deleted) hidden[i]=true;
        // A face point stands for its face: hidden when any corner is, which hides all of the face's children.
        for(uint32_t v=facePoints_;v<positions.size();++v)
            for(uint32_t s=stencilStart_[v];s<stencilStart_[v+1];++s) if(hidden[stencil_[s].first]) hidden[v]=true;
        std::vector<SkinVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MeshPart> parts;
        auto texture=[&](const nlohmann::json& m,const char* key,bool srgb) {
            return character.loadTexture(m.contains(key)&&m[key].is_string()?directory_/m[key].get<std::string>():std::filesystem::path(),srgb);
        };
        nlohmann::json skin=nlohmann::json::object();
        if(!meta_["skins"].empty()) skin=meta_["skins"][std::clamp(state.value("skin",0),0,int(meta_["skins"].size())-1)];
        float warmth=value(state,"skinWarmth",0.5f)-0.5f;
        glm::vec3 skinTint=std::exp2((value(state,"skinTone",0.5f)-0.5f)*1.4f)*glm::vec3(1+warmth*0.24f,1,1-warmth*0.36f);
        // Some source skins (old african) are far redder than the others; cap the red cast at the level of the rest.
        if(skin.contains("albedo") && skin["albedo"].is_string()) {
            glm::vec3 mean=meanColor(skin["albedo"].get<std::string>());
            skinTint.g*=std::max(1.0f,mean.r/mean.g/3.2f); skinTint.b*=std::max(1.0f,mean.r/mean.b/6.0f);
        }
        bool recolor=state.contains("hairColor") && validColor(state["hairColor"]);
        glm::vec3 hairColor(1);
        if(recolor) for(int k=0;k<3;++k) hairColor[k]=toLinear(std::clamp(state["hairColor"][k].get<float>(),0.0f,1.0f));
        for(auto& r:render_) vertices.push_back({positions[r.base],glm::vec3(0),r.uv,joints_[r.base],weights_[r.base]});
        for(size_t i=0;i<triangles_.size();i+=3) {
            bool hide=false; for(int k=0;k<3;++k) hide|=hidden[render_[triangles_[i+k]].base];
            if(!hide) indices.insert(indices.end(),triangles_.begin()+i,triangles_.begin()+i+3);
        }
        // Without a normal map of its own the skin takes the sculpt's detail of the dominant sex.
        nlohmann::json relief=skin;
        if(!relief.contains("normal") || !relief["normal"].is_string()) for(auto& r:meta_.value("realism",nlohmann::json::array()))
            if((r.value("sex",std::string())=="male")==(gender>=0.5f)) relief["normal"]=r["normal"];
        MeshPart body{0,uint32_t(indices.size()),texture(skin,"albedo",true),texture(relief,"normal",false),false};
        body.shading=MeshPart::Shading::Skin; body.tint=skinTint;
        // The sculpt's detail fits the sculpt's shape: it fades in together with that shape ("definition").
        if(relief!=skin) body.normalScale=definition;
        // MB-Lab's skin relief includes the wrinkles of an old body: young adults keep only its fine grain.
        else if(meta_.contains("modifiers")) body.normalScale=glm::mix(0.3f,1.0f,glm::smoothstep(0.55f,1.0f,value(state,"age",0.5f)));
        parts.push_back(body);
        // Vertices of skin assets with the body vertices they follow, to take over the body's crease shading.
        struct OnSkin { uint32_t vertex; const ProxyVertex* proxy; float height; };  // height above the skin, metres
        std::vector<OnSkin> onSkin;
        // Strand rigs of the shown hair: their bones follow the skeleton's in the joint palette (Character::setHair).
        std::vector<HairBone> hairBones;
        // Clothing layers, inside out. Each garment was made alone, so where layers overlap an inner one may stand out
        // through an outer one (a jeans waistband through a sweater): an inner garment's height over the skin is kept
        // under that of the layers over it, per body vertex. Accessories take no part.
        auto layer=[](const std::string& slot) {
            static const std::map<std::string,int> order={{"legwear",0},{"underwear",1},{"undertop",1},{"shoes",2},
                                                          {"bottom",3},{"top",4},{"outfit",5}};
            auto it=order.find(slot); return it==order.end()?-1:it->second;
        };
        std::vector<std::vector<float>> ceiling(assets_.size());  // per asset vertex: the most height allowed, metres
        {
            std::vector<std::pair<int,size_t>> worn;
            for(size_t a=0;a<assets_.size();++a)
                if(active[a] && layer(clothingSlot(meta_["assets"][a]))>=0) worn.push_back({layer(clothingSlot(meta_["assets"][a])),a});
            std::sort(worn.rbegin(),worn.rend());
            std::vector<float> outer(positions.size(),std::numeric_limits<float>::max()), mine;
            for(size_t k=0;k<worn.size();++k) {
                size_t a=worn[k].second; auto& asset=assets_[a];
                if(k>0 && worn[k].first<worn[k-1].first) {
                    // The layers over this one, spread one ring, so a garment's sparse vertices cover the skin between.
                    std::vector<float> spread=outer;
                    for(size_t v=0;v<outer.size();++v)
                        for(uint32_t n=neighborStart_[v];n<neighborStart_[v+1];++n) spread[v]=std::min(spread[v],outer[neighbors_[n]]);
                    outer.swap(spread);
                }
                auto& c=ceiling[a]; c.assign(asset.vertices.size(),std::numeric_limits<float>::max());
                mine.assign(positions.size(),std::numeric_limits<float>::max());
                for(size_t vi=0;vi<asset.vertices.size();++vi) {
                    auto& v=asset.vertices[vi];
                    float h=glm::length(v.offset)*0.1f;
                    for(int r=0;r<3;++r) {
                        c[vi]=std::min(c[vi],outer[v.refs[r]]-0.002f);
                        if(v.weights[r]>0.2f) mine[v.refs[r]]=std::min(mine[v.refs[r]],h);
                    }
                }
                for(size_t v=0;v<outer.size();++v) outer[v]=std::min(outer[v],mine[v]);
            }
        }
        for(size_t a=0;a<assets_.size();++a) if(active[a]) {
            auto& asset=assets_[a]; auto material=meta_["assets"][a];
            if(material["kind"]=="eyes" && !meta_["eyeMaterials"].empty()) material["albedo"]=meta_["eyeMaterials"][std::clamp(state.value("eyes",0),0,int(meta_["eyeMaterials"].size())-1)]["albedo"];
            glm::vec3 scale(0.1f);
            if(asset.scaled) for(int k=0;k<3;++k) scale[k]=std::abs(positions[asset.scaleA[k]][k]-positions[asset.scaleB[k]][k])/asset.reference[k];
            // Hair volume: the offsets hold the hair's distance from the scalp, so scaling them flattens or fluffs it.
            if(material["kind"]=="hair") scale*=0.8f+0.45f*value(state,"hairVolume",0.45f);
            // Asset morphs (genitals): two-sided sliders in "details" under "<kind>/<slider>".
            std::vector<glm::vec3> morphed(asset.vertices.size(),glm::vec3(0));
            for(auto& s:material.value("sliders",nlohmann::json::array())) {
                auto& details=state["details"]; std::string key=material["kind"].get<std::string>()+"/"+s.value("name",std::string());
                float w=details.contains(key)&&details[key].is_number()?std::clamp(details[key].get<float>(),-1.0f,1.0f):0.0f;
                auto morph=asset.morphs.find(s.value(w<0?"neg":"pos",std::string()));
                if(!std::isfinite(w) || w==0 || morph==asset.morphs.end()) continue;
                for(size_t i=0;i<morph->second.indices.size();++i) morphed[morph->second.indices[i]]+=morph->second.delta[i]*std::abs(w);
            }
            // Assets bound to one bone ("bone") move with it rigidly instead of blending the body's weights.
            int rigid=-1;
            uint32_t hairBase=uint32_t(bones.size()+hairBones.size());
            const bool rigged=!asset.rig.empty() && hairBase+asset.rig.size()<=Character::kMaxBones;
            if(rigged) for(auto& b:asset.rig) {
                auto at=[&](const Asset::RigPoint& q) {
                    glm::vec3 p=q.offset*scale; for(int k=0;k<3;++k) p+=positions[q.refs[k]]*q.weights[k]; return p;
                };
                hairBones.push_back({b.parent<0?-1:int(hairBase-bones.size())+b.parent,at(b.head),at(b.tail)});
            }
            if(material.contains("bone")) for(size_t b=0;b<bones.size();++b) if(bones[b].name==material.value("bone",std::string())) rigid=int(b);
            uint32_t offset=uint32_t(vertices.size()), first=uint32_t(indices.size());
            for(size_t vi=0;vi<asset.vertices.size();++vi) {
                auto& v=asset.vertices[vi];
                glm::vec3 p=(v.offset+morphed[vi])*scale; std::map<uint32_t,float> influences;
                if(!ceiling[a].empty()) {
                    float h=glm::length(p), top=std::max(ceiling[a][vi],0.001f);
                    if(h>top) p*=top/h;
                }
                for(int k=0;k<3;++k) {
                    p+=positions[v.refs[k]]*v.weights[k];
                    for(int j=0;j<4;++j) influences[(joints_[v.refs[k]]>>(j*8))&255]+=v.weights[k]*float((weights_[v.refs[k]]>>(j*8))&255);
                }
                // Rigid assets bend with the body where they lie on the skin and follow their bone where they stand
                // out, so their edge stays on the skin in any pose.
                if(rigged) {
                    auto& w=asset.rigWeights[vi]; float strand=0;
                    for(int k=0;k<4;++k) strand+=float(w.weights[k])/255.0f;
                    for(auto& [joint,x]:influences) x*=1-std::min(strand,1.0f);
                    for(int k=0;k<4;++k) if(w.weights[k]) influences[hairBase+w.bones[k]]+=float(w.weights[k]);
                }
                if(rigid>=0) {
                    float t=glm::smoothstep(0.002f,0.01f,glm::length((v.offset+morphed[vi])*scale));
                    for(auto& [joint,w]:influences) w*=1-t;
                    influences[uint32_t(rigid)]+=255*t;
                }
                std::vector<std::pair<float,uint32_t>> sorted;
                for(auto [joint,w]:influences) sorted.push_back({w,joint});
                std::sort(sorted.rbegin(),sorted.rend()); if(sorted.size()>4) sorted.resize(4);
                float sum=0; for(auto [w,j]:sorted) sum+=w;
                uint32_t js=0,ws=0; int remaining=255;
                for(size_t k=0;k<sorted.size();++k) {
                    int w=k+1==sorted.size()?remaining:std::clamp(int(std::round(sorted[k].first/std::max(sum,1e-5f)*255)),0,remaining);
                    remaining-=w; js|=sorted[k].second<<(k*8); ws|=uint32_t(w)<<(k*8);
                }
                vertices.push_back({p,glm::vec3(0),v.uv,js,ws});
            }
            for(auto i:asset.indices) indices.push_back(offset+i);
            // Skin assets ("skin") sample the body's skin texture at the UVs of the body underneath, so they blend in.
            const bool skinned=material.value("skin",false);
            const auto& look=skinned?skin:material;
            MeshPart part{first,uint32_t(indices.size())-first,texture(look,"albedo",true),texture(skinned?relief:look,"normal",false),
                          material.value("alphaTest",false)};
            std::string kind=material["kind"];
            part.shading=kind=="hair"?MeshPart::Shading::Hair:kind=="eyes"?MeshPart::Shading::Eyes:MeshPart::Shading::Cloth;
            if(skinned) {
                part.shading=MeshPart::Shading::Skin; part.tint=skinTint; part.normalScale=body.normalScale;
                for(size_t vi=0;vi<asset.vertices.size();++vi)
                    onSkin.push_back({offset+uint32_t(vi),&asset.vertices[vi],glm::length((asset.vertices[vi].offset+morphed[vi])*scale)});
            }
            // The eye mesh includes MakeHuman's cornea shell, mapped to a texel with zero alpha; drawn opaque it
            // covers the iris with one flat colour. The eye shader adds the cornea's wet highlight instead.
            if(kind=="eyes") part.alphaTest=true;
            // Brows and lashes follow the hair colour, a little darker.
            if(recolor && (kind=="hair" || kind=="eyebrows" || kind=="eyelashes") && material["albedo"].is_string()) {
                float strength=kind=="hair"?1.0f:kind=="eyebrows"?0.75f:0.4f;
                part.recolor=true; part.tint=hairColor*strength/std::max(glm::dot(meanColor(material["albedo"].get<std::string>()),glm::vec3(0.2126f,0.7152f,0.0722f)),0.01f);
            }
            parts.push_back(part);
        }
        // Accumulate body normals by base vertex so UV seams remain smooth.
        // Hidden faces count too: skin assets take the normals of the body under them.
        std::vector<glm::vec3> normals(positions.size(),glm::vec3(0));
        for(size_t i=0;i<triangles_.size();i+=3) {
            uint32_t a=render_[triangles_[i]].base,b=render_[triangles_[i+1]].base,c=render_[triangles_[i+2]].base;
            auto n=glm::cross(positions[b]-positions[a],positions[c]-positions[a]);
            for(auto v:{a,b,c}) normals[v]+=n;
        }
        for(size_t i=0;i<indices.size();i+=3) {
            auto a=indices[i],b=indices[i+1],c=indices[i+2];
            if(a<render_.size()) continue;
            auto n=glm::cross(vertices[b].pos-vertices[a].pos,vertices[c].pos-vertices[a].pos);
            for(auto v:{a,b,c}) vertices[v].normal+=n;
        }
        for(auto& n:normals) n=glm::length(n)>1e-12f?glm::normalize(n):glm::vec3(0,0,1);
        // Creases (eyelids, nostrils, armpits, between fingers) catch less light: concave curvature of the smoothed
        // body, blurred over a few rings. Stored as the body normal's length (1 = open), see skinned.vert.
        std::vector<float> crease(positions.size(),0), blurred(positions.size());
        for(size_t v=0;v<positions.size();++v) {
            uint32_t n=neighborStart_[v+1]-neighborStart_[v]; if(!n) continue;
            glm::vec3 mean(0); float len=0;
            for(uint32_t k=neighborStart_[v];k<neighborStart_[v+1];++k) {
                mean+=positions[neighbors_[k]]; len+=glm::distance(positions[neighbors_[k]],positions[v]);
            }
            mean/=float(n); len/=float(n);
            if(len>1e-6f) crease[v]=glm::dot(normals[v],mean-positions[v])/(len*len);  // ~ concave curvature, 1/m
        }
        for(int pass=0;pass<3;++pass) {
            for(size_t v=0;v<positions.size();++v) {
                float sum=crease[v]; uint32_t n=neighborStart_[v+1]-neighborStart_[v];
                for(uint32_t k=neighborStart_[v];k<neighborStart_[v+1];++k) sum+=crease[neighbors_[k]];
                blurred[v]=sum/float(n+1);
            }
            std::swap(crease,blurred);
        }
        for(size_t i=0;i<vertices.size();++i) {
            auto& n=vertices[i].normal;
            if(i<render_.size()) n=normals[render_[i].base]*(1.0f-std::clamp((crease[render_[i].base]-4.0f)/50.0f,0.0f,0.6f));
            else n=glm::length(n)>1e-8f?glm::normalize(n):glm::vec3(0,0,1);
        }
        // Skin assets shade like the body under them; where they lie on it they take its normal, so no edge shows.
        for(auto [i,v,height]:onSkin) {
            float c=0; glm::vec3 n(0);
            for(int k=0;k<3;++k) { c+=crease[v->refs[k]]*v->weights[k]; n+=normals[v->refs[k]]*v->weights[k]; }
            float lifted=glm::smoothstep(0.001f,0.005f,height);
            n=glm::mix(n,vertices[i].normal,lifted);
            vertices[i].normal=(glm::length(n)>1e-8f?glm::normalize(n):glm::vec3(0,0,1))*(1.0f-std::clamp((c-4.0f)/50.0f,0.0f,0.6f));
        }
        // Plain underwear while no outfit is worn (unless switched off): a shell a few millimetres above the body, cut by a signed
        // distance (metres, carried in uv.x) to its edges in the rest pose. The distance falls off with the vertex's
        // weight on the trunk and thighs, so hands resting at the hips stay bare and the cut along the arms stays clean.
        if(underwear) {
            auto bone=[&](const char* name) { for(auto& b:bones) if(b.name==name) return b.pos; return glm::vec3(0); };
            auto group=[&](std::initializer_list<const char*> names) {
                std::vector<bool> in(256,false);
                for(auto name:names) for(size_t b=0;b<bones.size();++b) if(bones[b].name==name) in[b]=true;
                return in;
            };
            auto weight=[&](uint32_t v,const std::vector<bool>& in) {
                float w=0;
                for(int j=0;j<4;++j) if(in[(joints_[v]>>(j*8))&255]) w+=float((weights_[v]>>(j*8))&255)/255.0f;
                return w;
            };
            const auto pelvis=group({"Hips","Spine","LeftUpLeg","RightUpLeg"}), torso=group({"Spine","Spine1","Spine2"});
            glm::vec3 hips=bone("Hips"), leg=bone("LeftUpLeg"), chest=bone("Spine2");
            float legX=std::abs(leg.x-hips.x), crotch=leg.z-0.9f*legX, top=hips.z+0.05f;
            const bool bra=value(state,"gender",0.5f)<0.5f;
            std::vector<float> mask(positions.size(),-1);
            for(size_t v=0;v<positions.size();++v) {
                glm::vec3 p=positions[v]; float side=std::abs(p.x-hips.x);
                float trunk=weight(uint32_t(v),pelvis);
                mask[v]=std::min(top-p.z,p.z-(crotch+std::max(side-0.25f*legX,0.0f)*0.95f))-std::max(0.6f-trunk,0.0f)*0.2f;
                if(bra) {
                    float chestWeight=weight(uint32_t(v),torso);
                    mask[v]=std::max(mask[v],std::min(chest.z+0.04f-p.z,p.z-(chest.z-0.09f))-std::max(0.75f-chestWeight,0.0f)*0.3f);
                }
            }
            // Fabric is taut: inside the garment the shell follows a Laplacian-smoothed body, so it spans the cleavage
            // and does not trace every bump. Towards the edges it returns to the body, lifted only over hollows.
            // Skin fully covered by fabric is dropped, so it cannot poke through the smoothed shell.
            std::vector<glm::vec3> smoothed=positions, next(positions.size());
            for(int pass=0;pass<16;++pass) {
                for(size_t v=0;v<positions.size();++v) {
                    uint32_t n=neighborStart_[v+1]-neighborStart_[v];
                    if(!n || mask[v]<-0.03f) { next[v]=smoothed[v]; continue; }
                    glm::vec3 mean(0);
                    for(uint32_t k=neighborStart_[v];k<neighborStart_[v+1];++k) mean+=smoothed[neighbors_[k]];
                    next[v]=glm::mix(smoothed[v],mean/float(n),0.6f);
                }
                std::swap(smoothed,next);
            }
            uint32_t first=uint32_t(indices.size());
            std::map<uint32_t,uint32_t> shell;
            for(uint32_t i=0;i<body.indexCount;i+=3) {
                const uint32_t t[3]={indices[i],indices[i+1],indices[i+2]};
                float lo=std::min({mask[render_[t[0]].base],mask[render_[t[1]].base],mask[render_[t[2]].base]});
                float hi=std::max({mask[render_[t[0]].base],mask[render_[t[1]].base],mask[render_[t[2]].base]});
                if(hi<=0) continue;
                for(int k=0;k<3;++k) {
                    auto [it,added]=shell.try_emplace(t[k],uint32_t(vertices.size()));
                    if(added) {
                        uint32_t b=render_[t[k]].base;
                        float lift=std::max(glm::dot(smoothed[b]-positions[b],normals[b]),0.0f);
                        glm::vec3 hug=positions[b]+normals[b]*lift;
                        glm::vec3 p=glm::mix(hug,smoothed[b],glm::smoothstep(0.02f,0.05f,mask[b]))+normals[b]*0.003f;
                        vertices.push_back({p,normals[b],glm::vec2(mask[b],0),joints_[b],weights_[b]});
                    }
                    indices.push_back(it->second);
                }
                if(lo>0.006f) indices[i+1]=indices[i+2]=indices[i];  // covered skin: degenerate, nothing rasterised
            }
            MeshPart part{first,uint32_t(indices.size())-first,texture(nlohmann::json::object(),"albedo",true),texture(nlohmann::json::object(),"normal",false),false};
            part.shading=MeshPart::Shading::Underwear; part.tint=glm::vec3(0.045f,0.047f,0.052f); parts.push_back(part);
        }
        for(auto& v:vertices) {
            if(!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.pos.z) || glm::length(v.pos)>10.0f)
                throw std::runtime_error("Invalid fitted vertex position");
            for(int k=0;k<4;++k) if(((v.weights>>(k*8))&255) && ((v.joints>>(k*8))&255)>=bones.size()+hairBones.size())
                throw std::runtime_error("Invalid skin joint");
        }
        character.setSkeleton(bones);
        if(!hairBones.empty()) {
            // Capsules around head, neck, trunk and shoulders: from the bone to the next one, as thick as the skin it
            // carries (a low percentile, so the strands resting on the back stay where they are). No arms or thighs: an
            // arm or knee swinging forward through the strands in front would carry them along instead of slipping past.
            auto index=[&](const char* name) { for(size_t b=0;b<bones.size();++b) if(bones[b].name==name) return int(b); return -1; };
            std::vector<HairCollider> colliders;
            const std::pair<const char*,const char*> segments[]={{"Head",nullptr},{"Neck","Head"},{"Spine2","Neck"},{"Spine1","Spine2"},
                {"Spine","Spine1"},{"Hips","Spine"},{"LeftShoulder","LeftArm"},{"RightShoulder","RightArm"}};
            for(auto [from,to]:segments) {
                int b=index(from), c=to?index(to):-1;
                if(b<0 || (to && c<0)) continue;
                std::vector<glm::vec3> skin;
                for(size_t v=0;v<positions.size();++v) {
                    int best=0; for(int k=1;k<4;++k) if(((weights_[v]>>(k*8))&255)>((weights_[v]>>(best*8))&255)) best=k;
                    if(int((joints_[v]>>(best*8))&255)==b) skin.push_back(positions[v]);
                }
                if(skin.size()<8) continue;
                glm::vec3 a=bones[size_t(b)].pos, e;
                if(c>=0) e=bones[size_t(c)].pos;
                else { e=glm::vec3(0); float top=-1e9f; for(auto& p:skin) { e+=p; top=std::max(top,p.z); } e/=float(skin.size()); e.z=top; }
                auto radius=[&](float pct) {
                    std::vector<float> d;
                    for(auto& p:skin) {
                        glm::vec3 ae=e-a; float t=std::clamp(glm::dot(p-a,ae)/std::max(glm::dot(ae,ae),1e-8f),0.0f,1.0f);
                        d.push_back(glm::distance(p,a+ae*t));
                    }
                    std::nth_element(d.begin(),d.begin()+long(d.size()*pct),d.end()); return d[size_t(d.size()*pct)];
                };
                float r=radius(0.35f);
                if(c<0) e.z-=r;  // the head: its cap ends at the crown
                colliders.push_back({b,a,e,r});
            }
            character.setHair(index("Head"),hairBones,colliders);
        }
        character.setMesh(vertices,indices,parts); return true;
    } catch(const std::exception& e) { std::fprintf(stderr,"Human rebuild: %s\n",e.what()); return false; }
}
}
