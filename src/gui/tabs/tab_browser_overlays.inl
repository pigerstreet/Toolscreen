if (ImGui::BeginTabItem(trc("tabs.browser_overlays"))) {
    static std::unordered_map<int, std::string> s_browserOverlayUrlDrafts;
    static std::unordered_map<int, bool> s_browserOverlayUrlEditing;
    static int s_browserOverlayCssEditorIndex = -1;
    static std::string s_browserOverlayCssEditorText;

    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    SliderCtrlClickTip();

    int browserOverlayToRemove = -1;
    for (size_t i = 0; i < g_config.browserOverlays.size(); ++i) {
        auto& overlay = g_config.browserOverlays[i];
        ImGui::PushID((int)i);

        std::string deleteLabel = "X##delete_browser_overlay_" + std::to_string(i);
        if (ImGui::Button(deleteLabel.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            std::string popupId = (tr("browser.overlays_delete") + "##" + std::to_string(i));
            ImGui::OpenPopup(popupId.c_str());
        }

        std::string popupId = (tr("browser.overlays_delete") + "##" + std::to_string(i));
        if (ImGui::BeginPopupModal(popupId.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(trc("browser.tooltip_delete", overlay.name));
            ImGui::Separator();
            if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                browserOverlayToRemove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        std::string oldOverlayName = overlay.name;
        bool nodeOpen = ImGui::TreeNodeEx("##browser_overlay_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", overlay.name.c_str());

        if (nodeOpen) {
            bool hasDuplicate = HasDuplicateBrowserOverlayName(overlay.name, i);
            if (hasDuplicate) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText(trc("browser.overlays_name"), &overlay.name)) {
                if (!HasDuplicateBrowserOverlayName(overlay.name, i)) {
                    g_configIsDirty = true;
                    if (oldOverlayName != overlay.name) {
                        RemoveBrowserOverlayFromCache(oldOverlayName);
                        for (auto& mode : g_config.modes) {
                            for (auto& overlayId : mode.browserOverlayIds) {
                                if (overlayId == oldOverlayName) { overlayId = overlay.name; }
                            }
                        }
                    }
                } else {
                    overlay.name = oldOverlayName;
                }
            }

            if (hasDuplicate) { ImGui::PopStyleColor(3); }
            if (hasDuplicate) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), trc("images.name_duplicate"));
            }

            ImGui::SeparatorText(trc("browser.overlays_source"));
            std::string& pendingUrl = s_browserOverlayUrlDrafts[(int)i];
            bool& isEditingUrl = s_browserOverlayUrlEditing[(int)i];
            if (!isEditingUrl) {
                pendingUrl = overlay.url;
            }

            ImGui::SetNextItemWidth((std::max)(200.0f, ImGui::GetContentRegionAvail().x - 190.0f));
            ImGui::InputText(trc("browser.overlays_url"), &pendingUrl);
            const bool urlCommitted = ImGui::IsItemDeactivatedAfterEdit();
            isEditingUrl = ImGui::IsItemActive();
            if (urlCommitted && overlay.url != pendingUrl) {
                overlay.url = pendingUrl;
                g_configIsDirty = true;
            }

            ImGui::SameLine();
            if (ImGui::Button((tr("button.refresh") + "##browser_overlay_refresh_" + std::to_string(i)).c_str())) {
                if (overlay.url != pendingUrl) {
                    overlay.url = pendingUrl;
                    g_configIsDirty = true;
                }
                RequestBrowserOverlayRefresh(overlay.name);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("browser.tooltip.refresh"));
            }

            if (ImGui::Button((tr("browser.overlays_edit_custom_css") + "##browser_overlay_css_" + std::to_string(i)).c_str())) {
                s_browserOverlayCssEditorIndex = (int)i;
                s_browserOverlayCssEditorText = overlay.customCss;
                ImGui::OpenPopup("##browser_overlay_css_editor");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("browser.tooltip.custom_css"));
            }

            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.85f, ImGui::GetIO().DisplaySize.y * 0.8f), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("##browser_overlay_css_editor", NULL, ImGuiWindowFlags_NoResize)) {
                ImGui::TextUnformatted(trc("browser.overlays_css_editor"));
                ImGui::Separator();
                float editorHeight = (std::max)(240.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.5f);
                ImGui::InputTextMultiline("##browser_overlay_css_editor_text", &s_browserOverlayCssEditorText,
                                          ImVec2(ImGui::GetContentRegionAvail().x, editorHeight), ImGuiInputTextFlags_AllowTabInput);
                if (ImGui::Button(trc("button.apply"), ImVec2(120, 0))) {
                    if (s_browserOverlayCssEditorIndex >= 0 && s_browserOverlayCssEditorIndex < (int)g_config.browserOverlays.size()) {
                        BrowserOverlayConfig& editedOverlay = g_config.browserOverlays[s_browserOverlayCssEditorIndex];
                        if (editedOverlay.customCss != s_browserOverlayCssEditorText) {
                            editedOverlay.customCss = s_browserOverlayCssEditorText;
                            g_configIsDirty = true;
                        }
                    }
                    s_browserOverlayCssEditorIndex = -1;
                    s_browserOverlayCssEditorText.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) {
                    s_browserOverlayCssEditorIndex = -1;
                    s_browserOverlayCssEditorText.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Columns(2, "browser_source", false);
            ImGui::SetColumnWidth(0, 140);
            ImGui::Text(trc("label.width"));
            ImGui::NextColumn();
            if (Spinner("##browser_width", &overlay.browserWidth, 1, 1, 16384)) { g_configIsDirty = true; }
            ImGui::NextColumn();
            ImGui::Text(trc("label.height"));
            ImGui::NextColumn();
            if (Spinner("##browser_height", &overlay.browserHeight, 1, 1, 16384)) { g_configIsDirty = true; }
            ImGui::Columns(1);

            ImGui::SeparatorText(trc("browser.overlays_rendering"));
            if (ImGui::SliderFloat(trc("label.opacity"), &overlay.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            if (ImGui::Checkbox(trc("browser.overlays_pixelated_scaling"), &overlay.pixelatedScaling)) g_configIsDirty = true;
            if (ImGui::Checkbox(trc("browser.overlays_only_on_my_screen"), &overlay.onlyOnMyScreen)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("browser.tooltip.only_on_my_screen"));
            }

            ImGui::Columns(2, "browser_render", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text(trc("label.x"));
            ImGui::NextColumn();
            if (Spinner("##browser_overlay_x", &overlay.x)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(trc("label.y"));
            ImGui::NextColumn();
            if (Spinner("##browser_overlay_y", &overlay.y)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(trc("label.scale"));
            ImGui::NextColumn();
            float scalePercent = overlay.scale * 100.0f;
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##browser_overlay_scale", &scalePercent, 10.0f, 300.0f, "%.0f%%")) {
                overlay.scale = scalePercent / 100.0f;
                g_configIsDirty = true;
            }
            ImGui::NextColumn();
            ImGui::Text(trc("label.relative_to"));
            ImGui::NextColumn();
            const char* currentRelTo = getFriendlyName(overlay.relativeTo, imageRelativeToOptions);
            ImGui::SetNextItemWidth(180);
            if (ImGui::BeginCombo("##browser_overlay_rel_to", currentRelTo)) {
                for (const auto& option : imageRelativeToOptions) {
                    if (ImGui::Selectable(option.second, overlay.relativeTo == option.first)) {
                        overlay.relativeTo = option.first;
                        g_configIsDirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Columns(1);

            ImGui::SeparatorText(trc("browser.overlays_cropping"));
            ImGui::Columns(2, "browser_crop", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text(trc("window.overlays_crop_top"));
            ImGui::NextColumn();
            if (Spinner("##browser_overlay_crop_t", &overlay.crop_top, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(trc("window.overlays_crop_bottom"));
            ImGui::NextColumn();
            if (Spinner("##browser_overlay_crop_b", &overlay.crop_bottom, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(trc("window.overlays_crop_left"));
            ImGui::NextColumn();
            if (Spinner("##browser_overlay_crop_l", &overlay.crop_left, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(trc("window.overlays_crop_right"));
            ImGui::NextColumn();
            if (Spinner("##browser_overlay_crop_r", &overlay.crop_right, 1, 0)) g_configIsDirty = true;
            ImGui::Columns(1);

            ImGui::SeparatorText(trc("browser.overlays_capture_settings"));
            ImGui::Columns(2, "browser_capture", false);
            ImGui::SetColumnWidth(0, 150);
            ImGui::Text(trc("label.fps"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderInt("##browser_overlay_fps", &overlay.fps, 1, 60, "%d fps")) { g_configIsDirty = true; }
            ImGui::NextColumn();
            ImGui::Text(trc("browser.overlays_reload_interval"));
            ImGui::NextColumn();
            float reloadIntervalSeconds = overlay.reloadInterval / 1000.0f;
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##browser_reload_interval", &reloadIntervalSeconds, 0.0f, 60.0f, "%.1f s")) {
                overlay.reloadInterval = static_cast<int>(reloadIntervalSeconds * 1000.0f);
                g_configIsDirty = true;
            }
            ImGui::Columns(1);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("browser.tooltip.reload_interval"));
            }

            if (ImGui::Checkbox(trc("browser.overlays_transparent_background"), &overlay.transparentBackground)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("browser.tooltip.transparent_background"));
            }

            if (ImGui::Checkbox(trc("browser.overlays_reload_on_update"), &overlay.reloadOnUpdate)) g_configIsDirty = true;
            if (ImGui::Checkbox(trc("browser.overlays_mute_audio"), &overlay.muteAudio)) g_configIsDirty = true;
            if (ImGui::Checkbox(trc("browser.overlays_allow_system_media_keys"), &overlay.allowSystemMediaKeys)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("browser.tooltip.allow_system_media_keys"));
            }

            ImGui::SeparatorText(trc("browser.overlays_background"));
            if (ImGui::Checkbox(trc("browser.overlays_enable_background"), &overlay.background.enabled)) g_configIsDirty = true;
            ImGui::BeginDisabled(!overlay.background.enabled);
            if (ImGui::ColorEdit3(trc("browser.overlays_bg_color"), &overlay.background.color.r)) g_configIsDirty = true;
            if (ImGui::SliderFloat(trc("browser.overlays_bg_opacity"), &overlay.background.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            ImGui::EndDisabled();

            ImGui::SeparatorText(trc("window.overlays_color_keying"));
            if (ImGui::Checkbox(trc("window.overlays_enable_color_keying"), &overlay.enableColorKey)) g_configIsDirty = true;
            ImGui::BeginDisabled(!overlay.enableColorKey);

            int colorKeyToRemove = -1;
            for (size_t k = 0; k < overlay.colorKeys.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                auto& ck = overlay.colorKeys[k];

                ImGui::Text("Key %zu:", k + 1);
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);
                if (ImGui::ColorEdit3("##browser_color", &ck.color.r, ImGuiColorEditFlags_NoLabel)) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                ImGui::PushItemWidth(80.0f);
                if (ImGui::SliderFloat("##browser_sens", &ck.sensitivity, 0.001f, 1.0f, "%.3f")) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("X##remove_browser_color_key")) { colorKeyToRemove = static_cast<int>(k); }
                ImGui::PopID();
            }

            if (colorKeyToRemove >= 0) {
                overlay.colorKeys.erase(overlay.colorKeys.begin() + colorKeyToRemove);
                g_configIsDirty = true;
            }

            ImGui::BeginDisabled(overlay.colorKeys.size() >= ConfigDefaults::MAX_COLOR_KEYS);
            if (ImGui::Button(trc("window.overlays_add_color_key"))) {
                ColorKeyConfig newKey;
                newKey.color = { 0.0f, 0.0f, 0.0f };
                newKey.sensitivity = 0.05f;
                overlay.colorKeys.push_back(newKey);
                g_configIsDirty = true;
            }
            ImGui::EndDisabled();

            ImGui::EndDisabled();

            ImGui::SeparatorText(trc("browser.overlays_border"));
            if (ImGui::Checkbox((tr("browser.overlays_enable_border") + "##BrowserOverlay").c_str(), &overlay.border.enabled)) {
                g_configIsDirty = true;
            }

            if (overlay.border.enabled) {
                ImGui::Text(trc("images.border_color"));
                ImVec4 borderCol = ImVec4(overlay.border.color.r, overlay.border.color.g, overlay.border.color.b, 1.0f);
                if (ImGui::ColorEdit3("##BrowserOverlayBorderColor", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                    overlay.border.color = { borderCol.x, borderCol.y, borderCol.z };
                    g_configIsDirty = true;
                }

                ImGui::Text(trc("images.border_width"));
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BrowserOverlayBorderWidth", &overlay.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.px"));

                ImGui::Text(trc("images.border_radius"));
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BrowserOverlayBorderRadius", &overlay.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.px"));
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    if (browserOverlayToRemove >= 0) {
        std::string deletedOverlayName = g_config.browserOverlays[browserOverlayToRemove].name;
        RemoveBrowserOverlayFromCache(deletedOverlayName);
        g_config.browserOverlays.erase(g_config.browserOverlays.begin() + browserOverlayToRemove);
        for (auto& mode : g_config.modes) {
            auto it = std::find(mode.browserOverlayIds.begin(), mode.browserOverlayIds.end(), deletedOverlayName);
            while (it != mode.browserOverlayIds.end()) {
                mode.browserOverlayIds.erase(it);
                it = std::find(mode.browserOverlayIds.begin(), mode.browserOverlayIds.end(), deletedOverlayName);
            }
        }
        g_configIsDirty = true;
    }

    ImGui::Separator();
    if (ImGui::Button(trc("browser.overlays_add_new_browser_overlay"))) {
        BrowserOverlayConfig newOverlay;
        newOverlay.name = tr("browser.overlays_new_browser_overlay") + " " + std::to_string(g_config.browserOverlays.size() + 1);
        newOverlay.relativeTo = "centerViewport";
        g_config.browserOverlays.push_back(newOverlay);
        g_configIsDirty = true;

        if (!g_currentModeId.empty()) {
            for (auto& mode : g_config.modes) {
                if (mode.id == g_currentModeId) {
                    if (std::find(mode.browserOverlayIds.begin(), mode.browserOverlayIds.end(), newOverlay.name) ==
                        mode.browserOverlayIds.end()) {
                        mode.browserOverlayIds.push_back(newOverlay.name);
                    }
                    break;
                }
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button((tr("button.reset_defaults") + "##browseroverlays").c_str())) {
        ImGui::OpenPopup(trc("browser.overlays_reset_to_defaults"));
    }

    if (ImGui::BeginPopupModal(trc("browser.overlays_reset_to_defaults"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("label.warning"));
        ImGui::Text(trc("browser.overlays_warning_reset_to_defaults"));
        ImGui::Text(trc("label.action_cannot_be_undone"));
        ImGui::Separator();
        if (ImGui::Button(trc("button.confirm_reset"), ImVec2(120, 0))) {
            for (const auto& overlay : g_config.browserOverlays) { RemoveBrowserOverlayFromCache(overlay.name); }
            g_config.browserOverlays = GetDefaultBrowserOverlays();
            for (auto& mode : g_config.modes) { mode.browserOverlayIds.clear(); }
            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::EndTabItem();
}
