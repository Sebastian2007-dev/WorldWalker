#include "render/Human.h"
#include <algorithm>
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
           {"assets",nlohmann::json::array()},{"skin",0},{"eyes",0},{"skinTone",0.5f},{"skinWarmth",0.5f},{"hairVolume",0.45f}};
    // The bob's texture is a pale ash blond; a warm brown reads better as the starting look.
    if(female) { auto& c=hairShades()[2].rgb; state["hairColor"]={c[0],c[1],c[2]}; }
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
    // Subtle facial variation: large detail values quickly look deformed. Eye morphs are left out because they
    // easily push the eyeball through the lids.
    static const std::set<std::string> face={"head","forehead","eyebrows","nose","mouth","cheek","chin","ears"};
    for(auto& s:meta_["sliders"]) {
        if(!face.count(s["group"].get<std::string>()) || rng()%3) continue;
        bool oneSided=s["neg"].get<int>()<0;
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
        "hair_o4saken_curly01","hair_witchybob_elv2"});
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
        if(j.contains("enabled") && !j["enabled"].is_boolean()) return false;
        if(j.contains("template") && (!j["template"].is_string() || (j["template"]!="male" && j["template"]!="female"))) return false;
        if(j.contains("hairColor") && !j["hairColor"].is_null() && !validColor(j["hairColor"])) return false;
        auto next=state;
        next.merge_patch(j);
        for(auto key:{"gender","age","muscle","weight","height","proportions","cupsize","firmness","african","asian","caucasian",
                      "skinTone","skinWarmth","hairVolume"})
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
        MeshPart body{0,uint32_t(indices.size()),texture(skin,"albedo",true),texture(skin,"normal",false),false};
        body.shading=MeshPart::Shading::Skin; body.tint=skinTint; parts.push_back(body);
        for(size_t a=0;a<assets_.size();++a) if(active[a]) {
            auto& asset=assets_[a]; auto material=meta_["assets"][a];
            if(material["kind"]=="eyes" && !meta_["eyeMaterials"].empty()) material["albedo"]=meta_["eyeMaterials"][std::clamp(state.value("eyes",0),0,int(meta_["eyeMaterials"].size())-1)]["albedo"];
            glm::vec3 scale(0.1f);
            if(asset.scaled) for(int k=0;k<3;++k) scale[k]=std::abs(positions[asset.scaleA[k]][k]-positions[asset.scaleB[k]][k])/asset.reference[k];
            // Hair volume: the offsets hold the hair's distance from the scalp, so scaling them flattens or fluffs it.
            if(material["kind"]=="hair") scale*=0.8f+0.45f*value(state,"hairVolume",0.45f);
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
            MeshPart part{first,uint32_t(indices.size())-first,texture(material,"albedo",true),texture(material,"normal",false),
                          material.value("alphaTest",false)};
            std::string kind=material["kind"];
            part.shading=kind=="hair"?MeshPart::Shading::Hair:kind=="eyes"?MeshPart::Shading::Eyes:MeshPart::Shading::Cloth;
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
