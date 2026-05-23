#include <hex/plugin.hpp>
#include <hex/api/content_registry/views.hpp>
#include <hex/api/imhex_api/provider.hpp>
#include <hex/api/imhex_api/hex_editor.hpp>
#include <hex/helpers/logger.hpp>
#include <hex/helpers/fs.hpp>
#include <hex/ui/view.hpp>
#include <imgui.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <algorithm>

namespace {

    class ViewPalette : public hex::View::Window {
    public:
        ViewPalette() : hex::View::Window("Palette Viewer", "") {}

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
\
            ImGui::SetNextItemWidth(80);
            ImGui::InputFloat("Scroll Speed (rows/s)", &m_auto_scroll_speed);
            if (m_auto_scroll_speed < 0.1f) m_auto_scroll_speed = 0.1f;



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

            if (ImGui::BeginChild("##palette", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                if (ImGui::IsWindowHovered()) {
                    float wheel = ImGui::GetIO().MouseWheel;
                    if (wheel != 0.0f) {
                        int step = ImGui::GetIO().KeyCtrl ? m_page_size * m_cols : m_cols;
                        if (wheel > 0.0f) {
                            m_offset = (m_offset >= static_cast<u32>(step * 2)) ? m_offset - step * 2 : 0;
                        } else {
                            u32 next = m_offset + static_cast<u32>(step * 2);
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
                    }
                }

                ImGui::Dummy({ float(m_cols * m_cell_size), float(rows * m_cell_size) });
            }
            ImGui::EndChild();


        }

    private:
        u32 m_offset    = 0x00;
        int m_cols      = 16;
        int m_cell_size = 24;
        int m_count     = 256;
        int m_page_size = 16;
        bool m_locked_to_cursor = false;
        bool m_auto_scroll = false;
        float m_auto_scroll_speed = 1.0f; 
        float m_auto_scroll_timer = 0.0f;
    };

}

IMHEX_PLUGIN_SETUP("Palette Viewer", "Badfitz", "NDS BGR555 palette visualizer") {
    hex::ContentRegistry::Views::add<ViewPalette>();
}