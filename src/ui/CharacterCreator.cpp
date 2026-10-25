#include "ui/CharacterCreator.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include "ui/Theme.h"
#include <algorithm>
#include <cctype>
#include <map>

namespace ui {
namespace {
std::string label(std::string s) {
    for(auto& c:s) if(c=='_' || c=='-') c=' ';
    if(!s.empty()) s[0]=char(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}
const char* groupLabel(const std::string& group) {
    static const std::map<std::string,const char*> names={
        {"arms","Arme"},{"legs","Beine"},{"torso","Oberkörper"},{"head","Kopfform"},
        {"chin","Kinn & Kiefer"},{"eyes","Augen"},{"nose","Nase"},{"mouth","Mund"},
        {"ears","Ohren"},{"neck","Hals"},{"cheek","Wangen"},{"forehead","Stirn"},
        {"eyebrows","Augenbrauen"},{"breast","Brust"},{"hips","Hüfte"},{"buttocks","Gesäß"},
        {"hands","Hände"},{"feet","Füße"},{"stomach","Bauch"},{"pelvis","Becken"}};
    auto it=names.find(group); return it==names.end()?nullptr:it->second;
}
}
void CharacterCreator::open(render::Human& human) {
    original_=human.state;
    if(!human.state.value("enabled",false)) human.applyTemplate(render::Human::Template::Male);
    open_=true; dirty_=true; error_.clear(); tab_=0; filter_[0]=0;
    human.state["enabled"]=true; orbit=180; distance=2.6f;
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
    theme::heading(ImVec2(x,screen.y*0.065f),"Dein Charakter","Eine Vorlage wählen. Deinen eigenen Weg gestalten.");
    theme::columns(width);
    ImGui::BeginChild("##creator-fields",ImVec2(width+S(12),std::max(S(100),screen.y-ImGui::GetCursorScreenPos().y-S(270))),
        ImGuiChildFlags_None,ImGuiWindowFlags_NoBackground);
    theme::section("VORLAGE");
    const bool female=state.value("gender",1.0f)<0.5f;
    float cardWidth=(width-S(12))*0.5f;
    if(theme::choice("##male","Männlich","Markant & athletisch",!female,cardWidth)) {
        human.applyTemplate(render::Human::Template::Male); dirty_=true;
    }
    ImGui::SameLine(0,S(12));
    if(theme::choice("##female","Weiblich","Weich & ausgewogen",female,cardWidth)) {
        human.applyTemplate(render::Human::Template::Female); dirty_=true;
    }
    ImGui::Dummy(ImVec2(0,S(6)));
    const char* tabs[]={"Körper","Details","Aussehen"};
    for(int t=0;t<3;++t) {
        if(t) ImGui::SameLine(0,S(4));
        if(theme::tab(tabs[t],tab_==t)) tab_=t;
    }
    auto slider=[&](const char* text,const char* key,float fallback,float lo=0.0f,float hi=1.0f) {
        float v=state.value(key,fallback);
        if(theme::slider(text,(std::string("##")+key).c_str(),&v,lo,hi,"%.2f")) { state[key]=v; dirty_=true; }
    };
    if(tab_==0) {
        theme::section("PROPORTIONEN");
        slider("Körperform","gender",1);
        slider("Alter","age",0.5f,0.5f,1.0f);
        slider("Muskelmasse","muscle",0.5f); slider("Gewicht","weight",0.5f);
        slider("Größe","height",0.5f); slider("Proportionen","proportions",0.5f);
        if(state.value("gender",1.0f)<0.5f && ImGui::CollapsingHeader("Brustform")) {
            slider("Volumen","cupsize",0.44f); slider("Form","firmness",0.6f);
        }
        if(ImGui::CollapsingHeader("Gesichtsmerkmale mischen")) {
            slider("Afrikanisch","african",0); slider("Asiatisch","asian",0); slider("Europäisch","caucasian",1);
        }
    } else if(tab_==1) {
        theme::section("GESICHT & KÖRPERDETAILS");
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##filter","Details suchen ...",filter_,sizeof(filter_));
        std::string previous; bool expanded=false;
        for(auto& s:catalog["sliders"]) {
            std::string group=s["group"], name=s["name"], key=group+"/"+name;
            if(group!=previous) {
                const char* translated=groupLabel(group);
                expanded=ImGui::CollapsingHeader(translated?translated:label(group).c_str()); previous=group;
            }
            if(!expanded && !filter_[0]) continue;
            if(filter_[0] && key.find(filter_)==std::string::npos) continue;
            float v=state["details"].value(key,0.0f);
            ImGui::PushTextWrapPos(width); ImGui::TextUnformatted(label(name).c_str()); ImGui::PopTextWrapPos();
            ImGui::SetNextItemWidth(width-S(12));
            if(ImGui::SliderFloat(("##"+key).c_str(),&v,s["neg"].get<int>()<0?0.0f:-1.0f,1.0f,"%.2f",ImGuiSliderFlags_AlwaysClamp)) {
                state["details"][key]=v; dirty_=true;
            }
        }
    } else {
        theme::section("HAUT & AUGEN");
        auto materials=[&](const char* title,const char* key,const char* source) {
            auto& list=catalog[source]; if(list.empty()) return;
            int selected=std::clamp(state.value(key,0),0,int(list.size())-1);
            ImGui::TextUnformatted(title); ImGui::SetNextItemWidth(width-S(12));
            if(ImGui::BeginCombo((std::string("##")+key).c_str(),label(list[selected]["name"]).c_str())) {
                for(size_t i=0;i<list.size();++i) if(ImGui::Selectable(label(list[i]["name"]).c_str(),selected==int(i))) { state[key]=i; dirty_=true; }
                ImGui::EndCombo();
            }
        };
        materials("Haut","skin","skins"); materials("Augenfarbe","eyes","eyeMaterials");
        theme::section("HAARE & GESICHT");
        for(auto kind:{"eyes","eyebrows","eyelashes","hair"}) {
            const char* title=std::string(kind)=="eyes"?"Augen":std::string(kind)=="eyebrows"?"Augenbrauen":std::string(kind)=="eyelashes"?"Wimpern":"Haare";
            ImGui::TextUnformatted(title); ImGui::SetNextItemWidth(width-S(12));
            std::string current="Keine", selectedName;
            for(auto& a:catalog["assets"]) if(a["kind"]==kind)
                for(auto& selected:state["assets"]) if(selected==a["name"]) {
                    selectedName=selected.get<std::string>(); current=label(selectedName.substr(std::string(kind).size()+1));
                }
            auto select=[&](const std::string& name) {
                auto& selection=state["assets"];
                for(auto it=selection.begin();it!=selection.end();) {
                    bool remove=false;
                    for(auto& a:catalog["assets"]) if(a["kind"]==kind && a["name"]==*it) remove=true;
                    if(remove) it=selection.erase(it); else ++it;
                }
                if(!name.empty()) selection.push_back(name); dirty_=true;
            };
            if(ImGui::BeginCombo((std::string("##asset-")+kind).c_str(),current.c_str())) {
                if(ImGui::Selectable("Keine",selectedName.empty())) select("");
                for(auto& a:catalog["assets"]) if(a["kind"]==kind) {
                    std::string name=a["name"];
                    if(ImGui::Selectable(label(name.substr(std::string(kind).size()+1)).c_str(),name==selectedName)) select(name);
                }
                ImGui::EndCombo();
            }
        }
    }
    ImGui::EndChild();
    float bottom=std::max(ImGui::GetCursorScreenPos().y,screen.y-S(264));
    ImGui::SetCursorScreenPos(ImVec2(x,bottom)); theme::columns(width);
    ImGui::BeginChild("##creator-actions",ImVec2(width+S(12),S(238)),ImGuiChildFlags_None,
        ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    theme::section("VORSCHAU");
    theme::slider("Drehen","##orbit",&orbit,0,360,"%.0f°");
    theme::slider("Abstand","##distance",&distance,1.5f,6,"%.1f m");
    theme::muted("Ohne Kleidung, damit du jede Form gut erkennen kannst.");
    if(theme::action("Vorlage zurücksetzen",width*0.53f)) {
        human.applyTemplate(state.value("template",std::string("male"))=="female"?render::Human::Template::Female:render::Human::Template::Male); dirty_=true;
    }
    ImGui::SameLine(0,0);
    if(theme::action("Gespeichert laden",width*0.47f)) {
        if(human.loadPreset(preset)) { state["enabled"]=true; dirty_=true; error_.clear(); }
        else error_="Kein gültiger gespeicherter Charakter gefunden.";
    }
    if(dirty_ && human.rebuild(character,true)) { dirty_=false; error_.clear(); }
    else if(dirty_) error_="Die Vorschau konnte nicht aktualisiert werden.";
    Action action=Action::None;
    ImGui::BeginDisabled(dirty_);
    if(theme::action("Übernehmen",width*0.53f)) {
        if(human.savePreset(preset)) { open_=false; action=Action::Apply; }
        else error_="Speichern fehlgeschlagen. Bitte erneut versuchen.";
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
