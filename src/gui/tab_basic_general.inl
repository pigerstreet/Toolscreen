if (ImGui::BeginTabItem(trc("tabs.general"))) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    SliderCtrlClickTip();

    ImGui::SeparatorText(trc("label.window"));
    {
        HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        const bool canToggleBorderless = (hwnd != NULL && IsWindow(hwnd));

        if (!canToggleBorderless) { ImGui::BeginDisabled(); }
        if (ImGui::Button(trc("general.go_borderless"), ImVec2(150, 0))) {
            ToggleBorderlessWindowedFullscreen(hwnd);
        }
        if (!canToggleBorderless) { ImGui::EndDisabled(); }

        ImGui::SameLine();
        HelpMarker(trc("general.tooltip.go_borderless"));
    }

    auto RenderInlineHotkeyBinding = [&](const std::string& targetModeId, const char* label) {
        int hotkeyIdx = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            if (EqualsIgnoreCase(g_config.hotkeys[i].mainMode, "Fullscreen") &&
                EqualsIgnoreCase(g_config.hotkeys[i].secondaryMode, targetModeId)) {
                hotkeyIdx = static_cast<int>(i);
                break;
            }
        }

        ImGui::SameLine();
        ImGui::Text(trc("label.hotkey"));
        ImGui::SameLine();

        if (hotkeyIdx != -1) {
            std::string keyStr = GetKeyComboString(g_config.hotkeys[hotkeyIdx].keys);
            bool isBinding = (s_mainHotkeyToBind == hotkeyIdx);
            const char* buttonLabel = isBinding ? "[Press Keys...]" : (keyStr.empty() ? "[Click to Bind]" : keyStr.c_str());

            ImGui::PushID(label);
            if (ImGui::Button(buttonLabel, ImVec2(120, 0))) {
                s_mainHotkeyToBind = hotkeyIdx;
                s_altHotkeyToBind = { -1, -1 };
                s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
            }
            ImGui::PopID();
        } else {
            ImGui::TextDisabled("[No hotkey]");
        }
    };

    auto EnsureModeExists = [&](const std::string& modeId, int width, int height) {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return;
        }
        ModeConfig newMode;
        newMode.id = modeId;
        newMode.width = width;
        newMode.height = height;
        newMode.background.selectedMode = "color";
        newMode.background.color = { 0.0f, 0.0f, 0.0f };
        g_config.modes.push_back(newMode);
        g_configIsDirty = true;
    };

    auto EnsureHotkeyForMode = [&](const std::string& targetModeId) {
        for (const auto& hotkey : g_config.hotkeys) {
            if (EqualsIgnoreCase(hotkey.mainMode, "Fullscreen") && EqualsIgnoreCase(hotkey.secondaryMode, targetModeId)) {
                return;
            }
        }
        HotkeyConfig newHotkey;
        newHotkey.keys = std::vector<DWORD>();
        newHotkey.mainMode = "Fullscreen";
        newHotkey.secondaryMode = targetModeId;
        newHotkey.debounce = 100;
        g_config.hotkeys.push_back(newHotkey);
        ResizeHotkeySecondaryModes(g_config.hotkeys.size());
        SetHotkeySecondaryMode(g_config.hotkeys.size() - 1, targetModeId);
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
        g_configIsDirty = true;
    };

    auto RemoveModeAndHotkey = [&](const std::string& modeId) {
        for (auto it = g_config.modes.begin(); it != g_config.modes.end(); ++it) {
            if (EqualsIgnoreCase(it->id, modeId)) {
                g_config.modes.erase(it);
                break;
            }
        }
        g_config.hotkeys.erase(std::remove_if(g_config.hotkeys.begin(), g_config.hotkeys.end(),
                                              [&](const HotkeyConfig& h) { return EqualsIgnoreCase(h.secondaryMode, modeId); }),
                               g_config.hotkeys.end());
        ResetAllHotkeySecondaryModes();
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
        g_configIsDirty = true;

        if (EqualsIgnoreCase(g_config.defaultMode, modeId)) {
            g_config.defaultMode = "Fullscreen";
        }

        if (EqualsIgnoreCase(g_currentModeId, modeId)) {
            std::string fallbackMode = (EqualsIgnoreCase(g_config.defaultMode, modeId) || g_config.defaultMode.empty())
                                           ? "Fullscreen" : g_config.defaultMode;
            std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
            g_pendingModeSwitch.pending = true;
            g_pendingModeSwitch.modeId = fallbackMode;
            g_pendingModeSwitch.source = "Basic mode disabled";
            g_pendingModeSwitch.forceInstant = true;
        }
    };

    auto ModeExists = [&](const std::string& modeId) -> bool {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return true;
        }
        return false;
    };

    ImGui::SeparatorText(trc("label.modes"));

    auto HasHotkeyBound = [&](const std::string& modeId) -> bool {
        for (const auto& hotkey : g_config.hotkeys) {
            if (EqualsIgnoreCase(hotkey.mainMode, "Fullscreen") && EqualsIgnoreCase(hotkey.secondaryMode, modeId)) {
                return !hotkey.keys.empty();
            }
        }
        return false;
    };

    auto RenderModeHotkeyBinding = [&](const std::string& targetModeId, const char* label) {
        int hotkeyIdx = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            if (EqualsIgnoreCase(g_config.hotkeys[i].mainMode, "Fullscreen") &&
                EqualsIgnoreCase(g_config.hotkeys[i].secondaryMode, targetModeId)) {
                hotkeyIdx = static_cast<int>(i);
                break;
            }
        }

        if (hotkeyIdx == -1) return;

        std::string keyStr = GetKeyComboString(g_config.hotkeys[hotkeyIdx].keys);
        bool isBinding = (s_mainHotkeyToBind == hotkeyIdx);
        const char* buttonLabel = isBinding ? "[Press Keys...]" : (keyStr.empty() ? "[Click to Bind]" : keyStr.c_str());

        ImGui::PushID(label);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 60, 100, 180));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 80, 120, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(80, 100, 140, 220));
        float columnWidth = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(buttonLabel, ImVec2(columnWidth, 0))) {
            s_mainHotkeyToBind = hotkeyIdx;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
            MarkHotkeyBindingActive();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    };

    auto GetModeConfig = [&](const std::string& modeId) -> ModeConfig* {
        for (auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return &mode;
        }
        return nullptr;
    };

    enum class EyeZoomInlineKind {
        None,
        LabelsOnly,
        ControlsOnly,
    };

    auto RenderModeTableRow = [&](const std::string& modeId, const char* label, const char* hotkeyLabel, int defaultWidth,
                                  int defaultHeight, int maxWidth, int maxHeight,
                                  EyeZoomInlineKind eyezoomInline = EyeZoomInlineKind::None, bool readOnlyDimensions = false) {
        ModeConfig* modeConfig = GetModeConfig(modeId);
        const bool relativeOnlySizing = EqualsIgnoreCase(modeId, "Fullscreen") || EqualsIgnoreCase(modeId, "Thin") || EqualsIgnoreCase(modeId, "Wide");
        const int safeMaxWidth = (std::max)(1, maxWidth);
        const int safeMaxHeight = (std::max)(1, maxHeight);

        EnsureHotkeyForMode(modeId);

        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Text("%s", label);

        if (readOnlyDimensions) { ImGui::BeginDisabled(); }

        ImGui::TableNextColumn();
        if (modeConfig) {
            ImGui::PushID((std::string(label) + "_width").c_str());
            if (relativeOnlySizing) {
                float minWidthPct = 1.0f;
                if (EqualsIgnoreCase(modeId, "Thin")) {
                    minWidthPct = (std::min)(100.0f, (330.0f / static_cast<float>(safeMaxWidth)) * 100.0f);
                }

                float widthPct = ((modeConfig->relativeWidth >= 0.0f && modeConfig->relativeWidth <= 1.0f)
                                      ? modeConfig->relativeWidth
                                      : (static_cast<float>(modeConfig->width) / static_cast<float>(safeMaxWidth))) *
                                 100.0f;
                widthPct = (std::max)(minWidthPct, (std::min)(100.0f, widthPct));

                if (ImGui::SliderFloat("##w_pct", &widthPct, minWidthPct, 100.0f, "%.1f%%")) {
                    modeConfig->useRelativeSize = true;
                    if (!modeConfig->widthExpr.empty()) { modeConfig->widthExpr.clear(); }
                    modeConfig->relativeWidth = widthPct / 100.0f;

                    int computedWidth = static_cast<int>(modeConfig->relativeWidth * static_cast<float>(safeMaxWidth));
                    if (EqualsIgnoreCase(modeId, "Thin")) {
                        computedWidth = (std::max)(330, computedWidth);
                    }
                    modeConfig->width = (std::max)(1, computedWidth);
                    g_configIsDirty = true;
                }
            } else if (Spinner("##w", &modeConfig->width, 10, 1, maxWidth, 64, 3)) {
                if (!modeConfig->widthExpr.empty()) { modeConfig->widthExpr.clear(); }
                modeConfig->relativeWidth = -1.0f;
                g_configIsDirty = true;
            }
            ImGui::PopID();
        }

        ImGui::TableNextColumn();
        if (modeConfig) {
            ImGui::PushID((std::string(label) + "_height").c_str());
            if (relativeOnlySizing) {
                float heightPct = ((modeConfig->relativeHeight >= 0.0f && modeConfig->relativeHeight <= 1.0f)
                                       ? modeConfig->relativeHeight
                                       : (static_cast<float>(modeConfig->height) / static_cast<float>(safeMaxHeight))) *
                                  100.0f;
                heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));

                if (ImGui::SliderFloat("##h_pct", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                    modeConfig->useRelativeSize = true;
                    if (!modeConfig->heightExpr.empty()) { modeConfig->heightExpr.clear(); }
                    modeConfig->relativeHeight = heightPct / 100.0f;
                    modeConfig->height = (std::max)(1, static_cast<int>(modeConfig->relativeHeight * static_cast<float>(safeMaxHeight)));
                    g_configIsDirty = true;
                }
            } else if (Spinner("##h", &modeConfig->height, 10, 1, maxHeight, 64, 3)) {
                if (!modeConfig->heightExpr.empty()) { modeConfig->heightExpr.clear(); }
                modeConfig->relativeHeight = -1.0f;
                g_configIsDirty = true;
            }
            ImGui::PopID();
        }

        if (readOnlyDimensions) { ImGui::EndDisabled(); }

        ImGui::TableNextColumn();
        RenderModeHotkeyBinding(modeId, hotkeyLabel);

        ImGui::TableNextColumn();
        if (eyezoomInline != EyeZoomInlineKind::None) {
            ImGui::PushID((std::string("eyezoom_inline_settings_") + modeId).c_str());

            if (ImGui::BeginTable("##eyezoom_inline_tbl", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextRow();

                if (eyezoomInline == EyeZoomInlineKind::LabelsOnly) {
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(trc("eyezoom.clone_width"));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(trc("eyezoom.overlay_pixels"));
                    ImGui::SameLine();
                    HelpMarker(trc("eyezoom.tooltip"));
                } else if (eyezoomInline == EyeZoomInlineKind::ControlsOnly) {
                    ImGui::TableSetColumnIndex(0);
                    int maxCloneWidth = (modeConfig ? modeConfig->width : maxWidth);
                    if (maxCloneWidth < 2) maxCloneWidth = 2;
                    if (Spinner("##EyeZoomCloneWidth", &g_config.eyezoom.cloneWidth, 2, 2, maxCloneWidth, 64, 3)) {
                        if (g_config.eyezoom.cloneWidth % 2 != 0) { g_config.eyezoom.cloneWidth = (g_config.eyezoom.cloneWidth / 2) * 2; }
                        int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                        if (g_config.eyezoom.overlayWidth > maxOverlay) g_config.eyezoom.overlayWidth = maxOverlay;
                        g_configIsDirty = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    {
                        int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                        if (Spinner("##EyeZoomOverlayWidth", &g_config.eyezoom.overlayWidth, 1, 0, maxOverlay, 64, 3)) g_configIsDirty = true;
                    }
                }

                ImGui::EndTable();
            }

            ImGui::PopID();
        }
    };

    if (ImGui::BeginTable("ModeTable", 5, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Width", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Height", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Hotkey", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("EyeZoom Settings", ImGuiTableColumnFlags_WidthFixed, 240);

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        const char* headers[] = { trc("general.mode_table.mode"), trc("general.mode_table.width"), trc("general.mode_table.height"), trc("general.mode_table.hotkey"), trc("general.mode_table.eyezoom_settings") };
        for (int i = 0; i < 5; i++) {
            ImGui::TableSetColumnIndex(i);
            float columnWidth = ImGui::GetColumnWidth();
            float textWidth = ImGui::CalcTextSize(headers[i]).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - textWidth) * 0.5f);
            ImGui::TableHeader(headers[i]);
        }

        int monitorWidth = GetCachedWindowWidth();
        int monitorHeight = GetCachedWindowHeight();

        RenderModeTableRow("Fullscreen", "Fullscreen", "fullscreen_hotkey", monitorWidth, monitorHeight, monitorWidth, monitorHeight,
                           EyeZoomInlineKind::None, false);

        RenderModeTableRow("Thin", "Thin", "thin_hotkey", 400, monitorHeight, monitorWidth, monitorHeight, EyeZoomInlineKind::None);

        RenderModeTableRow("Wide", "Wide", "wide_hotkey", monitorWidth, 400, monitorWidth, monitorHeight, EyeZoomInlineKind::LabelsOnly);

        RenderModeTableRow("EyeZoom", "EyeZoom", "eyezoom_hotkey", 384, 16384, monitorWidth, 16384, EyeZoomInlineKind::ControlsOnly);

        ImGui::EndTable();
    }

    ImGui::SeparatorText(trc("label.sensitivity"));

    RawInputSensitivityNote();
    ImGui::Text(trc("general.global_sensitivity"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("##globalSensBasic", &g_config.mouseSensitivity, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker(trc("general.sens.tooltip.global_sensitivity"));

    {
        ModeConfig* eyezoomMode = GetModeConfig("EyeZoom");
        if (eyezoomMode) {
            RawInputSensitivityNote();
            ImGui::Text(trc("general.eyezoom_sensitivity"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("##eyezoomSensBasic", &eyezoomMode->modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                if (eyezoomMode->modeSensitivity < 0.001f) eyezoomMode->modeSensitivity = 0.001f;
                eyezoomMode->sensitivityOverrideEnabled = true;
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("general.sens.tooltip.eyezoom_sensitivity"));
        }
    }

    ImGui::Separator();
    ImGui::SeparatorText(trc("label.overlays"));

    {
        auto FindNinjabrainBotImage = [&]() -> ImageConfig* {
            for (auto& img : g_config.images) {
                if (EqualsIgnoreCase(img.name, "Ninjabrain Bot")) { return &img; }
            }
            return nullptr;
        };

        auto CreateNinjabrainBotImage = [&]() {
            WCHAR tempPath[MAX_PATH];
            if (GetTempPathW(MAX_PATH, tempPath) > 0) {
                std::wstring nbImagePath = std::wstring(tempPath) + L"nb-overlay.png";
                ImageConfig ninjabrainBot;
                ninjabrainBot.name = "Ninjabrain Bot";
                ninjabrainBot.path = WideToUtf8(nbImagePath);
                ninjabrainBot.x = 0;
                ninjabrainBot.y = 0;
                ninjabrainBot.scale = 1.2f;
                ninjabrainBot.relativeTo = "topLeft";
                ninjabrainBot.opacity = 1.0f;
                ninjabrainBot.colorKey = { 55 / 255.0f, 60 / 255.0f, 66 / 255.0f };
                ninjabrainBot.enableColorKey = true;
                ninjabrainBot.colorKeySensitivity = 0.05f;
                ninjabrainBot.background = { true, { 0.0f, 0.0f, 0.0f }, 0.5f };
                g_config.images.push_back(ninjabrainBot);
                g_allImagesLoaded = false;
                g_pendingImageLoad = true;
            }
        };

        auto ModeHasNinjabrain = [&](const std::string& modeId) -> bool {
            ModeConfig* mode = GetModeConfig(modeId);
            if (!mode) return false;
            for (const auto& imgId : mode->imageIds) {
                if (EqualsIgnoreCase(imgId, "Ninjabrain Bot")) return true;
            }
            return false;
        };

        auto AddNinjabrainToMode = [&](const std::string& modeId) {
            ModeConfig* mode = GetModeConfig(modeId);
            if (mode && !ModeHasNinjabrain(modeId)) { mode->imageIds.push_back("Ninjabrain Bot"); }
        };

        auto RemoveNinjabrainFromMode = [&](const std::string& modeId) {
            ModeConfig* mode = GetModeConfig(modeId);
            if (mode) {
                mode->imageIds.erase(std::remove_if(mode->imageIds.begin(), mode->imageIds.end(),
                                                    [](const std::string& id) { return EqualsIgnoreCase(id, "Ninjabrain Bot"); }),
                                     mode->imageIds.end());
            }
        };

        bool ninjabrainEnabled =
            ModeHasNinjabrain("Fullscreen") || ModeHasNinjabrain("EyeZoom") || ModeHasNinjabrain("Thin") || ModeHasNinjabrain("Wide");

        if (ImGui::Checkbox(trc("general.ninjabrainbot_overlay"), &ninjabrainEnabled)) {
            if (ninjabrainEnabled) {
                if (!FindNinjabrainBotImage()) { CreateNinjabrainBotImage(); }
                AddNinjabrainToMode("Fullscreen");
                AddNinjabrainToMode("EyeZoom");
                AddNinjabrainToMode("Thin");
                AddNinjabrainToMode("Wide");
            } else {
                RemoveNinjabrainFromMode("Fullscreen");
                RemoveNinjabrainFromMode("EyeZoom");
                RemoveNinjabrainFromMode("Thin");
                RemoveNinjabrainFromMode("Wide");
            }
            g_configIsDirty = true;
        }
    }

    ImGui::SeparatorText(trc("general.mirrors"));
    ImGui::TextDisabled(trc("general.tooltip.mirrors"));

    auto RenderMirrorAssignments = [&](const std::string& modeId, const char* label) {
        ModeConfig* modeConfig = GetModeConfig(modeId);
        if (!modeConfig) return;

        ImGui::PushID(label);
        if (ImGui::TreeNode(label)) {
            int item_idx_to_remove = -1;
            bool remove_is_group = false;

            for (size_t k = 0; k < modeConfig->mirrorIds.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                    item_idx_to_remove = static_cast<int>(k);
                    remove_is_group = false;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(modeConfig->mirrorIds[k].c_str());
                ImGui::PopID();
            }

            for (size_t k = 0; k < modeConfig->mirrorGroupIds.size(); ++k) {
                ImGui::PushID(static_cast<int>(k) + 10000);
                if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                    item_idx_to_remove = static_cast<int>(k);
                    remove_is_group = true;
                }
                ImGui::SameLine();
                ImGui::Text("%s %s", trc("general.group"), modeConfig->mirrorGroupIds[k].c_str());
                ImGui::PopID();
            }

            if (item_idx_to_remove != -1) {
                if (remove_is_group) {
                    modeConfig->mirrorGroupIds.erase(modeConfig->mirrorGroupIds.begin() + item_idx_to_remove);
                } else {
                    modeConfig->mirrorIds.erase(modeConfig->mirrorIds.begin() + item_idx_to_remove);
                }
                g_configIsDirty = true;
            }

            if (ImGui::BeginCombo("##AddMirrorOrGroup", trc("general.add_mirror_or_group"))) {
                for (const auto& mirrorConf : g_config.mirrors) {
                    if (std::find(modeConfig->mirrorIds.begin(), modeConfig->mirrorIds.end(), mirrorConf.name) ==
                        modeConfig->mirrorIds.end()) {
                        if (ImGui::Selectable(mirrorConf.name.c_str())) {
                            modeConfig->mirrorIds.push_back(mirrorConf.name);
                            g_configIsDirty = true;
                        }
                    }
                }
                if (!g_config.mirrors.empty() && !g_config.mirrorGroups.empty()) { ImGui::Separator(); }
                for (const auto& groupConf : g_config.mirrorGroups) {
                    if (std::find(modeConfig->mirrorGroupIds.begin(), modeConfig->mirrorGroupIds.end(), groupConf.name) ==
                        modeConfig->mirrorGroupIds.end()) {
                        std::string displayName = std::string(trc("general.group")) + " " + groupConf.name;
                        if (ImGui::Selectable(displayName.c_str())) {
                            modeConfig->mirrorGroupIds.push_back(groupConf.name);
                            g_configIsDirty = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    RenderMirrorAssignments("Fullscreen", "Fullscreen");
    RenderMirrorAssignments("Thin", "Thin");
    RenderMirrorAssignments("Wide", "Wide");
    RenderMirrorAssignments("EyeZoom", "EyeZoom");

    ImGui::EndTabItem();
}


