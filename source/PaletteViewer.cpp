#include "imgui_internal.h"
#include <hex/plugin.hpp>
#include <hex/api/content_registry/views.hpp>
#include <hex/api/imhex_api/provider.hpp>
#include <hex/api/imhex_api/hex_editor.hpp>
#include <hex/helpers/logger.hpp>
#include <hex/helpers/fs.hpp>
#include <hex/ui/view.hpp>
#include <hex/api/events/events_interaction.hpp>
#include <hex/api/events/requests_interaction.hpp>
#include <hex/api/event_manager.hpp>
#include <hex/api/project_file_manager.hpp>
#include <hex/api/events/events_lifecycle.hpp>
#include <nds_plugin_events.hpp>
#include <hex/helpers/tar.hpp>
#include <nlohmann/json.hpp>

#include <imgui.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <algorithm>

namespace {

    struct PaletteViewerState {
        u32 offset    = 0;
        int cols      = 16;
        int cell_size = 24;
        int count     = 256;
        int page_size = 16;
        bool renaming_palette = false;
        std::string renaming_palette_key;
        std::map<std::string, u64> bookmarks;
        std::map<std::string, u32> saved_palettes;
    };
    static PaletteViewerState s_state;

    class ViewPalette : public hex::View::Window {
    public:
        ViewPalette() : hex::View::Window("Palette Viewer", "") {
            m_offset    = s_state.offset;
            m_cols      = s_state.cols;
            m_cell_size = s_state.cell_size;
            m_count     = s_state.count;
            m_page_size = s_state.page_size;
            

            hex::EventManager::subscribe<hex::EventProjectOpened>(this, [this]() {
                m_offset    = s_state.offset;
                m_cols      = s_state.cols;
                m_cell_size = s_state.cell_size;
                m_count     = s_state.count;
                m_page_size = s_state.page_size;
                hex::EventManager::post<hex::EventNDSSavedPalettesChanged>(s_state.saved_palettes);
            });
            hex::EventManager::subscribe<hex::EventPatternEditorChanged>(this, [this](const std::string &code) {
                m_patternCode = code;
            });
        }

        ~ViewPalette() {
            hex::EventManager::unsubscribe<hex::EventProjectOpened>(this);
            hex::EventManager::unsubscribe<hex::EventPatternEditorChanged>(this);
        }

        void drawHelpText() override {}

