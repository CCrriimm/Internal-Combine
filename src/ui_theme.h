#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <type_traits>

namespace CiyEUi {
inline constexpr ImU32 Accent = IM_COL32(46, 195, 214, 255);
inline constexpr ImU32 Text = IM_COL32(237, 237, 239, 255);
inline constexpr ImU32 Muted = IM_COL32(164, 166, 174, 255);
inline constexpr ImU32 Ground = IM_COL32(15, 16, 18, 255);
inline constexpr ImU32 Surface = IM_COL32(22, 23, 26, 255);
inline constexpr ImU32 Border = IM_COL32(41, 42, 47, 255);
inline constexpr ImU32 Elevated = IM_COL32(33, 34, 38, 255);
inline constexpr ImU32 Tertiary = IM_COL32(125, 128, 138, 255);
inline constexpr ImU32 Success = IM_COL32(61, 203, 130, 255);
inline constexpr ImU32 Danger = IM_COL32(240, 85, 90, 255);
inline constexpr ImU32 Warning = IM_COL32(242, 180, 65, 255);
inline constexpr ImU32 OnAccent = IM_COL32(17, 18, 20, 255);
inline void ApplyStyle(float scale = 1.0f) {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.FontSizeBase = 13 * scale;
    s.WindowRounding = 12; s.ChildRounding = 10; s.FrameRounding = 6;
    s.PopupRounding = 8; s.GrabRounding = 5; s.ScrollbarRounding = 6;
    s.WindowBorderSize = 1; s.ChildBorderSize = 1; s.FrameBorderSize = 1;
    s.WindowPadding = ImVec2(24, 18); s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 0); s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 3; s.CellPadding = ImVec2(16, 0);
    s.TabRounding = 4; s.TabBorderSize = 0;
    auto* c = s.Colors;
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(Text);
    c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(Muted);
    c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(Surface);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(.085f, .085f, .095f, 1);
    c[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(Border);
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = ImGui::ColorConvertU32ToFloat4(Elevated);
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = ImVec4(.18f, .185f, .20f, 1);
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = ImVec4(.16f, .165f, .18f, 1);
    c[ImGuiCol_Header] = ImVec4(.13f, .135f, .145f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabSelectedOverline] = ImGui::ColorConvertU32ToFloat4(Accent);
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_TabHovered] = ImGui::ColorConvertU32ToFloat4(Elevated);
    c[ImGuiCol_HeaderActive] = ImVec4(.28f, .245f, .39f, 1);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(Accent);
    c[ImGuiCol_SliderGrabActive] = ImVec4(.76f, .68f, 1, 1);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(Border);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(.28f, .28f, .32f, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(.40f, .37f, .49f, 1);
    if (scale != 1) s.ScaleAllSizes(scale);
}

// Geometry and face opacity follow DrawLogoMark in the supplied 34Cheat UI.
inline void Logo(ImDrawList* dl, ImVec2 p, float size) {
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), Accent, size * .28f);
    auto P = [&](float x, float y) { return ImVec2(p.x + size*x, p.y + size*y); };
    dl->AddQuadFilled(P(.50f,.20f), P(.77f,.355f), P(.50f,.51f), P(.23f,.355f), OnAccent);
    dl->AddQuadFilled(P(.23f,.355f), P(.50f,.51f), P(.50f,.82f), P(.23f,.665f), IM_COL32(17,18,20,158));
    dl->AddQuadFilled(P(.50f,.51f), P(.77f,.355f), P(.77f,.665f), P(.50f,.82f), IM_COL32(17,18,20,77));
    const float seam = std::max(1.f, size*.035f);
    dl->AddLine(P(.50f,.51f), P(.50f,.83f), Accent, seam);
    dl->AddLine(P(.50f,.51f), P(.22f,.35f), Accent, seam);
    dl->AddLine(P(.50f,.51f), P(.78f,.35f), Accent, seam);
}

inline void Shadow(ImDrawList* dl, ImVec2 p, ImVec2 size) {
    for (int i=8; i>=1; --i) {
        const float t=i/8.f, e=28*t;
        const int alpha=static_cast<int>(255*.7f*(1-t)*(1-t)*.5f);
        dl->AddRectFilled(ImVec2(p.x-e,p.y-e+12), ImVec2(p.x+size.x+e,p.y+size.y+e),
                          IM_COL32(0,0,0,alpha), 12+e);
    }
}

inline void NavIndicator(int active, float width) {
    const ImVec2 p=ImGui::GetCursorScreenPos();
    auto* state=ImGui::GetStateStorage();
    const auto id=ImGui::GetID("nav.indicator");
    float y=state->GetFloat(id,active*38.f);
    y+=(active*38.f-y)*(1-std::exp(-20*ImGui::GetIO().DeltaTime));
    state->SetFloat(id,y);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x,p.y+y),ImVec2(p.x+width,p.y+y+36),Elevated,8);
}

