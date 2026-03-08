if (ImGui::BeginTabItem(trc("tabs.settings"))) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    SliderCtrlClickTip();

    ImGui::SeparatorText(trc("settings.performance"));

    ImGui::Text(trc("label.fps_limit"));
    ImGui::SetNextItemWidth(600);
    int fpsLimitValue = (g_config.fpsLimit == 0) ? 1001 : g_config.fpsLimit;
    bool sliderActive = ImGui::IsItemActive() || ImGui::IsItemHovered();
    if (ImGui::SliderInt("##fpsLimit", &fpsLimitValue, 30, 1001, fpsLimitValue == 1001 ? trc("label.unlimited") : "%d fps")) {
        g_config.fpsLimit = (fpsLimitValue == 1001) ? 0 : fpsLimitValue;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.fps_limit.advanced"));

    ImGui::Spacing();
    ImGui::SeparatorText(trc("settings.capture_streaming"));
    if (ImGui::Checkbox(trc("settings.hide_animations_in_game"), &g_config.hideAnimationsInGame)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.hide_animations_in_game"));

/*    if (ImGui::Checkbox("Disable Fullscreen Prompt", &g_config.disableFullscreenPrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the fullscreen toast prompt (toast2).\n"
               "When disabled, toast2 appears in fullscreen and starts fading out after 10 seconds.");

    if (ImGui::Checkbox("Disable Configure Prompt", &g_config.disableConfigurePrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the configure toast prompt (toast1) shown in windowed mode.");*/

    ImGui::Spacing();
    ImGui::SeparatorText(trc("settings.mirrors"));
    {
        const char* gammaModes[] = { trc("settings.mirrors_auto"), trc("settings.mirrors_assume_srgb"), trc("settings.mirrors_assume_linear") };
        int gm = static_cast<int>(g_config.mirrorGammaMode);
        ImGui::SetNextItemWidth(250);
        if (ImGui::Combo(trc("settings.mirrors_match_colorspace"), &gm, gammaModes, IM_ARRAYSIZE(gammaModes))) {
            g_config.mirrorGammaMode = static_cast<MirrorGammaMode>(gm);
            g_configIsDirty = true;

            SetGlobalMirrorGammaMode(g_config.mirrorGammaMode);

            std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
            for (auto& kv : g_mirrorInstances) {
                kv.second.forceUpdateFrames = 3;
                kv.second.hasValidContent = false;
            }
        }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.colorspace"));
    }

    bool driverInstalled = IsVirtualCameraDriverInstalled();
    bool inUseByOBS = driverInstalled && IsVirtualCameraInUseByOBS();
    ImGui::BeginDisabled(!driverInstalled || inUseByOBS);
    bool vcEnabled = g_config.debug.virtualCameraEnabled;
    if (ImGui::Checkbox(trc("settings.enable_virtual_camera"), &vcEnabled)) {
        g_config.debug.virtualCameraEnabled = vcEnabled;
        g_configIsDirty = true;
        if (vcEnabled) {
            int screenW = GetCachedWindowWidth();
            int screenH = GetCachedWindowHeight();
            if (screenW <= 0 || screenH <= 0) {
                screenW = GetSystemMetrics(SM_CXSCREEN);
                screenH = GetSystemMetrics(SM_CYSCREEN);
            }
            StartVirtualCamera(screenW, screenH);
        } else {
            StopVirtualCamera();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!driverInstalled) {
        ImGui::TextDisabled(trc("settings.virtual.camera_not_installed"));
    } else if (inUseByOBS) {
        ImGui::TextDisabled(trc("settings.virtual.camera_in_use"));
    } else {
        HelpMarker(trc("settings.tooltip.virtual_camera"));
    }

    ImGui::Spacing();

    static bool s_debugUnlocked = false;
    static char s_passcodeInput[16] = "";

    if (!s_debugUnlocked) {
        if (ImGui::Button(trc("button.debug"))) {
            ImGui::OpenPopup(trc("settings.debug_passcode"));
            memset(s_passcodeInput, 0, sizeof(s_passcodeInput));
        }

        if (ImGui::BeginPopupModal(trc("settings.debug_passcode"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(trc("settings.debug_passcode_prompt"));
            ImGui::Spacing();

            ImGui::SetNextItemWidth(150);
            bool enterPressed = ImGui::InputText("##passcode", s_passcodeInput, sizeof(s_passcodeInput),
                                                 ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Spacing();

            if (ImGui::Button(trc("button.ok"), ImVec2(80, 0)) || enterPressed) {
                if (strcmp(s_passcodeInput, "5739") == 0) {
                    s_debugUnlocked = true;
                    ImGui::CloseCurrentPopup();
                } else {
                    memset(s_passcodeInput, 0, sizeof(s_passcodeInput));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(trc("button.cancel"), ImVec2(80, 0))) { ImGui::CloseCurrentPopup(); }

            ImGui::EndPopup();
        }
    } else {
        ImGui::SeparatorText(trc("settings.debug_options"));
        if (ImGui::Checkbox(trc("settings.delay_rendering_until_finished"), &g_config.debug.delayRenderingUntilFinished)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.delay_rendering_until_finished"));
        if (ImGui::Checkbox(trc("settings.delay_rendering_until_blitted"), &g_config.debug.delayRenderingUntilBlitted)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.delay_rendering_until_blitted"));
        ImGui::Spacing();
        if (ImGui::Checkbox(trc("settings.show_performance_overlay"), &g_config.debug.showPerformanceOverlay)) { g_configIsDirty = true; }
        if (ImGui::Checkbox(trc("settings.show_profiler"), &g_config.debug.showProfiler)) { g_configIsDirty = true; }
        ImGui::SetNextItemWidth(300);
        if (ImGui::SliderFloat(trc("settings.profiler_scale"), &g_config.debug.profilerScale, 0.25f, 2.0f, "%.2f")) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.profiler_scale"));
        if (ImGui::Checkbox(trc("settings.show_hotkey_debug"), &g_config.debug.showHotkeyDebug)) { g_configIsDirty = true; }
        if (ImGui::Checkbox(trc("settings.fake_cursor_overlay"), &g_config.debug.fakeCursor)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.fake_cursor"));
        if (ImGui::Checkbox(trc("settings.show_texture_grid"), &g_config.debug.showTextureGrid)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.show_texture_grid"));

        ImGui::Spacing();
        if (ImGui::CollapsingHeader(trc("settings.advanced_logging"))) {
            ImGui::Indent();
            ImGui::TextDisabled(trc("settings.enable_verbose_logging"));
            ImGui::Spacing();
            if (ImGui::Checkbox(trc("settings.log_mode_switch"), &g_config.debug.logModeSwitch)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_animation"), &g_config.debug.logAnimation)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_hotkey"), &g_config.debug.logHotkey)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_obs"), &g_config.debug.logObs)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_window_overlay"), &g_config.debug.logWindowOverlay)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_file_monitor"), &g_config.debug.logFileMonitor)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_image_monitor"), &g_config.debug.logImageMonitor)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_performance"), &g_config.debug.logPerformance)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_texture_ops"), &g_config.debug.logTextureOps)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_gui"), &g_config.debug.logGui)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_init"), &g_config.debug.logInit)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.log_cursor_textures"), &g_config.debug.logCursorTextures)) { g_configIsDirty = true; }
            ImGui::Unindent();
        }
    }
    ImGui::EndTabItem();
}