        void drawContent() override {

            hex::prv::Provider *provider = hex::ImHexApi::Provider::get();
            if (provider == nullptr) {
                ImGui::TextUnformatted("No file open.");
                return;
            }

            ImGui::SetNextItemWidth(160);
            ImGui::InputScalar("Start Offset", ImGuiDataType_U32, &m_offset,
                               nullptr, nullptr, "%08X",
                               ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("Columns", &m_cols);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("Cell Size", &m_cell_size);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("Count", &m_count);
            ImGui::Separator();
            ImGui::SameLine();
            if (ImGui::Button("<<< Page")) {
                m_offset = (m_offset >= m_cols * m_cell_size * m_page_size) ? m_offset - m_cols * m_cell_size * m_page_size : 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("<< Line")) {
                m_offset = (m_offset >= m_cols * 2) ? m_offset - m_cols * 2 : 0;
            }

            ImGui::SameLine();
            if (ImGui::Button("< Color")) {
                m_offset = (m_offset >= 2) ? m_offset - 2 : 0;
            }

            ImGui::SameLine();
            if (ImGui::Button("> Color")) {
                m_offset = (m_offset + 2 < provider->getActualSize()) ? m_offset + 2 : m_offset;
            }
            ImGui::SameLine();
            if (ImGui::Button(">> Line")) {
                m_offset = (m_offset + m_cols * 2 < provider->getActualSize()) ? m_offset + m_cols * 2 : m_offset;
            }
            ImGui::SameLine();
            if (ImGui::Button("Page >>>")) {
                m_offset = (m_offset + m_cols * m_cell_size * m_page_size < provider->getActualSize()) ? m_offset + m_cols * m_cell_size * m_page_size : m_offset;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                m_offset = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("Center on Cursor")) {
                auto selection = hex::ImHexApi::HexEditor::getSelection();
                if (selection.has_value()) {
                    u64 cursor_addr = selection->address;
                    hex::log::info("Center on Cursor: cursor address = 0x{:08X}", cursor_addr);
                    if (cursor_addr < provider->getActualSize()) {
                        m_offset = (cursor_addr >= (m_cols * m_cell_size * 2) / 2) ? cursor_addr - (m_cols * m_cell_size * 2) / 2 : 0;
                    }
                } else {
                    hex::log::warn("Center on Cursor: no selection");
                }
            }
            ImGui::Separator();
            ImGui::Checkbox("Lock to Cursor", &m_locked_to_cursor);
            if (m_locked_to_cursor) {
                auto selection = hex::ImHexApi::HexEditor::getSelection();
                if (selection.has_value()) {
                    u64 cursor_addr = selection->address;
                    hex::log::info("Lock to Cursor: cursor address = 0x{:08X}", cursor_addr);
                    if (cursor_addr < provider->getActualSize()) {
                        m_offset = (cursor_addr >= (m_cols * m_cell_size * 2) / 2) ? cursor_addr - (m_cols * m_cell_size * 2) / 2 : 0;
                    }
                } else {
                    hex::log::warn("Lock to Cursor: no selection");
                }
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("Page Size", &m_page_size);
            if (m_page_size < 1) m_page_size = 1;
            ImGui::SameLine();
            ImGui::Checkbox("Auto-Scroll", &m_auto_scroll);
            if (m_auto_scroll) {
                m_auto_scroll_timer += ImGui::GetIO().DeltaTime;
                if (m_auto_scroll_timer >= 1.0f / m_auto_scroll_speed) {
                    m_auto_scroll_timer = 0.0f;
                    m_offset = (m_offset + m_cols * m_cell_size * 2 < provider->getActualSize()) ? m_offset + m_cols * m_cell_size * 2 : m_offset;
                }
            }
            ImGui::SameLine();

            ImGui::SetNextItemWidth(80);
            ImGui::InputFloat("Scroll Speed (rows/s)", &m_auto_scroll_speed);
            if (m_auto_scroll_speed < 0.1f) m_auto_scroll_speed = 0.1f;

            ImGui::Separator();
            if(ImGui::Button("Mark visible area on hex editor")) {
                u64 addr = m_offset;
                std::string line = fmt::format("\nNdsPalette256 nds_palette_{0:X} @ 0x{0:X};\n", addr);
                hex::EventManager::post<hex::RequestSetPatternLanguageCode>(m_patternCode + line);
                hex::log::info("Added pattern: NdsPalette256 nds_palette_{:X} @ 0x{:X}", addr, addr);
            }

            ImGui::Separator();
            if (ImGui::Button("Export as PNG")) {
                int width  = m_cols * m_cell_size;
                int height = ((m_count + m_cols - 1) / m_cols) * m_cell_size;
                std::vector<unsigned char> image_data(width * height * 3, 0);

                for (int i = 0; i < m_count; i++) {
                    u64 addr = m_offset + static_cast<u64>(i) * 2;
                    if (addr + 2 > provider->getActualSize())
                        break;

                    u16 raw = 0;
                    provider->read(addr, &raw, sizeof(raw));

                    unsigned char r = ((raw >> 0)  & 0x1F) * 8;
                    unsigned char g = ((raw >> 5)  & 0x1F) * 8;
                    unsigned char b = ((raw >> 10) & 0x1F) * 8;

                    int col = i % m_cols;
                    int row = i / m_cols;

                    for (int py = 0; py < m_cell_size; py++) {
                        for (int px = 0; px < m_cell_size; px++) {
                            int idx = ((row * m_cell_size + py) * width + (col * m_cell_size + px)) * 3;
                            image_data[idx + 0] = r;
                            image_data[idx + 1] = g;
                            image_data[idx + 2] = b;
                        }
                    }
                }

                hex::fs::openFileBrowser(
                    hex::fs::DialogMode::Save,
                    {{ "PNG Image", "png" }},
                    [width, height, image_data = std::move(image_data)](const std::fs::path &path) {
                        auto pathStr = path.string();
                        if (stbi_write_png(pathStr.c_str(), width, height, 3, image_data.data(), width * 3))
                            hex::log::info("Palette exported to {}", pathStr);
                        else
                            hex::log::error("Failed to write PNG to {}", pathStr);
                    }
                );
            }

            m_cols      = std::max(1, m_cols);
            m_cell_size = std::max(4, m_cell_size);
            m_count     = std::clamp(m_count, 1, 4096);
                

            int rows = (m_count + m_cols - 1) / m_cols;

            if (ImGui::BeginChild("##palette", ImVec2(-205, 0), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                if (ImGui::IsWindowHovered()) {
                    float wheel = ImGui::GetIO().MouseWheel;
                    if (wheel != 0.0f) {
                        int step = m_cols * 2;
                        if (ImGui::GetIO().KeyCtrl) {
                            step = m_cols * m_cell_size * m_page_size;
                        } else if (ImGui::GetIO().KeyShift) {
                            step = 2;
                        }
                        if (wheel > 0.0f) {
                            m_offset = (m_offset >= static_cast<u32>(step)) ? m_offset - step : 0;
                        } else {
                            u32 next = m_offset + static_cast<u32>(step);
                            m_offset = (next < provider->getActualSize()) ? next : m_offset;
                        }
                    }
                }
                ImDrawList *dl  = ImGui::GetWindowDrawList();
                ImVec2      pos = ImGui::GetCursorScreenPos();

                for (int i = 0; i < m_count; i++) {
                    u64 addr = m_offset + static_cast<u64>(i) * 2;
                    if (addr + 2 > provider->getActualSize())
                        break;

                    u16 raw = 0;
                    provider->read(addr, &raw, sizeof(raw));

                    float r = float((raw >> 0)  & 0x1F) * 8.0f / 255.0f;
                    float g = float((raw >> 5)  & 0x1F) * 8.0f / 255.0f;
                    float b = float((raw >> 10) & 0x1F) * 8.0f / 255.0f;

                    int    col = i % m_cols;
                    int    row = i / m_cols;
                    ImVec2 tl  = { pos.x + col * m_cell_size, pos.y + row * m_cell_size };
                    ImVec2 br  = { tl.x + m_cell_size,        tl.y + m_cell_size        };

                    dl->AddRectFilled(tl, br,
                        ImGui::ColorConvertFloat4ToU32({ r, g, b, 1.0f }));

                    if (ImGui::IsMouseHoveringRect(tl, br)) {
                        ImGui::BeginTooltip();
                        ImGui::Text("Offset: 0x%08llX", static_cast<unsigned long long>(addr));
                        ImGui::Text("Raw:    0x%04X", raw);
                        ImGui::Text("R:%-3d  G:%-3d  B:%-3d",
                            (raw >> 0)  & 0x1F,
                            (raw >> 5)  & 0x1F,
                            (raw >> 10) & 0x1F);
                        ImGui::ColorButton("##swatch",
                            { r, g, b, 1.0f },
                            ImGuiColorEditFlags_NoTooltip, { 40, 40 });
                        ImGui::EndTooltip();

                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            ImGui::SetClipboardText(fmt::format("0x{:08X}", addr).c_str());
                    }
                }

                ImGui::Dummy({ float(m_cols * m_cell_size), float(rows * m_cell_size) });
            }
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("##sidebar", ImVec2(200, 0), true)) {
                // --- Bookmarks ---
                if (ImGui::CollapsingHeader("Bookmarks", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const auto &[name, addr] : s_state.bookmarks) {
                        ImGui::Selectable(fmt::format("{} @ 0x{:08X}", name, addr).c_str());
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            ImGui::SetClipboardText(fmt::format("0x{:08X}", addr).c_str());
                        if (ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
                            m_offset = static_cast<u32>(addr);
                        }
                    }
                    ImGui::Separator();

                    

                    if (ImGui::Button("Add Bookmark##bm", ImVec2(-1, 0)))
                        s_state.bookmarks[fmt::format("Bookmark_{:08X}", m_offset)] = m_offset;
                }
                // --- Saved Palettes ---
                if (ImGui::CollapsingHeader("Saved Palettes", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const auto &[name, off] : s_state.saved_palettes) {
                        ImGui::Selectable(fmt::format("{} @ 0x{:08X}", name, off).c_str());
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            ImGui::SetClipboardText(fmt::format("0x{:08X}", off).c_str());

                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) 
                            m_offset = off;

                        if (ImGui::IsItemActive() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
                            //Rename on double-click
                            s_state.renaming_palette = true;
                            s_state.renaming_palette_key = name;
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::Button("Save Palette##sp", ImVec2(-1, 0))) {
                        s_state.saved_palettes[fmt::format("Palette_{:08X}", m_offset)] = m_offset;
                        hex::EventManager::post<hex::EventNDSSavedPalettesChanged>(s_state.saved_palettes);
                    }
                }
            }
            ImGui::EndChild();

            if (s_state.renaming_palette) {
                ImGui::OpenPopup("Rename Palette");
                s_state.renaming_palette = false;
            }

            ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);
            if (ImGui::BeginPopupModal("Rename Palette", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                static char name_buf[64] = "";
                if (ImGui::IsWindowAppearing())
                    strncpy(name_buf, s_state.renaming_palette_key.c_str(), sizeof(name_buf) - 1);

                ImGui::InputText("New Name", name_buf, sizeof(name_buf));
                if (ImGui::Button("OK", ImVec2(80, 0))) {
                    std::string new_name(name_buf);
                    if (!new_name.empty() && s_state.saved_palettes.count(new_name) == 0) {
                        u32 offset = s_state.saved_palettes[s_state.renaming_palette_key];
                        s_state.saved_palettes.erase(s_state.renaming_palette_key);
                        s_state.saved_palettes[new_name] = offset;
                        hex::EventManager::post<hex::EventNDSSavedPalettesChanged>(s_state.saved_palettes);
                    }
                    s_state.renaming_palette_key = "";
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                    s_state.renaming_palette_key = "";
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            s_state.offset    = m_offset;
            s_state.cols      = m_cols;
            s_state.cell_size = m_cell_size;
            s_state.count     = m_count;
            s_state.page_size = m_page_size;

            if (m_offset != m_last_broadcast_offset) {
                m_last_broadcast_offset = m_offset;
                hex::EventManager::post<hex::EventNDSPaletteOffsetChanged>(m_offset, true);
            }

        }

    private:
        u32 m_offset    = 0x00;
        int m_cols      = 16;
        int m_cell_size = 24;
        int m_count     = 256;
        int m_page_size = 16;
        u32 m_last_broadcast_offset = 0xFFFFFFFF;
        bool m_locked_to_cursor = false;
        std::string m_patternCode;
        bool m_auto_scroll = false;
        float m_auto_scroll_speed = 1.0f; 
        float m_auto_scroll_timer = 0.0f;
    };

}

IMHEX_PLUGIN_SETUP("Palette Viewer", "Badfitz", "NDS BGR555 palette visualizer") {
    hex::ContentRegistry::Views::add<ViewPalette>();

    hex::ProjectFile::registerHandler({
        .basePath = "palette_viewer",
        .required = false,
        .load = [](const std::fs::path &basePath, hex::Tar &tar) -> bool {
            if (!tar.isValid() || !tar.contains(basePath / "settings.json"))
                return true;
            auto j = nlohmann::json::parse(tar.readString(basePath / "settings.json"), nullptr, false);
            if (!j.is_discarded()) {
                s_state.offset    = j.value("offset",    0u);
                s_state.cols      = j.value("cols",      16);
                s_state.cell_size = j.value("cell_size", 24);
                s_state.count     = j.value("count",     256);
                s_state.page_size = j.value("page_size", 16);
                if (j.contains("bookmarks") && j["bookmarks"].is_object())
                    s_state.bookmarks = j["bookmarks"].get<std::map<std::string, u64>>();
                if (j.contains("saved_palettes") && j["saved_palettes"].is_object())
                    s_state.saved_palettes = j["saved_palettes"].get<std::map<std::string, u32>>();
            }
            return true;
        },
        .store = [](const std::fs::path &basePath, hex::Tar &tar) -> bool {
            nlohmann::json j = {
                { "offset",         s_state.offset         },
                { "cols",           s_state.cols           },
                { "cell_size",      s_state.cell_size      },
                { "count",          s_state.count          },
                { "page_size",      s_state.page_size      },
                { "bookmarks",      s_state.bookmarks      },
                { "saved_palettes", s_state.saved_palettes }
            };
            tar.writeString(basePath / "settings.json", j.dump(4));
            return true;
        }
    });
}