// Native slider input keeps keyboard, focus and clamping while the track matches the reference.
template<typename T>
inline bool Slider(const char* label, T* value, T lo, T hi, const char* format) {
    ImGui::PushID(label);
    const ImVec2 p=ImGui::GetCursorScreenPos();
    const float width=ImGui::GetContentRegionAvail().x;
    char text[64]; std::snprintf(text,sizeof(text),format,*value);
    const float number_width=ImGui::CalcTextSize(text).x;
    auto* dl=ImGui::GetWindowDrawList();
    const char* end=label;
    while (*end && !(end[0]=='#' && end[1]=='#')) ++end;
    dl->PushClipRect(p,ImVec2(p.x+std::max(0.f,width-number_width-12),p.y+20),true);
    dl->AddText(p,Muted,label,end); dl->PopClipRect();
    ImGui::Dummy(ImVec2(width,20));
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(0,6));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,12);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,ImVec4(0,0,0,0));
    const ImGuiID slider_id=ImGui::GetID("##value");
    const bool text_input=ImGui::TempInputIsActive(slider_id)||ImGui::GetIO().KeyCtrl||ImGui::GetCurrentContext()->NavActivateId==slider_id;
    ImGui::PushStyleColor(ImGuiCol_Text,text_input?ImGui::ColorConvertU32ToFloat4(Text):ImVec4(0,0,0,0));
    const bool changed=ImGui::SliderScalar("##value",std::is_integral_v<T>?ImGuiDataType_S32:ImGuiDataType_Float,
                                         value,&lo,&hi,format,ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor();
    const bool typing=ImGui::TempInputIsActive(slider_id);
    const bool emphasized=ImGui::IsItemHovered()||ImGui::IsItemActive();
    const ImVec2 a=ImGui::GetItemRectMin(), b=ImGui::GetItemRectMax();
    ImGui::PopStyleColor(5); ImGui::PopStyleVar(3);
    std::snprintf(text,sizeof(text),format,*value);
    dl->AddText(ImVec2(p.x+width-ImGui::CalcTextSize(text).x,p.y),Text,text);
    const float t=hi>lo?std::clamp(float((*value-lo)/double(hi-lo)),0.f,1.f):0.f;
    const float left=a.x+8, right=b.x-8, y=(a.y+b.y)*.5f, x=left+(right-left)*t;
    if(!typing) {
        dl->AddRectFilled(ImVec2(left,y-2),ImVec2(right,y+2),Elevated,2);
        if(t>0) dl->AddRectFilled(ImVec2(left,y-2),ImVec2(std::max(left+4,x),y+2),Accent,2);
        dl->AddCircleFilled(ImVec2(x,y),emphasized?7.f:5.5f,Text);
    }
    ImGui::Dummy(ImVec2(0,3));
    ImGui::PopID();
    return changed;
}

enum class Icon { Eye, Target, Skins, Settings };
inline void DrawIcon(ImDrawList* dl, ImVec2 p, Icon icon, ImU32 color) {
    const float t = 1.5f;
    if (icon == Icon::Eye) {
        dl->AddBezierCubic(ImVec2(p.x-7,p.y), ImVec2(p.x-3,p.y-6), ImVec2(p.x+3,p.y-6), ImVec2(p.x+7,p.y), color,t);
        dl->AddBezierCubic(ImVec2(p.x-7,p.y), ImVec2(p.x-3,p.y+6), ImVec2(p.x+3,p.y+6), ImVec2(p.x+7,p.y), color,t);
        dl->AddCircle(p,2.2f,color,16,t);
    } else if (icon == Icon::Target) {
        dl->AddCircle(p,4.7f,color,20,t);
        dl->AddLine(ImVec2(p.x-8,p.y),ImVec2(p.x-3,p.y),color,t);
        dl->AddLine(ImVec2(p.x+3,p.y),ImVec2(p.x+8,p.y),color,t);
        dl->AddLine(ImVec2(p.x,p.y-8),ImVec2(p.x,p.y-3),color,t);
        dl->AddLine(ImVec2(p.x,p.y+3),ImVec2(p.x,p.y+8),color,t);
    } else if (icon == Icon::Skins) {
        ImVec2 points[] = {{p.x-3,p.y-6},{p.x-7,p.y-3},{p.x-5,p.y},{p.x-3,p.y-1},{p.x-3,p.y+6},
                          {p.x+3,p.y+6},{p.x+3,p.y-1},{p.x+5,p.y},{p.x+7,p.y-3},{p.x+3,p.y-6},
                          {p.x+1,p.y-4},{p.x-1,p.y-4}};
        dl->AddPolyline(points,12,color,t,ImDrawFlags_Closed);
    } else {
        dl->AddCircle(p,4,color,20,t);
        for (int i=0;i<8;++i) {
            float a=i*3.14159265f/4;
            dl->AddLine(ImVec2(p.x+std::cos(a)*6,p.y+std::sin(a)*6),ImVec2(p.x+std::cos(a)*8,p.y+std::sin(a)*8),color,t);
        }
    }
}

