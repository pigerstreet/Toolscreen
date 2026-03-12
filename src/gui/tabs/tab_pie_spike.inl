if (ImGui::BeginTabItem("Pie Spike")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    ImGui::SeparatorText("Pie Chart Spike Detection");

    if (ImGui::Checkbox("Enable spike detection", &g_config.pieSpike.enabled)) {
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Automatically detects when the F3 pie chart shows a silverfish spawner spike (portal room).\n\nReads from the 'Pie' mirror if available, otherwise falls back to manual capture offsets.");

    if (!g_config.pieSpike.enabled) { ImGui::BeginDisabled(); }

    ImGui::Spacing();
    ImGui::SeparatorText("Spike Targets");

    for (int i = 0; i < (int)g_config.pieSpike.targets.size(); i++) {
        auto& t = g_config.pieSpike.targets[i];
        ImGui::PushID(i);

        if (ImGui::Checkbox("##enabled", &t.enabled)) { g_configIsDirty = true; }
        ImGui::SameLine();

        char nameBuf[128];
        strncpy(nameBuf, t.name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
            t.name = nameBuf;
            g_configIsDirty = true;
        }
        ImGui::SameLine();

        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderFloat("Ratio", &t.ratio, 0.0f, 1.0f, "%.3f")) { g_configIsDirty = true; }
        ImGui::SameLine();

        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderFloat("Tol", &t.tolerance, 0.001f, 0.2f, "%.3f")) { g_configIsDirty = true; }
        ImGui::SameLine();

        if (ImGui::Button("X##remove")) {
            g_config.pieSpike.targets.erase(g_config.pieSpike.targets.begin() + i);
            g_configIsDirty = true;
            ImGui::PopID();
            i--;
            continue;
        }

        ImGui::PopID();
    }

    if (ImGui::Button("+ Add Target")) {
        PieSpikeTarget newTarget;
        newTarget.name = "New Target";
        g_config.pieSpike.targets.push_back(newTarget);
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Each target defines a ratio range. Alert triggers when any enabled target matches.\n\nPure: spawner only (~0.10)\nAverage: spawner + chest in front (~0.15)\nBig Orange: chest behind spawner (~0.55)");

    ImGui::Spacing();
    ImGui::SeparatorText("Detection Settings");

    if (ImGui::SliderFloat("Color match threshold", &g_config.pieSpike.colorThreshold, 0.01f, 0.5f, "%.3f")) {
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Maximum Euclidean distance (0-1 per channel) for a pixel to be classified as orange or green.");

    ImGui::Spacing();
    ImGui::SeparatorText("Reference Colors");

    float orangeCol[3] = { g_config.pieSpike.orangeReference.r, g_config.pieSpike.orangeReference.g, g_config.pieSpike.orangeReference.b };
    if (ImGui::ColorEdit3("Orange reference", orangeCol)) {
        g_config.pieSpike.orangeReference.r = orangeCol[0];
        g_config.pieSpike.orangeReference.g = orangeCol[1];
        g_config.pieSpike.orangeReference.b = orangeCol[2];
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("The pie chart color for block entities (orange slice). Default: RGB(233, 109, 77).");

    float greenCol[3] = { g_config.pieSpike.greenReference.r, g_config.pieSpike.greenReference.g, g_config.pieSpike.greenReference.b };
    if (ImGui::ColorEdit3("Green reference", greenCol)) {
        g_config.pieSpike.greenReference.r = greenCol[0];
        g_config.pieSpike.greenReference.g = greenCol[1];
        g_config.pieSpike.greenReference.b = greenCol[2];
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("The pie chart color for unspecified entities (green slice). Default: RGB(69, 204, 101).");

    ImGui::Spacing();
    ImGui::SeparatorText("Timing");

    if (ImGui::InputInt("Sample rate (ms)", &g_config.pieSpike.sampleRateMs, 50, 200)) {
        if (g_config.pieSpike.sampleRateMs < 50) g_config.pieSpike.sampleRateMs = 50;
        if (g_config.pieSpike.sampleRateMs > 5000) g_config.pieSpike.sampleRateMs = 5000;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("How often (in ms) to sample the pie chart. Lower = more responsive, higher = less GPU usage.");

    if (ImGui::InputInt("Cooldown (ms)", &g_config.pieSpike.cooldownMs, 500, 1000)) {
        if (g_config.pieSpike.cooldownMs < 500) g_config.pieSpike.cooldownMs = 500;
        if (g_config.pieSpike.cooldownMs > 30000) g_config.pieSpike.cooldownMs = 30000;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Minimum time between alerts to prevent spam.");

    if (ImGui::InputInt("Capture size (px)", &g_config.pieSpike.captureSize, 16, 64)) {
        if (g_config.pieSpike.captureSize < 32) g_config.pieSpike.captureSize = 32;
        if (g_config.pieSpike.captureSize > 512) g_config.pieSpike.captureSize = 512;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Size of the square region captured around the pie chart center.");

    if (ImGui::InputInt("Offset X (from right)", &g_config.pieSpike.captureOffsetX, 1, 10)) {
        if (g_config.pieSpike.captureOffsetX < 0) g_config.pieSpike.captureOffsetX = 0;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Pixels from the right edge of the window to the pie chart center.");

    if (ImGui::InputInt("Offset Y (from bottom)", &g_config.pieSpike.captureOffsetY, 1, 10)) {
        if (g_config.pieSpike.captureOffsetY < 0) g_config.pieSpike.captureOffsetY = 0;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Pixels from the bottom edge of the window to the pie chart center.");

    ImGui::Spacing();
    ImGui::SeparatorText("Alerts");

    if (ImGui::Checkbox("Visual alert (screen flash)", &g_config.pieSpike.visualAlert)) {
        g_configIsDirty = true;
    }

    if (ImGui::Checkbox("Sound alert", &g_config.pieSpike.soundAlert)) {
        g_configIsDirty = true;
    }

    {
        char soundBuf[512];
        strncpy(soundBuf, g_config.pieSpike.soundPath.c_str(), sizeof(soundBuf) - 1);
        soundBuf[sizeof(soundBuf) - 1] = '\0';
        if (ImGui::InputText("Custom sound path (.wav)", soundBuf, sizeof(soundBuf))) {
            g_config.pieSpike.soundPath = soundBuf;
            g_configIsDirty = true;
        }
        ImGui::SameLine();
        HelpMarker("Path to a .wav file for custom alert sound. Leave empty for system beep.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Live Status");

    float currentRatio = g_pieSpikeLastOrangeRatio.load(std::memory_order_relaxed);
    ImGui::Text("Current orange ratio: %.4f", currentRatio);
    ImGui::ProgressBar(currentRatio, ImVec2(-1, 0), "");

    bool alertActive = g_pieSpikeAlertActive.load(std::memory_order_relaxed);
    if (alertActive) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "SPIKE DETECTED");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No spike");
    }

    for (const auto& t : g_config.pieSpike.targets) {
        if (!t.enabled) continue;
        bool inRange = (currentRatio >= t.ratio - t.tolerance && currentRatio <= t.ratio + t.tolerance);
        ImVec4 col = inRange ? ImVec4(1.0f, 0.5f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        ImGui::TextColored(col, "%s: %.3f - %.3f %s", t.name.c_str(),
                           t.ratio - t.tolerance, t.ratio + t.tolerance,
                           inRange ? "(MATCH)" : "");
    }

    if (!g_config.pieSpike.enabled) { ImGui::EndDisabled(); }

    ImGui::EndTabItem();
}
