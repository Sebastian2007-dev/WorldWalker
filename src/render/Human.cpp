#include "render/Human.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>
#include <glm/gtx/quaternion.hpp>

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
}
bool Human::load(const std::filesystem::path& directory) {
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
        assets_.clear();
        for(auto& a:meta_["assets"]) {
            Asset asset; std::ifstream af(directory/a.at("file").get<std::string>(),std::ios::binary);
            array(af,asset.vertices,count(af)); array(af,asset.indices,size_t(count(af))*3);
            uint8_t scaled; read(af,&scaled,1); asset.scaled=scaled!=0;
            for(int k=0;k<3;++k) { read(af,&asset.scaleA[k],1); read(af,&asset.scaleB[k],1); read(af,&asset.reference[k],1); }
            array(af,asset.deleted,count(af));
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
        reset(); return true;
    } catch(const std::exception& e) { std::fprintf(stderr,"Human: %s\n",e.what()); base_.clear(); return false; }
}
void Human::reset() {
    applyTemplate(Template::Male);
}
void Human::applyTemplate(Template body) {
    const bool female=body==Template::Female;
    state={{"version",1},{"enabled",true},{"template",female?"female":"male"},
           {"gender",female?0.0f:1.0f},{"age",0.5f},{"muscle",female?0.42f:0.62f},{"weight",female?0.46f:0.49f},
           {"height",female?0.48f:0.56f},{"proportions",0.65f},{"cupsize",0.44f},{"firmness",0.6f},
           {"african",0.0f},{"asian",0.0f},{"caucasian",1.0f},{"details",nlohmann::json::object()},
           {"assets",nlohmann::json::array()},{"skin",0},{"eyes",0}};
    std::map<std::string,bool> selected;
    for(size_t i=0;i<meta_["skins"].size();++i)
        if(meta_["skins"][i]["name"]==(female?"young_caucasian_female":"young_caucasian_male")) state["skin"]=i;
    for(auto& a:meta_["assets"]) {
        std::string kind=a["kind"], name=a["name"];
        bool use=kind!="clothes" && kind!="hair" && !selected[kind];
        if(kind=="hair") use=name==(female?"hair_bob02":"hair_short01");
        if(kind=="clothes") use=name==(female?"clothes_female_casualsuit01":"clothes_male_casualsuit01") || name=="clothes_shoes01";
        if(use) { state["assets"].push_back(name); selected[kind]=true; }
    }
}
bool Human::loadPreset(const std::filesystem::path& file) {
    try {
        std::ifstream f(file); if(!f) return false;
        auto j=nlohmann::json::parse(f);
        if(!j.is_object() || j.value("version",0)!=1) return false;
        if(!j.value("details",nlohmann::json::object()).is_object() || !j.value("assets",nlohmann::json::array()).is_array()) return false;
        for(auto& a:j.value("assets",nlohmann::json::array())) if(!a.is_string()) return false;
        for(auto key:{"skin","eyes"}) if(j.contains(key) && !j[key].is_number_integer()) return false;
        if(j.contains("enabled") && !j["enabled"].is_boolean()) return false;
        if(j.contains("template") && (!j["template"].is_string() || (j["template"]!="male" && j["template"]!="female"))) return false;
        auto next=state;
        next.merge_patch(j);
        for(auto key:{"gender","age","muscle","weight","height","proportions","cupsize","firmness","african","asian","caucasian"})
            next[key]=value(next,key,value(state,key,0.5f));
        if(!j.contains("template")) next["template"]=next["gender"].get<float>()<0.5f?"female":"male";
        for(auto it=next["details"].begin();it!=next["details"].end();++it) {
            if(!it.value().is_number()) return false;
            float v=it.value().get<float>(); if(!std::isfinite(v)) return false;
            it.value()=std::clamp(v,-1.0f,1.0f);
        }
        state=std::move(next); return true;
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
bool Human::rebuild(Character& character, bool hideClothes) {
    if(!ready()) return false;
    try {
        auto positions=base_;
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
            std::string key=s["group"].get<std::string>()+"/"+s["name"].get<std::string>();
            auto& details=state["details"];
            float v=details.contains(key)&&details[key].is_number()?details[key].get<float>():0;
            if(!std::isfinite(v)) v=0;
            v=std::clamp(v,s["neg"].get<int>()<0?0.0f:-1.0f,1.0f);
            apply(s[v<0?"neg":"pos"].get<int>(),std::abs(v));
        }
        float ground=1e10f;
        for(auto& r:render_) ground=std::min(ground,positions[r.base].z);
        for(auto& p:positions) p.z-=ground;
        std::vector<SkeletonBone> bones;
        auto mean=[&](const nlohmann::json& ids) { glm::vec3 p(0); for(auto& i:ids) p+=positions[i.get<size_t>()]; return p/float(ids.size()); };
        for(auto& b:meta_["bones"]) {
            auto q=b["rot"]; glm::quat rot(q[3].get<float>(),q[0].get<float>(),q[1].get<float>(),q[2].get<float>());
            glm::vec3 oldDir=vec(b["neutralTail"])-vec(b["neutralHead"]), newDir=mean(b["tail"])-mean(b["head"]);
            if(glm::length(oldDir)>1e-5f && glm::length(newDir)>1e-5f) rot=glm::rotation(glm::normalize(oldDir),glm::normalize(newDir))*rot;
            bones.push_back({b["name"],b["parent"],glm::normalize(rot),mean(b["head"])});
        }
        std::vector<bool> active(assets_.size()), hidden(base_.size(),false);
        for(size_t a=0;a<assets_.size();++a) {
            auto name=meta_["assets"][a]["name"];
            for(auto& selected:state["assets"]) if(selected==name) active[a]=true;
            if(hideClothes && meta_["assets"][a]["kind"]=="clothes") active[a]=false;
            if(active[a]) for(auto i:assets_[a].deleted) hidden[i]=true;
        }
        std::vector<SkinVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MeshPart> parts;
        auto texture=[&](const nlohmann::json& m,const char* key,bool srgb) {
            return character.loadTexture(m.contains(key)&&m[key].is_string()?directory_/m[key].get<std::string>():std::filesystem::path(),srgb);
        };
        nlohmann::json skin=nlohmann::json::object();
        if(!meta_["skins"].empty()) skin=meta_["skins"][std::clamp(state.value("skin",0),0,int(meta_["skins"].size())-1)];
        for(auto& r:render_) vertices.push_back({positions[r.base],glm::vec3(0),r.uv,joints_[r.base],weights_[r.base]});
        for(size_t i=0;i<triangles_.size();i+=3) {
            bool hide=false; for(int k=0;k<3;++k) hide|=hidden[render_[triangles_[i+k]].base];
            if(!hide) indices.insert(indices.end(),triangles_.begin()+i,triangles_.begin()+i+3);
        }
        parts.push_back({0,uint32_t(indices.size()),texture(skin,"albedo",true),texture(skin,"normal",false),false});
        for(size_t a=0;a<assets_.size();++a) if(active[a]) {
            auto& asset=assets_[a]; auto material=meta_["assets"][a];
            if(material["kind"]=="eyes" && !meta_["eyeMaterials"].empty()) material["albedo"]=meta_["eyeMaterials"][std::clamp(state.value("eyes",0),0,int(meta_["eyeMaterials"].size())-1)]["albedo"];
            glm::vec3 scale(0.1f);
            if(asset.scaled) for(int k=0;k<3;++k) scale[k]=std::abs(positions[asset.scaleA[k]][k]-positions[asset.scaleB[k]][k])/asset.reference[k];
            uint32_t offset=uint32_t(vertices.size()), first=uint32_t(indices.size());
            for(auto& v:asset.vertices) {
                glm::vec3 p=v.offset*scale; std::map<uint32_t,float> influences;
                for(int k=0;k<3;++k) {
                    p+=positions[v.refs[k]]*v.weights[k];
                    for(int j=0;j<4;++j) influences[(joints_[v.refs[k]]>>(j*8))&255]+=v.weights[k]*float((weights_[v.refs[k]]>>(j*8))&255);
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
            parts.push_back({first,uint32_t(indices.size())-first,texture(material,"albedo",true),texture(material,"normal",false),material.value("alphaTest",false)});
        }
        // Accumulate body normals by base vertex so UV seams remain smooth.
        std::vector<glm::vec3> normals(base_.size(),glm::vec3(0));
        for(size_t i=0;i<indices.size();i+=3) {
            auto a=indices[i],b=indices[i+1],c=indices[i+2];
            auto n=glm::cross(vertices[b].pos-vertices[a].pos,vertices[c].pos-vertices[a].pos);
            for(auto v:{a,b,c}) { vertices[v].normal+=n; if(v<render_.size()) normals[render_[v].base]+=n; }
        }
        for(size_t i=0;i<vertices.size();++i) {
            auto& n=vertices[i].normal; if(i<render_.size()) n=normals[render_[i].base];
            n=glm::length(n)>1e-8f?glm::normalize(n):glm::vec3(0,0,1);
        }
        for(auto& v:vertices) {
            if(!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.pos.z) || glm::length(v.pos)>10.0f)
                throw std::runtime_error("Invalid fitted vertex position");
            for(int k=0;k<4;++k) if(((v.weights>>(k*8))&255) && ((v.joints>>(k*8))&255)>=bones.size())
                throw std::runtime_error("Invalid skin joint");
        }
        character.setSkeleton(bones); character.setMesh(vertices,indices,parts); return true;
    } catch(const std::exception& e) { std::fprintf(stderr,"Human rebuild: %s\n",e.what()); return false; }
}
}