inline bool NavItem(const char* label, Icon icon, bool active, float width) {
    ImGui::PushID(label);
    ImVec2 p=ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0,0,0,0));
    bool pressed=ImGui::Button("##nav",ImVec2(width,36));
    bool hovered=ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);
    auto* dl=ImGui::GetWindowDrawList();
    if(hovered&&!active) dl->AddRectFilled(p,ImVec2(p.x+width,p.y+36),IM_COL32(255,255,255,9),8);
    DrawIcon(dl,ImVec2(p.x+20,p.y+18),icon,active?Accent:Muted);
    dl->AddText(ImVec2(p.x+40,p.y+(36-ImGui::GetFontSize())*.5f),active?Text:Muted,label);
    ImGui::PopID();
    return pressed;
}

inline bool Toggle(const char* label, bool* value, float width=0, float scale=1) {
    ImGui::PushID(label);
    const ImVec2 p=ImGui::GetCursorScreenPos();
    const float h=30*scale, track_w=32*scale, track_h=18*scale;
    if(width<=0) width=ImGui::CalcTextSize(label,nullptr,true).x+48*scale;
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0,0,0,0));
    const bool pressed=ImGui::Button("##switch",ImVec2(width,h));
    if(pressed) *value=!*value;
    const bool hovered=ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);
    auto* dl=ImGui::GetWindowDrawList();
    if(hovered) dl->AddRectFilled(p,ImVec2(p.x+width,p.y+h),IM_COL32(33,34,38,140),6*scale);
    ImGuiID anim_id=ImGui::GetID("position");
    auto* storage=ImGui::GetStateStorage();
    float x=storage->GetFloat(anim_id,*value?1.f:0.f);
    x+=((*value?1.f:0.f)-x)*(1-std::exp(-20*ImGui::GetIO().DeltaTime));
    storage->SetFloat(anim_id,x);
    const ImVec2 track(p.x+width-track_w,p.y+(h-track_h)*.5f);
    ImVec4 off=ImGui::ColorConvertU32ToFloat4(IM_COL32(33,34,38,255));
    ImVec4 on=ImGui::ColorConvertU32ToFloat4(Accent);
    ImVec4 fill(off.x+(on.x-off.x)*x,off.y+(on.y-off.y)*x,off.z+(on.z-off.z)*x,1);
    dl->AddRectFilled(track,ImVec2(track.x+track_w,track.y+track_h),ImGui::GetColorU32(fill),track_h*.5f);
    if(x<.5f) dl->AddRect(track,ImVec2(track.x+track_w,track.y+track_h),IM_COL32(255,255,255,18),track_h*.5f);
    dl->AddCircleFilled(ImVec2(track.x+9*scale+14*scale*x,track.y+9*scale),6*scale,
                        ImGui::GetColorU32(*value?ImGui::ColorConvertU32ToFloat4(OnAccent):ImGui::ColorConvertU32ToFloat4(Muted)));
    ImGui::PushStyleColor(ImGuiCol_Text,ImGui::ColorConvertU32ToFloat4((*value||hovered)?Text:Muted));
    const char* end=label;
    while(*end && !(end[0]=='#' && end[1]=='#')) ++end;
    dl->PushClipRect(p,ImVec2(track.x-8*scale,p.y+h),true);
    dl->AddText(ImVec2(p.x,p.y+(h-ImGui::GetFontSize())*.5f),ImGui::GetColorU32(ImGuiCol_Text),label,end);
    dl->PopClipRect();
    ImGui::PopStyleColor();
    ImGui::PopID();
    return pressed;
}

inline bool CloseButton(const char* id, ImVec2 size) {
    const ImVec2 p=ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    bool pressed=ImGui::Button(id,size);
    ImGui::PopStyleColor();
    const ImVec2 center(p.x+size.x*.5f,p.y+size.y*.5f);
    auto* dl=ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(center.x-3,center.y-3),ImVec2(center.x+3,center.y+3),Muted,1.5f);
    dl->AddLine(ImVec2(center.x-3,center.y+3),ImVec2(center.x+3,center.y-3),Muted,1.5f);
    return pressed;
}
inline const char* Rarity(unsigned quality) {
    switch (quality) {
    case 4: return "Legendary"; case 5: return "Ultra"; case 6: return "Eternal";
    case 7: return "EVO"; case 8: return "XTREME"; default: return "";
    }
}
}
