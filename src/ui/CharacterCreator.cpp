#include "ui/CharacterCreator.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include "ui/Lang.h"
#include "ui/Theme.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <random>
#include <set>

namespace ui {
namespace {
std::string label(std::string s) {
    for(auto& c:s) if(c=='_' || c=='-') c=' ';
    if(!s.empty()) s[0]=char(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}
std::string lower(std::string s) {
    for(auto& c:s) c=char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
const char* groupLabel(const std::string& group) {
    static const std::map<std::string,const char*> en={
        {"arms","Arms"},{"legs","Legs"},{"torso","Upper body"},{"head","Head shape"},
        {"chin","Chin & jaw"},{"eyes","Eyes"},{"nose","Nose"},{"mouth","Mouth"},
        {"ears","Ears"},{"neck","Neck"},{"cheek","Cheeks"},{"forehead","Forehead"},
        {"eyebrows","Eyebrows"},{"breast","Breasts"},{"hip","Hips"},{"hips","Hips"},{"buttocks","Buttocks"},
        {"hands","Hands"},{"feet","Feet"},{"stomach","Stomach"},{"pelvis","Pelvis"}};
    if(english()) { auto it=en.find(group); return it==en.end()?nullptr:it->second; }
    static const std::map<std::string,const char*> names={
        {"arms","Arme"},{"legs","Beine"},{"torso","Oberkörper"},{"head","Kopfform"},
        {"chin","Kinn & Kiefer"},{"eyes","Augen"},{"nose","Nase"},{"mouth","Mund"},
        {"ears","Ohren"},{"neck","Hals"},{"cheek","Wangen"},{"forehead","Stirn"},
        {"eyebrows","Augenbrauen"},{"breast","Brust"},{"hip","Hüfte"},{"hips","Hüfte"},{"buttocks","Gesäß"},
        {"hands","Hände"},{"feet","Füße"},{"stomach","Bauch"},{"pelvis","Becken"}};
    auto it=names.find(group); return it==names.end()?nullptr:it->second;
}

// MakeHuman modifier names ("nose-trans-down-up") as German labels with the words for both slider ends.
struct DetailText { std::string label; std::string lo, hi; };
DetailText detailTextEnglish(const std::string& name);
DetailText detailText(const std::string& name) {
    if(english()) return detailTextEnglish(name);
    static const std::map<std::string,std::pair<const char*,const char*>> ranges={
        {"decr-incr",{"weniger","mehr"}},{"down-up",{"runter","hoch"}},{"in-out",{"innen","außen"}},
        {"backward-forward",{"zurück","vor"}},{"concave-convex",{"konkav","konvex"}},
        {"compress-uncompress",{"gestaucht","gestreckt"}},{"square-round",{"eckig","rund"}},
        {"pointed-triangle",{"spitz","dreieckig"}}};
    static const std::map<std::string,const char*> words={
        {"lowerarm","Unterarm"},{"upperarm","Oberarm"},{"upperleg","Oberschenkel"},{"upperlegs","Oberschenkel"},
        {"lowerleg","Unterschenkel"},{"lowerlegs","Unterschenkel"},{"leg","Beine"},{"calf","Wade"},{"knee","Knie"},
        {"thigh","Oberschenkel"},{"ankle","Knöchel"},{"wrist","Handgelenk"},{"shoulder","Schulter"},
        {"scale","Größe"},{"depth","Tiefe"},{"horiz","Breite"},{"vert","Höhe"},{"fat","Fett"},{"muscle","Muskeln"},
        {"length","Länge"},{"circ","Umfang"},{"height","Höhe"},{"height1","Höhe 1"},{"height2","Höhe 2"},
        {"height3","Höhe 3"},{"angle","Winkel"},{"rot","Drehung"},{"point","Spitze"},{"dist","Abstand"},
        {"volume","Volumen"},{"lobe","Ohrläppchen"},{"shape","Form"},{"flap","Ohrmuschel"},{"wing","Abstehen"},
        {"corner1","Augenwinkel 1"},{"corner2","Augenwinkel 2"},{"push1","Lid 1"},{"push2","Lid 2"},
        {"eyefold","Lidfalte"},{"bag","Tränensack"},{"epicanthus","Innere Lidfalte"},{"jaw","Kiefer"},
        {"drop","Absenkung"},{"prominent","Vorstehen"},{"bones","Knochen"},{"width","Breite"},
        {"width1","Breite oben"},{"width2","Breite Mitte"},{"width3","Breite unten"},{"cleft","Grübchen"},
        {"triangle","Dreiecksform"},{"prognathism","Vorbiss"},{"temple","Schläfen"},{"nubian","Wölbung"},
        {"fingers","Finger"},{"diameter","Dicke"},{"distance","Abstand"},{"back","Rückseite"},
        {"invertedtriangular","Umgekehrtes Dreieck"},{"diamond","Raute"},{"oval","Oval"},
        {"rectangular","Rechteckig"},{"round","Rund"},{"age","Alterung"},{"triangular","Dreieckig"},
        {"square","Eckig"},{"waist","Taille"},{"valgus","X-Bein-Stellung"},{"lowerlip","Unterlippe"},
        {"upperlip","Oberlippe"},{"cupidsbow","Amorbogen"},{"angles","Mundwinkel"},{"ext","Kontur"},
        {"middle","Mitte"},{"dimples","Grübchen"},{"philtrum","Philtrum"},{"double","Doppelkinn"},
        {"nostrils","Nasenlöcher"},{"flaring","Nasenflügel"},{"curve","Krümmung"},{"hump","Höcker"},
        {"greek","Griechische Form"},{"septumangle","Septumwinkel"},{"base","Basis"},{"compression","Stauchung"},
        {"tone","Straffheit"},{"pregnant","Schwangerschaft"},{"navel","Bauchnabel"},{"pectoral","Brustmuskel"},
        {"dorsi","Rückenmuskel"},{"underbust","Unterbrust"},{"napetowaist","Nacken bis Taille"},
        {"waisttohip","Taille bis Hüfte"},{"hips","Hüfte"},{"vshape","V-Form"},{"frontchest","Brustbreite"},
        {"bust","Brust"},{"inner","Innen"},{"bones","Knochen"},{"neck","Hals"}};
    static const std::set<std::string> stems={"nose","ear","eye","eyes","mouth","head","neck","hip","foot","hand","cheek",
        "chin","forehead","torso","stomach","pelvis","buttocks","breast","eyebrows"};
    static const std::set<std::string> sizes={"scale","depth","horiz","vert","length","circ","height","width","dist",
        "volume","distance","diameter","width1","width2","width3"};
    std::vector<std::string> tokens;
    for(size_t start=0;start<=name.size();) {
        size_t end=name.find('-',start); if(end==std::string::npos) end=name.size();
        tokens.push_back(name.substr(start,end-start)); start=end+1;
    }
    DetailText out;
    std::string range;
    if(tokens.size()>=3) {
        range=tokens[tokens.size()-2]+"-"+tokens.back();
        if(auto r=ranges.find(range); r!=ranges.end()) { out.lo=r->second.first; out.hi=r->second.second; tokens.resize(tokens.size()-2); }
        else range.clear();
    }
    if(tokens.size()>1 && stems.count(tokens[0])) tokens.erase(tokens.begin());
    std::erase(tokens,std::string("measure"));
    bool size=false;
    for(auto& t:tokens) size|=sizes.count(t)>0;
    if(range=="decr-incr" && size) { out.lo="kleiner"; out.hi="größer"; }
    for(size_t i=0;i<tokens.size();++i) {
        std::string word;
        if(tokens[i]=="trans") word=range=="down-up"?"Position vertikal":range=="in-out"?"Position seitlich":"Position vor/zurück";
        else if(tokens[i]=="laugh" && i+1<tokens.size() && tokens[i+1]=="lines") { word="Lachfalten"; ++i; }
        else if(tokens[i]=="scale" && i+1<tokens.size() && sizes.count(tokens[i+1])) continue;
        else if(auto w=words.find(tokens[i]); w!=words.end()) word=w->second;
        else word=label(tokens[i]);
        out.label+=(out.label.empty()?"":" ")+word;
    }
    if(out.label.empty()) out.label=label(name);
    return out;
}

// The same in English: the name's words, abbreviations spelled out.
DetailText detailTextEnglish(const std::string& name) {
    static const std::map<std::string,std::pair<const char*,const char*>> ranges={
        {"decr-incr",{"less","more"}},{"down-up",{"down","up"}},{"in-out",{"in","out"}},
        {"backward-forward",{"back","forward"}},{"concave-convex",{"concave","convex"}},
        {"compress-uncompress",{"compressed","stretched"}},{"square-round",{"square","round"}},
        {"pointed-triangle",{"pointed","triangular"}}};
    static const std::map<std::string,const char*> words={
        {"lowerarm","lower arm"},{"upperarm","upper arm"},{"upperleg","thigh"},{"upperlegs","thighs"},
        {"lowerleg","lower leg"},{"lowerlegs","lower legs"},{"leg","legs"},{"horiz","width"},{"vert","height"},
        {"circ","circumference"},{"rot","rotation"},{"dist","distance"},{"corner1","eye corner 1"},
        {"corner2","eye corner 2"},{"push1","lid 1"},{"push2","lid 2"},{"eyefold","eyelid fold"},{"bag","eye bag"},
        {"width1","width top"},{"width2","width middle"},{"width3","width bottom"},{"prognathism","underbite"},
        {"invertedtriangular","inverted triangle"},{"valgus","knock knees"},{"lowerlip","lower lip"},
        {"upperlip","upper lip"},{"cupidsbow","Cupid's bow"},{"angles","mouth corners"},{"ext","contour"},
        {"double","double chin"},{"flaring","nostril flare"},{"septumangle","septum angle"},{"tone","firmness"},
        {"pregnant","pregnancy"},{"pectoral","pectorals"},{"dorsi","back muscles"},{"napetowaist","nape to waist"},
        {"waisttohip","waist to hip"},{"vshape","V shape"},{"frontchest","chest width"},{"age","ageing"}};
    static const std::set<std::string> stems={"nose","ear","eye","eyes","mouth","head","neck","hip","foot","hand","cheek",
        "chin","forehead","torso","stomach","pelvis","buttocks","breast","eyebrows"};
    static const std::set<std::string> sizes={"scale","depth","horiz","vert","length","circ","height","width","dist",
        "volume","distance","diameter","width1","width2","width3"};
    std::vector<std::string> tokens;
    for(size_t start=0;start<=name.size();) {
        size_t end=name.find('-',start); if(end==std::string::npos) end=name.size();
        tokens.push_back(name.substr(start,end-start)); start=end+1;
    }
    DetailText out;
    std::string range;
    if(tokens.size()>=3) {
        range=tokens[tokens.size()-2]+"-"+tokens.back();
        if(auto r=ranges.find(range); r!=ranges.end()) { out.lo=r->second.first; out.hi=r->second.second; tokens.resize(tokens.size()-2); }
        else range.clear();
    }
    if(tokens.size()>1 && stems.count(tokens[0])) tokens.erase(tokens.begin());
    std::erase(tokens,std::string("measure"));
    bool size=false;
    for(auto& t:tokens) size|=sizes.count(t)>0;
    if(range=="decr-incr" && size) { out.lo="smaller"; out.hi="larger"; }
    for(size_t i=0;i<tokens.size();++i) {
        std::string word;
        if(tokens[i]=="trans") word=range=="down-up"?"vertical position":range=="in-out"?"sideways position":"front/back position";
        else if(tokens[i]=="scale" && i+1<tokens.size() && sizes.count(tokens[i+1])) continue;
        else if(auto w=words.find(tokens[i]); w!=words.end()) word=w->second;
        else word=tokens[i];
        out.label+=(out.label.empty()?"":" ")+word;
    }
    if(out.label.empty()) out.label=name;
    out.label[0]=char(std::toupper(static_cast<unsigned char>(out.label[0])));
    return out;
}

std::string skinLabel(const std::string& name) {
    static const std::map<std::string,const char*> en={
        {"young","Young"},{"middleage","Middle-aged"},{"old","Old"},{"african","African"},{"asian","Asian"},
        {"caucasian","European"},{"female","female"},{"male","male"},{"female2","female (variant 2)"},
        {"male2","male (variant 2)"},{"special","with suit"},{"latino","Latin American"}};
    static const std::map<std::string,const char*> de={
        {"young","Jung"},{"middleage","Mittleres Alter"},{"old","Alt"},{"african","afrikanisch"},{"asian","asiatisch"},
        {"caucasian","europäisch"},{"female","weiblich"},{"male","männlich"},{"female2","weiblich (Variante 2)"},
        {"male2","männlich (Variante 2)"},{"special","mit Anzug"},{"latino","lateinamerikanisch"}};
    const auto& words=english()?en:de;
    std::string out; size_t start=0;
    while(start<=name.size()) {
        size_t end=name.find('_',start); if(end==std::string::npos) end=name.size();
        std::string token=name.substr(start,end-start); start=end+1;
        auto w=words.find(token);
        if(token=="suit") continue;
        out+=(out.empty()?"":", ")+(w!=words.end()?std::string(w->second):token);
    }
    return out;
}
std::string assetLabel(const std::string& name) {
    static const std::map<std::string,const char*> names={
        {"hair_afro01","Afro"},{"hair_bob01","Bob, lang"},{"hair_bob02","Bob, kurz"},{"hair_braid01","Geflochtener Zopf"},
        {"hair_long01","Lang & offen"},{"hair_ponytail01","Pferdeschwanz"},{"hair_short01","Kurz, gescheitelt"},
        {"hair_short02","Kurz, frech"},{"hair_short03","Kurz, zurückgekämmt"},{"hair_short04","Kurzhaarschnitt"},
        {"hair_bob_blunt","Bob, gerade"},{"hair_bob_blunt_bangs","Bob, gerade mit Pony"},
        {"hair_bob_curled_under","Bob, eingedreht"},{"hair_bob_curled_under_bangs","Bob, eingedreht mit Pony"},
        {"hair_bob_inverted","Bob, gestuft"},{"hair_bob_inverted_bangs","Bob, gestuft mit Pony"},
        {"hair_littleright_hair_bobcut","Bob, kinnlang"},{"hair_fhair01","Lang, glatt"},
        {"hair_o4saken_long01","Lang, wellig"},{"hair_hair_06","Kurz, fransig"},{"hair_hair_bun_brown","Dutt"},
        {"hair_double_mhbraid","Zwei Zöpfe"},{"hair_frenchbraid1mh01","Französischer Zopf"},
        {"hair_rev_fr_braidbun1","Zopf mit Dutt"},{"hair_unkempt_mhbraidfr","Zopf, locker"},
        {"hair_blondwithheadband","Mit Haarband"},{"hair_mhair02","Kurz, seitlich"},{"hair_hair_05","Kurz, voll"},
        {"hair_hair1","Mittellang, gescheitelt"},{"hair_short_messy","Kurz, zerzaust"},{"hair_junglebookhair","Lang, zottelig"},
        {"hair_shaggy_green","Stufig, schulterlang"},{"hair_straight_bangs","Glatt mit Pony"},
        {"hair_strawberry_cloud","Lockenwolke"},{"hair_animehair","Anime"},
        {"hair_cornrowsofelv5","Cornrows"},{"hair_curly","Locken, lang"},{"hair_curly2","Locken, sehr lang"},
        {"hair_elv_50supdo1","50er-Hochsteckfrisur"},{"hair_elvs_adrienne_hair1","Lang, Seitenscheitel"},
        {"hair_elvs_ashley_mayhair1","Schulterlang, wellig"},{"hair_elvs_braid_bun_q1","Flechtdutt"},
        {"hair_elvs_daisyhair1","Lang, Mittelscheitel"},{"hair_elvs_grumphair","Zurückgekämmt"},
        {"hair_elvs_hazel_hair","Lang, gesträhnt"},{"hair_elvs_inverted_curlybob1","Bob, fransig"},
        {"hair_elvs_island_princess1","Lang mit Schleife"},{"hair_elvs_katherinehair1","Asymmetrisch mit Pony"},
        {"hair_elvs_keylth_hair_m1","Zurückgebunden"},{"hair_elvs_ladies_hippyhair1","Hippie, seitlich"},
        {"hair_elvs_laracroft_hair1a","Hochgesteckt mit Strähnen"},{"hair_elvs_mickey_afro","Afro-Puffs"},
        {"hair_elvs_short_daisyh1","Bob, Mittelscheitel"},{"hair_elvs_that80sbabe_m1","80er-Wellen"},
        {"hair_elvs_thejournalist1","Kurz, Seitenscheitel"},{"hair_elvscurly_bob1","Locken-Bob"},
        {"hair_hair_01","Meister mit Bart"},{"hair_hair_02","Anime, wellig"},{"hair_hair_03","Kurz, stufig"},
        {"hair_hair_04","Kurz, gewellt"},{"hair_hair_07","Lang, voluminös"},{"hair_hair_08","Pagenkopf"},
        {"hair_long2_alpha7","Lang, über dem Auge"},{"hair_long_alpha7","Lang, schlicht"},
        {"hair_maxwell_hair_mh","Stachelig"},{"hair_o4saken_chinesebob01","Bob, lang und stufig"},
        {"hair_o4saken_curly01","Locken, seitlich"},{"hair_wig_bow_tie","Mit Zopfband"},
        {"hair_wig_bun_blonde","Kurz, hochgesteckt"},{"hair_witchybob_elv2","Bob mit Pony"},
        {"eyes_highpolyeyes","Detailliert"},{"eyes_low_poly","Einfach"},
        {"clothes_fedora","Fedora"},{"clothes_fedora_cocked","Fedora, schräg"},
        {"clothes_female_casualsuit01","Freizeit 1"},{"clothes_female_casualsuit02","Freizeit 2"},
        {"clothes_female_elegantsuit01","Elegant"},{"clothes_female_sportsuit01","Sport"},
        {"clothes_male_casualsuit01","Freizeit 1"},{"clothes_male_casualsuit02","Freizeit 2"},
        {"clothes_male_casualsuit03","Freizeit 3"},{"clothes_male_casualsuit04","Freizeit 4"},
        {"clothes_male_casualsuit05","Freizeit 5"},{"clothes_male_casualsuit06","Freizeit 6"},
        {"clothes_male_elegantsuit01","Anzug"},{"clothes_male_worksuit01","Arbeitskleidung"}};
    if(auto it=names.find(name); it!=names.end()) {
        // Both templates offer every outfit; the cut is part of the name.
        if(name.rfind("clothes_female_",0)==0) return tr(it->second)+tr(std::string(" (Damen)"));
        if(name.rfind("clothes_male_",0)==0) return tr(it->second)+tr(std::string(" (Herren)"));
        return tr(it->second);
    }
    auto digits=name.find_last_not_of("0123456789");
    std::string number=digits+1<name.size()?std::to_string(std::stoi(name.substr(digits+1))):"";
    if(name.rfind("eyebrows_",0)==0) return tr(std::string("Augenbrauen "))+number;
    if(name.rfind("eyelashes_",0)==0) return tr(std::string("Wimpern "))+number;
    if(name.rfind("clothes_shoes",0)==0) return tr(std::string("Schuhe "))+number;
    return label(name.substr(name.find('_')+1));
}
bool isShoes(const std::string& name) { return name.rfind("clothes_shoes",0)==0; }
bool isHat(const std::string& name) { return name.rfind("clothes_fedora",0)==0; }
bool isOutfit(const std::string& name) { return name.rfind("clothes_",0)==0 && !isShoes(name) && !isHat(name); }

// Previous/next arrows around a combo listing every entry. Returns true when `selected` changed.
bool picker(const char* text, const char* id, const std::vector<std::string>& entries, int& selected) {
    if(entries.empty()) return false;
    int n=int(entries.size()), before=selected=std::clamp(selected,0,n-1);
    float field=theme::row(text), arrow=ImGui::GetFrameHeight(), gap=theme::scale(4);
    ImGui::PushID(id);
    if(ImGui::ArrowButton("##prev",ImGuiDir_Left)) selected=(selected+n-1)%n;
    ImGui::SameLine(0,gap);
    ImGui::SetNextItemWidth(std::max(arrow,field-2*arrow-2*gap-theme::scale(12)));
    if(ImGui::BeginCombo("##list",entries[size_t(selected)].c_str(),ImGuiComboFlags_NoArrowButton)) {
        for(int i=0;i<n;++i) if(ImGui::Selectable(entries[size_t(i)].c_str(),i==selected)) selected=i;
        ImGui::EndCombo();
    }
    ImGui::SameLine(0,gap);
    if(ImGui::ArrowButton("##next",ImGuiDir_Right)) selected=(selected+1)%n;
    ImGui::PopID();
    return selected!=before;
}
// One asset slot (hair style, shoes, ...): every catalog asset accepted by `matches`, optionally "Keine".
template<class Match> bool assetSlot(render::Human& human, const char* text, const char* id, Match matches, bool none) {
    std::vector<std::string> names, entries;
    if(none) { names.push_back(""); entries.push_back(tr("Keine")); }
    for(auto& a:human.catalog()["assets"]) {
        std::string name=a["name"];
        if(matches(name)) { names.push_back(name); entries.push_back(a.contains("label")?tr(a["label"].get<std::string>()):assetLabel(name)); }
    }
    if(names.empty() || (none && names.size()==1)) return false;
    // Alphabetical, so related styles ("Bob, ...") sit together; "Keine" stays first.
    std::vector<size_t> order(names.size());
    for(size_t i=0;i<order.size();++i) order[i]=i;
    std::stable_sort(order.begin()+(none?1:0),order.end(),[&](size_t a,size_t b) { return entries[a]<entries[b]; });
    std::vector<std::string> sortedNames, sortedEntries;
    for(auto i:order) { sortedNames.push_back(names[i]); sortedEntries.push_back(entries[i]); }
    names.swap(sortedNames); entries.swap(sortedEntries);
    auto& selection=human.state["assets"];
    int current=0;
    for(auto& s:selection) for(size_t i=0;i<names.size();++i) if(!names[i].empty() && s==names[i]) current=int(i);
    if(!picker(text,id,entries,current)) return false;
    for(auto it=selection.begin();it!=selection.end();) if(matches(it->get<std::string>())) it=selection.erase(it); else ++it;
    if(!names[size_t(current)].empty()) selection.push_back(names[size_t(current)]);
    return true;
}
bool sameColor(const nlohmann::json& c, const float rgb[3]) {
    if(!c.is_array() || c.size()!=3) return false;
    for(int k=0;k<3;++k) if(!c[k].is_number() || std::abs(c[k].get<float>()-rgb[k])>0.004f) return false;
    return true;
}
struct EyeColor { const char* id; const char* name; float rgb[3]; };
const EyeColor kEyeColors[]={
    {"blue","Blau",{0.27f,0.45f,0.72f}},{"bluegreen","Blaugrün",{0.24f,0.52f,0.52f}},{"brown","Braun",{0.33f,0.19f,0.09f}},
    {"brownlight","Hellbraun",{0.56f,0.38f,0.19f}},{"deepblue","Tiefblau",{0.13f,0.24f,0.58f}},{"green","Grün",{0.3f,0.52f,0.28f}},
    {"grey","Grau",{0.53f,0.57f,0.6f}},{"ice","Eisblau",{0.7f,0.82f,0.9f}},{"lightblue","Hellblau",{0.5f,0.7f,0.9f}}};
const EyeColor* eyeColor(const std::string& id) {
    for(auto& e:kEyeColors) if(id==e.id) return &e;
    return nullptr;
}
}
void CharacterCreator::open(render::Human& human) {
    original_=human.state;
    if(!human.state.value("enabled",false)) human.applyTemplate(render::Human::Template::Male);
    open_=true; dirty_=true; showClothes_=false; error_.clear(); tab_=0; filter_[0]=0;
    human.state["enabled"]=true; setView(View{}); dragging_=panning_=false;
}
void CharacterCreator::limitPitch(float maxPitch) {
    target_.pitch=std::min(target_.pitch,maxPitch); view.pitch=std::min(view.pitch,maxPitch);
}
// Mouse over the free preview area: left drag turns and tilts, the wheel zooms, right drag moves the focus up and down,
// a double click returns to the full body view. The view eases towards the target.
void CharacterCreator::updateCamera() {
    ImGuiIO& io=ImGui::GetIO();
    bool overPreview=io.MousePos.x>previewLeft*io.DisplaySize.x && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive();
    if(overPreview && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) target_=View{};
    if(overPreview && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) dragging_=true;
    if(overPreview && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) panning_=true;
    if(!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging_=false;
    if(!ImGui::IsMouseDown(ImGuiMouseButton_Right)) panning_=false;
    if(dragging_) {
        target_.orbit+=io.MouseDelta.x*0.4f;
        target_.pitch=std::clamp(target_.pitch-io.MouseDelta.y*0.25f,-60.0f,35.0f);
    }
    // Panning moves the body by about as much as the mouse at the current zoom.
    if(panning_) target_.lift=std::clamp(target_.lift+io.MouseDelta.y*target_.distance*0.0012f,-1.0f,1.0f);
    if(overPreview && io.MouseWheel!=0) target_.distance=std::clamp(target_.distance*std::pow(0.87f,io.MouseWheel),0.45f,6.0f);
    if(dragging_ || panning_) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    float k=1.0f-std::exp(-std::min(io.DeltaTime,0.1f)*12.0f);
    view.orbit+=(target_.orbit-view.orbit)*k; view.pitch+=(target_.pitch-view.pitch)*k;
    // Zoom eases in log space, so it feels the same close up and far away.
    view.distance*=std::pow(target_.distance/view.distance,k); view.lift+=(target_.lift-view.lift)*k;
}
CharacterCreator::Action CharacterCreator::cancel(render::Human& human) {
    human.state=original_; open_=false; return Action::Cancel;
}
CharacterCreator::Action CharacterCreator::draw(render::Human& human, render::Character& character,
                                               const std::filesystem::path& preset) {
    if(!open_) return Action::None;
    auto& state=human.state; auto& catalog=human.catalog();
    auto S=[](float v) { return theme::scale(v); };
    ImVec2 screen=ImGui::GetIO().DisplaySize;
    float x=std::max(S(32),screen.x*0.065f);
    float width=std::min(S(550),screen.x*0.43f-x);
    // Keep controls usable at smaller window sizes by allowing their content to scroll.
    width=std::max(S(280),width);
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(screen);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::Begin("##character-creator",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|
        ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoScrollbar);
    theme::backdrop(x+width+S(18));
    previewLeft=std::min((x+width+S(18))/std::max(screen.x,1.0f),0.8f);
    updateCamera();
    theme::heading(ImVec2(x,screen.y*0.065f),"Dein Charakter","Eine Vorlage wählen. Deinen eigenen Weg gestalten.");
    theme::columns(width);
    ImGui::BeginChild("##creator-fields",ImVec2(width+S(12),std::max(S(100),screen.y-ImGui::GetCursorScreenPos().y-S(270))),
        ImGuiChildFlags_None,ImGuiWindowFlags_NoBackground);
    theme::section("VORLAGE");
    const bool female=state.value("gender",1.0f)<0.5f;
    float cardWidth=(width-S(24))/3.0f;
    if(theme::choice("##male","Männlich","Markant & athletisch",!female,cardWidth)) {
        human.applyTemplate(render::Human::Template::Male); dirty_=true;
    }
    ImGui::SameLine(0,S(12));
    if(theme::choice("##female","Weiblich","Weich & ausgewogen",female,cardWidth)) {
        human.applyTemplate(render::Human::Template::Female); dirty_=true;
    }
    ImGui::SameLine(0,S(12));
    if(theme::choice("##random","Zufällig","Überrasch mich",false,cardWidth)) {
        human.randomize(std::random_device{}()); dirty_=true;
    }
    ImGui::Dummy(ImVec2(0,S(6)));
    const char* tabs[]={"Körper","Details","Aussehen","Kleidung"};
    for(int t=0;t<4;++t) {
        if(t) ImGui::SameLine(0,S(4));
        if(theme::tab(tabs[t],tab_==t)) {
            // Opening the clothing tab shows the outfit; it can be hidden again there.
            if(t==3 && tab_!=3 && !showClothes_) { showClothes_=true; dirty_=true; }
            tab_=t;
        }
    }
    auto slider=[&](const char* text,const char* key,float fallback,float lo=0.0f,float hi=1.0f) {
        float v=state.value(key,fallback);
        if(theme::slider(text,(std::string("##")+key).c_str(),&v,lo,hi,"%.2f")) { state[key]=v; dirty_=true; }
    };
    if(tab_==0) {
        theme::section("PROPORTIONEN");
        // Humans with one mesh per sex (MB-Lab) take the sex from the template.
        if(!catalog.contains("sex")) slider("Körperform","gender",1);
        slider("Alter","age",0.5f,0.5f,1.0f);
        slider("Muskelmasse","muscle",0.5f); slider("Gewicht","weight",0.5f);
        slider("Größe","height",0.5f); slider("Proportionen","proportions",0.5f);
        if(!catalog.value("realism",nlohmann::json::array()).empty()) slider("Definition","definition",0.6f);
        if(state.value("gender",1.0f)<0.5f) {
            theme::section("BRUST");
            slider("Volumen","cupsize",0.44f); slider("Form","firmness",0.6f);
        }
        bool genitals=false;
        for(auto& a:catalog["assets"]) genitals|=a["kind"]=="genitals";
        if(genitals) {
            theme::section("INTIMBEREICH");
            if(assetSlot(human,"Genitalien","genitals",[](const std::string& n) { return n.rfind("genitals_",0)==0; },true)) dirty_=true;
            static const std::map<std::string,std::string> de={{"length","Länge"},{"circ","Umfang"},{"testicles","Hoden"}},
                en={{"length","Length"},{"circ","Girth"},{"testicles","Testicles"}};
            const auto& names=english()?en:de;
            for(auto& a:catalog["assets"]) {
                if(a["kind"]!="genitals") continue;
                bool chosen=false;
                for(auto& s:state["assets"]) chosen|=s==a["name"];
                if(!chosen) continue;
                for(auto& s:a.value("sliders",nlohmann::json::array())) {
                    std::string name=s.value("name",std::string()), key="genitals/"+name;
                    auto it=names.find(name);
                    float v=state["details"].value(key,0.0f), lo=s.contains("neg")?-1.0f:0.0f;
                    if(theme::slider(it!=names.end()?it->second.c_str():label(name).c_str(),("##g-"+name).c_str(),&v,lo,1,"%.2f")) {
                        state["details"][key]=v; dirty_=true;
                    }
                }
            }
            theme::muted("Sichtbar, wenn weder Kleidung noch Unterwäsche getragen wird.");
        }
        theme::section("HERKUNFT");
        slider("Afrikanisch","african",0); slider("Asiatisch","asian",0); slider("Europäisch","caucasian",1);
        theme::muted("Mischt die typischen Gesichts- und Körpermerkmale.");
    } else if(tab_==1) {
        ImGui::Dummy(ImVec2(0,S(6)));
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##filter",tr("Details suchen ..."),filter_,sizeof(filter_));
        theme::muted("Rechtsklick auf einen Regler setzt ihn zurück.");
        auto& details=state["details"];
        std::string query=lower(filter_);
        auto group=[&](const std::string& g) {
            std::vector<std::pair<const nlohmann::json*,DetailText>> rows;
            for(auto& s:catalog["sliders"]) if(s["group"]==g) {
                auto field=[&](const char* key) {
                    std::string en=std::string(key)+"En";
                    return english()&&s.contains(en)?s[en].get<std::string>():tr(s.value(key,std::string()));
                };
                auto text=s.contains("label")?DetailText{field("label"),field("lo"),field("hi")}:detailText(s["name"]);
                if(!query.empty() && lower(text.label).find(query)==std::string::npos &&
                   lower(s["name"].get<std::string>()).find(query)==std::string::npos) continue;
                rows.push_back({&s,text});
            }
            if(rows.empty()) return;
            const char* translated=groupLabel(g);
            std::string title=translated?translated:label(g);
            if(query.empty() ? !ImGui::CollapsingHeader(title.c_str()) : (ImGui::TextDisabled("%s",title.c_str()),false)) return;
            for(auto& [s,text]:rows) {
                std::string key=g+"/"+(*s)["name"].get<std::string>();
                bool oneSided=s->value("neg",0)<0;
                float v=details.contains(key)&&details[key].is_number()?details[key].get<float>():0.0f;
                ImGui::PushTextWrapPos(width); ImGui::TextUnformatted(text.label.c_str()); ImGui::PopTextWrapPos();
                ImGui::SetNextItemWidth(width-S(12));
                std::string format=oneSided||text.lo.empty()?"%.2f":v<-0.005f?text.lo+"  %.2f":v>0.005f?text.hi+"  +%.2f":"neutral";
                if(ImGui::SliderFloat(("##"+key).c_str(),&v,oneSided?0.0f:-1.0f,1.0f,format.c_str(),ImGuiSliderFlags_AlwaysClamp)) {
                    details[key]=v; dirty_=true;
                }
                if(ImGui::IsItemClicked(ImGuiMouseButton_Right) && details.contains(key)) { details.erase(key); dirty_=true; }
            }
        };
        static const std::vector<std::string> face={"head","forehead","eyebrows","eyes","nose","cheek","mouth","chin","ears","neck"};
        static const std::vector<std::string> body={"torso","breast","stomach","hip","pelvis","buttocks","arms","hands","legs","feet"};
        theme::section("GESICHT");
        for(auto& g:face) group(g);
        theme::section("KÖRPER");
        for(auto& g:body) group(g);
        std::set<std::string> known(face.begin(),face.end()); known.insert(body.begin(),body.end());
        for(auto& s:catalog["sliders"]) {
            std::string g=s["group"];
            if(known.insert(g).second) group(g);
        }
        ImGui::Dummy(ImVec2(0,S(6)));
        if(!details.empty() && theme::action("Alle Details zurücksetzen",width)) { details=nlohmann::json::object(); dirty_=true; }
    } else if(tab_==2) {
        theme::section("HAUT");
        {
            std::vector<std::string> skins;
            for(auto& s:catalog["skins"]) skins.push_back(skinLabel(s["name"]));
            int skin=state.value("skin",0);
            if(picker("Hauttyp","skin",skins,skin)) { state["skin"]=skin; dirty_=true; }
        }
        slider("Helligkeit","skinTone",0.5f); slider("Wärme","skinWarmth",0.5f);
        theme::section("AUGEN");
        {
            auto& eyes=catalog["eyeMaterials"];
            int current=state.value("eyes",0);
            std::vector<size_t> shown;
            for(size_t i=0;i<eyes.size();++i) if(eyeColor(eyes[i]["name"])) shown.push_back(i);
            ImGui::TextUnformatted(tr("Augenfarbe"));
            float size=S(34), gap=S(6), used=0;
            for(size_t k=0;k<shown.size();++k) {
                auto* e=eyeColor(eyes[shown[k]]["name"]);
                if(k && used+size<=width) ImGui::SameLine(0,gap); else if(k) used=0;
                ImGui::PushID(int(k));
                if(theme::swatch("##eye",e->rgb,current==int(shown[k]),size)) { state["eyes"]=shown[k]; dirty_=true; }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tr(e->name));
                ImGui::PopID(); used+=size+gap;
            }
        }
        if(assetSlot(human,"Augenmodell","eyes",[](const std::string& n) { return n.rfind("eyes_",0)==0; },false)) dirty_=true;
        theme::section("HAARE");
        if(assetSlot(human,"Frisur","hair",[](const std::string& n) { return n.rfind("hair_",0)==0; },true)) dirty_=true;
        // CC-BY assets need visible attribution; show it while such a style is selected.
        for(auto& a:catalog["assets"]) if(a["kind"]=="hair" && a.value("license",std::string("CC0"))!="CC0")
            for(auto& selected:state["assets"]) if(selected==a["name"]) {
                std::string credit=tr(std::string("Frisur von "))+a.value("author",tr(std::string("unbekannt")))+" ("+a["license"].get<std::string>()+")";
                theme::muted(credit.c_str());
            }
        slider("Volumen","hairVolume",0.45f);
        bool custom=state.contains("hairColor");
        if(theme::toggle("Eigene Haarfarbe","##hair-color",&custom)) {
            if(custom) { auto& c=render::Human::hairShades()[2].rgb; state["hairColor"]={c[0],c[1],c[2]}; }
            else state.erase("hairColor");
            dirty_=true;
        }
        if(custom) {
            auto& shades=render::Human::hairShades();
            float size=S(34), gap=S(6), used=0;
            for(size_t i=0;i<shades.size();++i) {
                if(i && used+size<=width) ImGui::SameLine(0,gap); else if(i) used=0;
                ImGui::PushID(int(i));
                if(theme::swatch("##shade",shades[i].rgb,sameColor(state["hairColor"],shades[i].rgb),size)) {
                    state["hairColor"]={shades[i].rgb[0],shades[i].rgb[1],shades[i].rgb[2]}; dirty_=true;
                }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",tr(shades[i].name));
                ImGui::PopID(); used+=size+gap;
            }
            // Fine tuning in HSV; the colour itself is stored as sRGB.
            float h=0,s=0,v=0;
            auto& c=state["hairColor"];
            ImGui::ColorConvertRGBtoHSV(c[0].get<float>(),c[1].get<float>(),c[2].get<float>(),h,s,v);
            bool changed=theme::slider("Farbton","##hair-h",&h,0,1,"%.2f");
            changed|=theme::slider("Sättigung","##hair-s",&s,0,1,"%.2f");
            changed|=theme::slider("Helligkeit","##hair-v",&v,0,1,"%.2f");
            if(changed) { float r,g,b; ImGui::ColorConvertHSVtoRGB(h,s,v,r,g,b); state["hairColor"]={r,g,b}; dirty_=true; }
        } else theme::muted("Die Frisur behält ihre natürliche Farbe.");
        if(assetSlot(human,"Augenbrauen","eyebrows",[](const std::string& n) { return n.rfind("eyebrows_",0)==0; },true)) dirty_=true;
        if(assetSlot(human,"Wimpern","eyelashes",[](const std::string& n) { return n.rfind("eyelashes_",0)==0; },true)) dirty_=true;
        if(state.contains("hairColor")) theme::muted("Augenbrauen und Wimpern übernehmen die Haarfarbe.");
    } else {
        theme::section("KLEIDUNG");
        if(theme::toggle("In Vorschau zeigen","##show-clothes",&showClothes_)) dirty_=true;
        bool underwear=state.value("underwear",true);
        if(theme::toggle("Unterwäsche","##underwear",&underwear)) { state["underwear"]=underwear; dirty_=true; }
        if(assetSlot(human,"Outfit","outfit",isOutfit,true)) dirty_=true;
        if(assetSlot(human,"Schuhe","shoes",isShoes,true)) dirty_=true;
        if(assetSlot(human,"Hut","hat",isHat,true)) dirty_=true;
        theme::muted("Die Kleidung passt sich automatisch an deine Körperform an.");
    }
    ImGui::EndChild();
    float bottom=std::max(ImGui::GetCursorScreenPos().y,screen.y-S(264));
    ImGui::SetCursorScreenPos(ImVec2(x,bottom)); theme::columns(width);
    ImGui::BeginChild("##creator-actions",ImVec2(width+S(12),S(238)),ImGuiChildFlags_None,
        ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    theme::section("VORSCHAU");
    struct Shot { const char* name; float distance, pitch; };
    static const Shot shots[]={{"Ganzkörper",2.6f,-5},{"Oberkörper",1.15f,-4},{"Gesicht",0.75f,-2}};
    for(int s=0;s<3;++s) {
        if(s) ImGui::SameLine(0,S(4));
        bool active=std::abs(std::log(target_.distance/shots[s].distance))<0.12f && std::abs(target_.lift)<0.02f;
        if(theme::tab(shots[s].name,active)) {
            target_.distance=shots[s].distance; target_.pitch=shots[s].pitch; target_.lift=0;
        }
    }
    theme::muted("Ziehen: drehen  ·  Mausrad: zoomen  ·  Rechts ziehen: verschieben  ·  Doppelklick: zurück");
    theme::muted(showClothes_?"Mit Kleidung, so wie du im Spiel aussiehst.":"Ohne Kleidung, damit du jede Form gut erkennen kannst.");
    if(theme::action("Vorlage zurücksetzen",width*0.53f)) {
        human.applyTemplate(state.value("template",std::string("male"))=="female"?render::Human::Template::Female:render::Human::Template::Male); dirty_=true;
    }
    ImGui::SameLine(0,0);
    if(theme::action("Gespeichert laden",width*0.47f)) {
        if(human.loadPreset(preset)) { state["enabled"]=true; dirty_=true; error_.clear(); }
        else error_=tr("Kein gültiger gespeicherter Charakter gefunden.");
    }
    if(dirty_ && human.rebuild(character,!showClothes_)) { dirty_=false; error_.clear(); }
    else if(dirty_) error_=tr("Die Vorschau konnte nicht aktualisiert werden.");
    Action action=Action::None;
    ImGui::BeginDisabled(dirty_);
    if(theme::action("Übernehmen",width*0.53f)) {
        if(human.savePreset(preset)) { open_=false; action=Action::Apply; }
        else error_=tr("Speichern fehlgeschlagen. Bitte erneut versuchen.");
    }
    ImGui::EndDisabled(); ImGui::SameLine(0,0);
    if(theme::action("Abbrechen",width*0.47f)) action=cancel(human);
    if(!error_.empty()) ImGui::TextWrapped("%s",error_.c_str());
    ImGui::EndChild();
    // Match the pause menu's quiet footer instead of a desktop window title bar.
    ImGui::SetCursorScreenPos(ImVec2(x+S(4),screen.y-S(29)));
    theme::muted("Esc  Zurück ohne Änderungen");
    ImGui::End(); ImGui::PopStyleVar(); return action;
}
}
