if (ImGui::BeginTabItem(trc("tabs.other"))) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    SliderCtrlClickTip();

    ImGui::SeparatorText(trc("hotkeys.gui_hotkey"));
    ImGui::PushID("basic_gui_hotkey");
    std::string guiKeyStr = GetKeyComboString(g_config.guiHotkey);

    ImGui::Text(trc("hotkeys.gui_hotkey_open_close"));
    ImGui::SameLine();

    bool isBindingGui = (s_mainHotkeyToBind == -999);
    const char* guiButtonLabel = isBindingGui ? trc("hotkeys.press_keys") : (guiKeyStr.empty() ? trc("hotkeys.click_to_bind") : guiKeyStr.c_str());
    if (ImGui::Button(guiButtonLabel, ImVec2(150, 0))) {
        s_mainHotkeyToBind = -999;
        s_altHotkeyToBind = { -1, -1 };
        s_exclusionToBind = { -1, -1 };
            MarkHotkeyBindingActive();
    }
    ImGui::PopID();

    ImGui::SeparatorText(trc("label.overlay_visibility_hotkeys"));

    ImGui::PushID("basic_image_overlay_toggle_hotkey");
    {
        const bool imgOverlaysVisible = g_imageOverlaysVisible.load(std::memory_order_acquire);
        const ImVec4 visibleGreen = ImVec4(0.20f, 1.00f, 0.20f, 1.00f);
        const ImVec4 hiddenRed = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);

        std::string imgKeyStr = GetKeyComboString(g_config.imageOverlaysHotkey);
        ImGui::Text(trc("label.toggle_image_overlays"));
        ImGui::SameLine();
        const bool isBindingImg = (s_mainHotkeyToBind == -997);
        const char* imgBtnLabel = isBindingImg ? trc("hotkeys.press_keys") : (imgKeyStr.empty() ? trc("hotkeys.click_to_bind") : imgKeyStr.c_str());
        if (ImGui::Button(imgBtnLabel, ImVec2(150, 0))) {
            s_mainHotkeyToBind = -997;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.question_mark"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(trc("tooltip.toggle_image_overlays.basic"));
        }

        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.status"));
        ImGui::SameLine();
        ImGui::TextColored(imgOverlaysVisible ? visibleGreen : hiddenRed, "%s", imgOverlaysVisible ? trc("label.shown") : trc("label.hidden"));
    }
    ImGui::PopID();

    ImGui::PushID("basic_window_overlay_toggle_hotkey");
    {
        const bool winOverlaysVisible = g_windowOverlaysVisible.load(std::memory_order_acquire);
        const ImVec4 visibleGreen = ImVec4(0.20f, 1.00f, 0.20f, 1.00f);
        const ImVec4 hiddenRed = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);

        std::string winKeyStr = GetKeyComboString(g_config.windowOverlaysHotkey);
        ImGui::Text(trc("label.toggle_window_overlays"));
        ImGui::SameLine();
        const bool isBindingWin = (s_mainHotkeyToBind == -996);
        const char* winBtnLabel = isBindingWin ? trc("hotkeys.press_keys") : (winKeyStr.empty() ? trc("hotkeys.click_to_bind") : winKeyStr.c_str());
        if (ImGui::Button(winBtnLabel, ImVec2(150, 0))) {
            s_mainHotkeyToBind = -996;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.question_mark"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(trc("tooltip.toggle_window_overlays.basic"));
        }

        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.status"));
        ImGui::SameLine();
        ImGui::TextColored(winOverlaysVisible ? visibleGreen : hiddenRed, "%s", winOverlaysVisible ? trc("label.shown") : trc("label.hidden"));
    }
    ImGui::PopID();

    ImGui::SeparatorText(trc("hotkeys.window_hotkeys"));
    ImGui::PushID("basic_borderless_hotkey");
    std::string borderlessKeyStr = GetKeyComboString(g_config.borderlessHotkey);

    ImGui::Text(trc("label.toggle_borderless"));
    ImGui::SameLine();

    bool isBindingBorderless = (s_mainHotkeyToBind == -998);
    const char* borderlessButtonLabel =
        isBindingBorderless ? trc("hotkeys.press_keys") : (borderlessKeyStr.empty() ? trc("hotkeys.click_to_bind") : borderlessKeyStr.c_str());
    if (ImGui::Button(borderlessButtonLabel, ImVec2(150, 0))) {
        s_mainHotkeyToBind = -998;
        s_altHotkeyToBind = { -1, -1 };
        s_exclusionToBind = { -1, -1 };
            MarkHotkeyBindingActive();
    }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.toggle_borderless"));
    ImGui::PopID();

    {
        ImGui::PushID("basic_auto_borderless");
        ImGui::Text(trc("label.auto_borderless"));
        ImGui::SameLine();
        const char* label = g_config.autoBorderless ? trc("label.enabled") : trc("label.disabled");
        if (ImGui::Button(label, ImVec2(150, 0))) {
            g_config.autoBorderless = !g_config.autoBorderless;
            g_configIsDirty = true;
        }
        ImGui::SameLine();
        HelpMarker(trc("tooltip.auto_borderless"));
        ImGui::PopID();
    }

    ImGui::SeparatorText(trc("label.display_settings"));

    ImGui::Text(trc("label.fps_limit"));
    ImGui::SetNextItemWidth(300);
    int fpsLimitValue = (g_config.fpsLimit == 0) ? 1001 : g_config.fpsLimit;
    if (ImGui::SliderInt("##FpsLimit", &fpsLimitValue, 30, 1001, fpsLimitValue == 1001 ? trc("label.unlimited") : "%d fps")) {
        g_config.fpsLimit = (fpsLimitValue == 1001) ? 0 : fpsLimitValue;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.fps_limit.advanced"));

    if (ImGui::Checkbox(trc("label.hide_animations_in_game"), &g_config.hideAnimationsInGame)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.hide_animations_in_game"));

/*    if (ImGui::Checkbox("Disable Fullscreen Prompt", &g_config.disableFullscreenPrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the fullscreen toast prompt (toast2).\n"
               "When disabled, toast2 appears in fullscreen and starts fading out after 10 seconds.");

    if (ImGui::Checkbox("Disable Configure Prompt", &g_config.disableConfigurePrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the configure toast prompt (toast1) shown in windowed mode.");*/

    ImGui::SeparatorText(trc("label.font"));

    ImGui::Text(trc("label.font_path"));
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText("##FontPath", &g_config.fontPath)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.font"));

    ImGui::EndTabItem();
}


