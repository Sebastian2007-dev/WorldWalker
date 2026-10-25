#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "render/Human.h"
#include <fstream>
#include <cmath>
#include <stdexcept>

void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }

int main() {
    int result=0;
    glfwInit(); glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE); glfwWindowHint(GLFW_FOCUSED,GLFW_FALSE);
    auto window=glfwCreateWindow(320,240,"Human integration test",nullptr,nullptr);
    gfx::Device device; device.init(window,true);
    gfx::Bindless bindless; bindless.init(device);
    render::Character character;
    character.init(device,bindless,std::filesystem::path(WW_ASSET_DIR)/"baked/character",VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_D32_SFLOAT);
    auto temporary=std::filesystem::path(WW_ASSET_DIR).parent_path()/"build/human-test-preset.json";
    try {
        render::Human human;
        require(human.load(std::filesystem::path(WW_ASSET_DIR)/"baked/human"),"human data load");
        auto defaults=human.state;
        require(human.catalog()["sliders"].size()>100,"missing detail morphs");
        require(human.catalog()["assets"].size()>20,"missing fitted assets");
        human.state["gender"]=0.13f;
        human.state["details"]["nose/nose-trans-up-down"]=0.4f;
        auto expected=human.state;
        require(human.savePreset(temporary),"save preset"); human.reset();
        require(human.loadPreset(temporary) && human.state==expected,"preset round trip");
        { std::ofstream f(temporary); f << R"({"version":1,"details":{"broken":"bad"}})"; }
        require(!human.loadPreset(temporary) && human.state==expected,"invalid detail must leave state intact");
        { std::ofstream f(temporary); f << "{broken"; }
        require(!human.loadPreset(temporary) && human.state==expected,"malformed JSON must leave state intact");

        auto frame=[&] {
            gfx::FrameContext fc; require(device.beginFrame(fc),"begin test frame");
            character.update(0.016f,{}); character.prepare(fc.frameIndex);
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=fc.swapImage; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount=1; dependency.pImageMemoryBarriers=&barrier;
            vkCmdPipelineBarrier2(fc.cmd,&dependency); device.endFrame(fc);
        };
        auto rebuild=[&] {
            require(human.rebuild(character),"morph/proxy rebuild");
            for(auto& bone:character.skeleton()) require(std::isfinite(bone.pos.z) && glm::length(bone.pos)<5,"invalid morphed skeleton");
        };
        for(auto body:{render::Human::Template::Male,render::Human::Template::Female}) {
            human.applyTemplate(body);
            auto appearance=human.state;
            require(human.rebuild(character,true),"unclothed template preview"); frame();
            require(human.state==appearance,"preview must preserve clothing selection");
            auto previewSkeleton=character.skeleton();
            require(human.rebuild(character),"restore template clothing"); frame();
            require(human.state==appearance,"restoring clothes must preserve appearance");
            require(character.skeleton().size()==previewSkeleton.size(),"preview skeleton mismatch");
            for(size_t b=0;b<previewSkeleton.size();++b)
                require(glm::length(character.skeleton()[b].pos-previewSkeleton[b].pos)<1e-5f,"clothing must not alter body proportions");
            require(human.savePreset(temporary),"save template"); human.reset();
            require(human.loadPreset(temporary) && human.state==appearance,"template persistence");
        }
        // Two edits before a frame cover pending uploads; subsequent edits cover buffers still in flight.
        for(float gender:{0.0f,1.0f}) for(float shape:{0.0f,1.0f}) {
            human.state=defaults; human.state["gender"]=gender; human.state["height"]=shape;
            human.state["muscle"]=shape; human.state["weight"]=shape;
            rebuild(); rebuild(); frame();
        }
        for(auto& asset:human.catalog()["assets"]) {
            human.state=defaults; human.state["assets"]=nlohmann::json::array({asset["name"]});
            rebuild(); frame();
        }
        for(int i=0;i<3;++i) frame();
        std::puts("PASS: preset round trip, corrupt preset rollback, extreme body shapes, all fitted assets, repeated GPU mesh replacement");
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); result=1; }
    std::error_code error; std::filesystem::remove(temporary,error);
    device.waitIdle(); character.shutdown(); bindless.shutdown(); device.shutdown();
    glfwDestroyWindow(window); glfwTerminate(); return result;
}
