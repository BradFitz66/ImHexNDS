#include <hex/plugin.hpp>
#include <hex/api/content_registry/views.hpp>
#include <hex/api/imhex_api/provider.hpp>
#include <hex/ui/view.hpp>
#include <hex/api/event_manager.hpp>
#include <hex/helpers/fmt.hpp>
#include <hex/api/project_file_manager.hpp>
#include <hex/api/events/events_lifecycle.hpp>
#include <hex/helpers/tar.hpp>
#include <nlohmann/json.hpp>
#include <nds_plugin_events.hpp>
#include <imgui.h>
#include <climits>
#include <hex/helpers/fs.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {

    struct TileImageData {
        std::vector<u8> pixels;
        int w = 0, h = 0;
    };

    struct GraphicsViewerState {
        u32  offset         = 0;
        int  tiles_wide     = 16;
        int  zoom           = 2;
        bool show_grid      = false;
        bool use_palette    = false;
        u32  palette_offset = 0;
        bool sync_palette   = false;
        std::map<std::string, u32> saved_palettes;
        std::map<u64, std::string> tile_overrides;
    };
    static GraphicsViewerState s_gfx_state;

    // NDS 4bpp tiled graphics viewer
    // Format: 8x8 tiles, 4bpp little-endian (low nibble = left pixel)
    class ViewGraphics : public hex::View::Window {
    public:
        ViewGraphics() : hex::View::Window("Graphics Viewer", "") {
            m_offset         = s_gfx_state.offset;
            m_tiles_wide     = s_gfx_state.tiles_wide;
            m_zoom           = s_gfx_state.zoom;
            m_show_grid      = s_gfx_state.show_grid;
            m_use_palette    = s_gfx_state.use_palette;
            m_palette_offset = s_gfx_state.palette_offset;
            m_sync_palette   = s_gfx_state.sync_palette;
            m_saved_palettes = s_gfx_state.saved_palettes;
            m_tile_overrides = s_gfx_state.tile_overrides;

            hex::EventManager::subscribe<hex::EventProjectOpened>(this, [this]() {
                m_offset         = s_gfx_state.offset;
                m_tiles_wide     = s_gfx_state.tiles_wide;
                m_zoom           = s_gfx_state.zoom;
                m_show_grid      = s_gfx_state.show_grid;
                m_use_palette    = s_gfx_state.use_palette;
                m_palette_offset = s_gfx_state.palette_offset;
                m_sync_palette   = s_gfx_state.sync_palette;
                m_saved_palettes = s_gfx_state.saved_palettes;
                m_tile_overrides = s_gfx_state.tile_overrides;
            });

            hex::EventManager::subscribe<hex::EventNDSSavedPalettesChanged>(this,
                [this](const std::map<std::string, u32> &palettes) {
                    m_saved_palettes           = palettes;
                    s_gfx_state.saved_palettes = palettes;
                });

            hex::EventManager::subscribe<hex::EventNDSPaletteOffsetChanged>(this,
                [this](u32 offset, bool active) {
                    if (m_sync_palette) {
                        m_use_palette    = active;
                        m_palette_offset = offset;
                    }
                });
        }
        ~ViewGraphics() override {
            hex::EventManager::unsubscribe<hex::EventProjectOpened>(this);
            hex::EventManager::unsubscribe<hex::EventNDSSavedPalettesChanged>(this);
            hex::EventManager::unsubscribe<hex::EventNDSPaletteOffsetChanged>(this);
        }
        void drawHelpText() override {}

        void drawContent() override {
            auto *prov = hex::ImHexApi::Provider::get();
            if (!prov) { ImGui::TextUnformatted("No file open."); return; }

            constexpr int TILE_BYTES = 32; // 8x8 pixels * 4bpp / 8

            // ---- Controls ----
            ImGui::SetNextItemWidth(160);
            ImGui::InputScalar("Offset##gfx", ImGuiDataType_U32, &m_offset,
                nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("Tiles Wide##gfx", &m_tiles_wide);
            m_tiles_wide = std::clamp(m_tiles_wide, 1, 256);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("Zoom##gfx", &m_zoom);
            m_zoom = std::clamp(m_zoom, 1, 8);
            ImGui::SameLine();
            ImGui::Checkbox("Grid##gfx", &m_show_grid);

            ImGui::Separator();

            // ---- Navigation ----
            int row_bytes  = m_tiles_wide * TILE_BYTES;
            int page_bytes = row_bytes * 16;

            if (ImGui::Button("<<< Page"))
                m_offset = (m_offset >= (u32)page_bytes) ? m_offset - page_bytes : 0;
            ImGui::SameLine();
            if (ImGui::Button("<< Row"))
                m_offset = (m_offset >= (u32)row_bytes) ? m_offset - row_bytes : 0;
            ImGui::SameLine();
            if (ImGui::Button("< Tile"))
                m_offset = (m_offset >= (u32)TILE_BYTES) ? m_offset - TILE_BYTES : 0;
            ImGui::SameLine();
            if (ImGui::Button("< Byte"))
                m_offset = (m_offset > 0) ? m_offset - 1 : 0;
            ImGui::SameLine();
            if (ImGui::Button("> Byte")) {
                if (m_offset + 1 < prov->getActualSize()) m_offset++;
            }
            ImGui::SameLine();
            if (ImGui::Button("> Tile")) {
                u32 next = m_offset + TILE_BYTES;
                if (next < prov->getActualSize()) m_offset = next;
            }
            ImGui::SameLine();
            if (ImGui::Button(">> Row")) {
                u32 next = m_offset + row_bytes;
                if (next < prov->getActualSize()) m_offset = next;
            }
            ImGui::SameLine();
            if (ImGui::Button(">>> Page")) {
                u32 next = m_offset + page_bytes;
                if (next < prov->getActualSize()) m_offset = next;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset##gfx")) m_offset = 0;

            ImGui::Separator();

            // ---- Palette ----
            ImGui::Checkbox("BGR555 palette at offset##gfx", &m_use_palette);
            if (m_use_palette) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160);
                ImGui::InputScalar("##paloff", ImGuiDataType_U32, &m_palette_offset,
                    nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
                ImGui::SameLine();
                ImGui::TextUnformatted("Palette offset");
            }
            ImGui::SameLine();
            ImGui::Checkbox("Sync from Palette Viewer##gfx", &m_sync_palette);
            if (m_sync_palette)
                ImGui::TextDisabled("  (palette offset controlled by Palette Viewer)");

            ImGui::Separator();

            auto default_palette = buildPalette(prov);
            int  tile_px = 8 * m_zoom; 
            int  canvas_w = m_tiles_wide * tile_px;

            u64 file_size   = prov->getActualSize();
            int avail_tiles = (m_offset < file_size)
                ? (int)((file_size - m_offset) / TILE_BYTES) : 0;
            int total_rows  = (avail_tiles + m_tiles_wide - 1) / m_tiles_wide;
            int content_h   = total_rows * tile_px;

            ImGui::SetNextWindowContentSize(ImVec2((float)canvas_w, (float)content_h));
            if (ImGui::BeginChild("##gfxcanvas", ImVec2(-205, 0), false,
                    ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {

                if (ImGui::IsWindowHovered()) {
                    float wheel = ImGui::GetIO().MouseWheel;
                    if (wheel != 0.0f) {
                        int step = row_bytes;
                        if (ImGui::GetIO().KeyCtrl)  step = page_bytes;
                        if (ImGui::GetIO().KeyShift) step = TILE_BYTES;
                        if (wheel > 0.0f)
                            m_offset = (m_offset >= (u32)step) ? m_offset - step : 0;
                        else {
                            u32 next = m_offset + (u32)step;
                            if (next < file_size) m_offset = next;
                        }
                    }
                }

                ImDrawList *dl       = ImGui::GetWindowDrawList();
                ImVec2      origin   = ImGui::GetCursorScreenPos();
                float       scroll_y = ImGui::GetScrollY();
                int         first_row = (int)(scroll_y / tile_px);
                int         vis_rows  = (int)(ImGui::GetWindowHeight() / tile_px) + 2;
                int         last_row  = std::min(first_row + vis_rows, total_rows);

                // Drag-select: update state from mouse position
                {
                    ImVec2 mouse    = ImGui::GetMousePos();
                    int mouse_col   = std::clamp((int)((mouse.x - origin.x) / tile_px), 0, m_tiles_wide - 1);
                    int mouse_row   = std::max(0, (int)((mouse.y - origin.y) / tile_px));

                    if (ImGui::IsWindowHovered() &&
                            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        m_drag_selecting  = true;
                        m_drag_add_to_sel = ImGui::GetIO().KeyCtrl;
                        m_drag_col0 = m_drag_col1 = mouse_col;
                        m_drag_row0 = m_drag_row1 = mouse_row;
                    }
                    if (m_drag_selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        m_drag_col1 = mouse_col;
                        m_drag_row1 = mouse_row;
                    }
                    if (m_drag_selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                        int c0 = std::min(m_drag_col0, m_drag_col1), c1 = std::max(m_drag_col0, m_drag_col1);
                        int r0 = std::min(m_drag_row0, m_drag_row1), r1 = std::max(m_drag_row0, m_drag_row1);
                        bool is_single = (c0 == c1 && r0 == r1);
                        if (!m_drag_add_to_sel)
                            m_selected_tiles.clear();
                        for (int r = r0; r <= r1; r++)
                            for (int c = c0; c <= c1; c++) {
                                int idx = r * m_tiles_wide + c;
                                if (idx < avail_tiles) {
                                    u64 addr = m_offset + (u64)idx * TILE_BYTES;
                                    if (m_drag_add_to_sel && is_single) {
                                        if (m_selected_tiles.count(addr)) m_selected_tiles.erase(addr);
                                        else                               m_selected_tiles.insert(addr);
                                    } else {
                                        m_selected_tiles.insert(addr);
                                    }
                                }
                            }
                        m_drag_selecting = false;
                    }
                }

                bool out_of_data = false;
                for (int row = first_row; row < last_row && !out_of_data; row++) {
                    for (int col = 0; col < m_tiles_wide && !out_of_data; col++) {
                        int tile_idx = row * m_tiles_wide + col;
                        if (tile_idx >= avail_tiles) { out_of_data = true; break; }

                        u64 tile_addr = m_offset + (u64)tile_idx * TILE_BYTES;
                        u8  tile_data[32] = {};
                        u64 to_read = std::min((u64)TILE_BYTES, file_size - tile_addr);
                        prov->read(tile_addr, tile_data, (size_t)to_read);

                        // Per-tile palette override
                        auto tile_palette = default_palette;
                        {
                            auto oit = m_tile_overrides.find(tile_addr);
                            if (oit != m_tile_overrides.end()) {
                                auto pit = m_saved_palettes.find(oit->second);
                                if (pit != m_saved_palettes.end())
                                    tile_palette = buildPaletteAtOffset(prov, pit->second);
                            }
                        }

                        float tx = origin.x + col * tile_px;
                        float ty = origin.y + row * tile_px;

                        // Draw 8x8 pixels
                        for (int py = 0; py < 8; py++) {
                            for (int px = 0; px < 8; px++) {
                                int   i      = py * 8 + px;
                                int   nibble = (tile_data[i >> 1] >> ((i & 1) << 2)) & 0xF;
                                float x0 = tx + px * m_zoom;
                                float y0 = ty + py * m_zoom;
                                dl->AddRectFilled({x0, y0},
                                    {x0 + m_zoom, y0 + m_zoom}, tile_palette[nibble]);
                            }
                        }

                        // Tile grid overlay
                        if (m_show_grid)
                            dl->AddRect({tx, ty}, {tx + tile_px, ty + tile_px},
                                IM_COL32(100, 100, 100, 160));

                        // Selection / override border
                        bool is_selected  = m_selected_tiles.count(tile_addr) > 0;
                        bool has_override = m_tile_overrides.count(tile_addr) > 0;
                        if (is_selected)
                            dl->AddRect({tx, ty}, {tx + tile_px, ty + tile_px},
                                IM_COL32(255, 220, 0, 255), 0, 0, 2.0f);
                        else if (has_override)
                            dl->AddRect({tx, ty}, {tx + tile_px, ty + tile_px},
                                IM_COL32(0, 200, 255, 180), 0, 0, 1.0f);

                        // Hover: tooltip + click interactions
                        if (ImGui::IsMouseHoveringRect({tx, ty},
                                {tx + (float)tile_px, ty + (float)tile_px})) {
                            ImGui::BeginTooltip();
                            ImGui::Text("Tile:   #%d", tile_idx);
                            ImGui::Text("Offset: 0x%08X", (u32)tile_addr);
                            if (has_override)
                                ImGui::Text("Palette: %s", m_tile_overrides.at(tile_addr).c_str());
                            ImGui::EndTooltip();

                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                                ImGui::SetClipboardText(fmt::format("0x{:08X}", (u32)tile_addr).c_str());
                        }
                    }
                }

                // Draw drag-select rectangle overlay
                if (m_drag_selecting) {
                    int c0 = std::min(m_drag_col0, m_drag_col1), c1 = std::max(m_drag_col0, m_drag_col1);
                    int r0 = std::min(m_drag_row0, m_drag_row1), r1 = std::max(m_drag_row0, m_drag_row1);
                    float sx = origin.x + c0 * tile_px;
                    float sy = origin.y + r0 * tile_px;
                    float ex = origin.x + (c1 + 1) * tile_px;
                    float ey = origin.y + (r1 + 1) * tile_px;
                    ImU32 drag_fill = m_drag_add_to_sel ? IM_COL32(100, 200, 255,  30) : IM_COL32(255, 220,   0,  40);
                    ImU32 drag_bord = m_drag_add_to_sel ? IM_COL32(100, 200, 255, 200) : IM_COL32(255, 220,   0, 200);
                    dl->AddRectFilled({sx, sy}, {ex, ey}, drag_fill);
                    dl->AddRect({sx, sy}, {ex, ey}, drag_bord, 0, 0, 1.5f);
                }

                ImGui::Dummy({(float)canvas_w, (float)content_h});
            }
            ImGui::EndChild();
            ImGui::SameLine();

            // ---- Palette sidebar ----
            if (ImGui::BeginChild("##gfxsidebar", ImVec2(200, 0), true)) {
                ImGui::TextUnformatted("Saved Palettes");
                ImGui::Separator();
                if (m_saved_palettes.empty()) {
                    ImGui::TextDisabled("None (save in Palette Viewer)");
                } else {
                    for (const auto &[name, off] : m_saved_palettes) {
                        bool sel = (m_selected_palette == name);
                        if (ImGui::Selectable(
                                fmt::format("{}\n0x{:08X}", name, off).c_str(), sel))
                            m_selected_palette = name;
                    }
                }
                ImGui::Separator();
                ImGui::BeginDisabled(m_selected_palette.empty() || m_selected_tiles.empty());
                if (ImGui::Button("Assign to Selection", ImVec2(-1, 0))) {
                    for (u64 addr : m_selected_tiles)
                        m_tile_overrides[addr] = m_selected_palette;
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(m_selected_tiles.empty());
                if (ImGui::Button("Clear Selected", ImVec2(-1, 0))) {
                    for (u64 addr : m_selected_tiles)
                        m_tile_overrides.erase(addr);
                }
                ImGui::EndDisabled();
                ImGui::Separator();
                if (ImGui::Button("Clear All Overrides", ImVec2(-1, 0)))
                    m_tile_overrides.clear();
                ImGui::Separator();
                ImGui::TextDisabled("%zu selected", m_selected_tiles.size());
                ImGui::TextDisabled("%zu overridden", m_tile_overrides.size());
                ImGui::Separator();
                if (ImGui::Button("Export All Tiles", ImVec2(-1, 0))) {
                    if (avail_tiles > 0) {
                        std::vector<u64> addrs;
                        addrs.reserve(avail_tiles);
                        for (int i = 0; i < avail_tiles; i++)
                            addrs.push_back(m_offset + (u64)i * TILE_BYTES);
                        auto img = renderTilesToImage(prov, addrs, m_tiles_wide);
                        hex::fs::openFileBrowser(
                            hex::fs::DialogMode::Save, {{ "PNG Image", "png" }},
                            [img = std::move(img)](const std::fs::path &path) {
                                auto s = path.string();
                                if (img.w > 0)
                                    stbi_write_png(s.c_str(), img.w, img.h, 4, img.pixels.data(), img.w * 4);
                            });
                    }
                }
                ImGui::BeginDisabled(m_selected_tiles.empty());
                if (ImGui::Button("Export Selected", ImVec2(-1, 0))) {
                    // Render the full tile grid then crop to the selection's bounding box
                    if (avail_tiles > 0) {
                        std::vector<u64> all_addrs;
                        all_addrs.reserve(avail_tiles);
                        for (int i = 0; i < avail_tiles; i++)
                            all_addrs.push_back(m_offset + (u64)i * TILE_BYTES);
                        auto full_img = renderTilesToImage(prov, all_addrs, m_tiles_wide);

                        int min_col = INT_MAX, max_col = INT_MIN;
                        int min_row = INT_MAX, max_row = INT_MIN;
                        for (u64 addr : m_selected_tiles) {
                            if (addr < m_offset) continue;
                            int idx = (int)((addr - m_offset) / TILE_BYTES);
                            if (idx >= avail_tiles) continue;
                            int col = idx % m_tiles_wide, row = idx / m_tiles_wide;
                            min_col = std::min(min_col, col); max_col = std::max(max_col, col);
                            min_row = std::min(min_row, row); max_row = std::max(max_row, row);
                        }

                        if (max_col >= 0) {
                            int px0 = min_col * 8, py0 = min_row * 8;
                            int cw  = (max_col - min_col + 1) * 8;
                            int ch  = (max_row - min_row + 1) * 8;
                            TileImageData crop;
                            crop.w = cw; crop.h = ch;
                            crop.pixels.resize((size_t)cw * ch * 4);
                            for (int y = 0; y < ch; y++)
                                memcpy(crop.pixels.data() + (size_t)y * cw * 4,
                                       full_img.pixels.data() + ((size_t)(py0 + y) * full_img.w + px0) * 4,
                                       (size_t)cw * 4);
                            hex::fs::openFileBrowser(
                                hex::fs::DialogMode::Save, {{ "PNG Image", "png" }},
                                [crop = std::move(crop)](const std::fs::path &path) {
                                    auto s = path.string();
                                    stbi_write_png(s.c_str(), crop.w, crop.h, 4, crop.pixels.data(), crop.w * 4);
                                });
                        }
                    }
                }
                ImGui::EndDisabled();
            }
            ImGui::EndChild();

            // Keep shared state in sync so the project handler can save it
            s_gfx_state.offset         = m_offset;
            s_gfx_state.tiles_wide     = m_tiles_wide;
            s_gfx_state.zoom           = m_zoom;
            s_gfx_state.show_grid      = m_show_grid;
            s_gfx_state.use_palette    = m_use_palette;
            s_gfx_state.palette_offset = m_palette_offset;
            s_gfx_state.sync_palette   = m_sync_palette;
            s_gfx_state.saved_palettes = m_saved_palettes;
            s_gfx_state.tile_overrides = m_tile_overrides;
        }

    private:
        u32  m_offset         = 0;
        int  m_tiles_wide     = 16;
        int  m_zoom           = 2;
        bool m_show_grid      = false;
        bool m_use_palette    = false;
        u32  m_palette_offset = 0;
        bool m_sync_palette   = false;
        std::map<std::string, u32> m_saved_palettes;
        std::map<u64, std::string> m_tile_overrides;
        std::set<u64>              m_selected_tiles;
        std::string                m_selected_palette;
        bool                       m_drag_selecting  = false;
        bool                       m_drag_add_to_sel = false;
        int                        m_drag_col0 = 0, m_drag_row0 = 0;
        int                        m_drag_col1 = 0, m_drag_row1 = 0;

        TileImageData renderTilesToImage(hex::prv::Provider *prov,
                                          const std::vector<u64> &addrs,
                                          int tiles_wide) const {
            constexpr int TB = 32;
            int count = (int)addrs.size();
            if (count == 0) return {};
            int cols  = std::min(tiles_wide, count);
            int rows  = (count + cols - 1) / cols;
            TileImageData img;
            img.w = cols * 8;
            img.h = rows * 8;
            img.pixels.resize((size_t)img.w * img.h * 4, 0);
            u64 file_size = prov->getActualSize();
            auto def_pal  = buildPalette(prov);
            for (int t = 0; t < count; t++) {
                u64 tile_addr = addrs[t];
                auto pal = def_pal;
                auto oit = m_tile_overrides.find(tile_addr);
                if (oit != m_tile_overrides.end()) {
                    auto pit = m_saved_palettes.find(oit->second);
                    if (pit != m_saved_palettes.end())
                        pal = buildPaletteAtOffset(prov, pit->second);
                }
                u8 tile_data[TB] = {};
                if (tile_addr < file_size) {
                    u64 to_read = std::min((u64)TB, file_size - tile_addr);
                    prov->read(tile_addr, tile_data, (size_t)to_read);
                }
                int bx = (t % cols) * 8, by = (t / cols) * 8;
                for (int py = 0; py < 8; py++) {
                    for (int px = 0; px < 8; px++) {
                        int   i      = py * 8 + px;
                        int   nibble = (tile_data[i >> 1] >> ((i & 1) << 2)) & 0xF;
                        ImU32 color  = pal[nibble];
                        int   idx    = ((by + py) * img.w + (bx + px)) * 4;
                        img.pixels[idx + 0] = (color >>  0) & 0xFF;
                        img.pixels[idx + 1] = (color >>  8) & 0xFF;
                        img.pixels[idx + 2] = (color >> 16) & 0xFF;
                        img.pixels[idx + 3] = 0xFF;
                    }
                }
            }
            return img;
        }

        std::array<ImU32, 16> buildPaletteAtOffset(hex::prv::Provider *prov, u32 offset) const {
            std::array<ImU32, 16> pal = {};
            for (int i = 0; i < 16; i++) {
                u64 addr = offset + (u64)i * 2;
                u16 raw  = 0;
                if (addr + 2 <= prov->getActualSize())
                    prov->read(addr, &raw, 2);
                pal[i] = IM_COL32(
                    ((raw >>  0) & 0x1F) * 8,
                    ((raw >>  5) & 0x1F) * 8,
                    ((raw >> 10) & 0x1F) * 8,
                    255);
            }
            return pal;
        }

        std::array<ImU32, 16> buildPalette(hex::prv::Provider *prov) const {
            if (m_use_palette)
                return buildPaletteAtOffset(prov, m_palette_offset);
            std::array<ImU32, 16> pal = {};
            for (int i = 0; i < 16; i++) {
                u8 v = static_cast<u8>(i * 255 / 15);
                pal[i] = IM_COL32(v, v, v, 255);
            }
            return pal;
        }
    };

}

IMHEX_PLUGIN_SETUP("Graphics Viewer", "Badfitz", "NDS 4bpp tiled graphics viewer") {
    hex::ContentRegistry::Views::add<ViewGraphics>();

    hex::ProjectFile::registerHandler({
        .basePath = "graphics_viewer",
        .required = false,
        .load = [](const std::fs::path &basePath, hex::Tar &tar) -> bool {
            if (!tar.isValid() || !tar.contains(basePath / "settings.json"))
                return true;
            auto j = nlohmann::json::parse(tar.readString(basePath / "settings.json"), nullptr, false);
            if (!j.is_discarded()) {
                s_gfx_state.offset         = j.value("offset",         0u);
                s_gfx_state.tiles_wide     = j.value("tiles_wide",     16);
                s_gfx_state.zoom           = j.value("zoom",           2);
                s_gfx_state.show_grid      = j.value("show_grid",      false);
                s_gfx_state.use_palette    = j.value("use_palette",    false);
                s_gfx_state.palette_offset = j.value("palette_offset", 0u);
                s_gfx_state.sync_palette   = j.value("sync_palette",   false);
                if (j.contains("saved_palettes") && j["saved_palettes"].is_object())
                    s_gfx_state.saved_palettes = j["saved_palettes"].get<std::map<std::string, u32>>();
                if (j.contains("tile_overrides") && j["tile_overrides"].is_object()) {
                    s_gfx_state.tile_overrides.clear();
                    for (const auto &[key, val] : j["tile_overrides"].items()) {
                        try { s_gfx_state.tile_overrides[std::stoull(key, nullptr, 0)] = val.get<std::string>(); }
                        catch (...) {}
                    }
                }
            }
            return true;
        },
        .store = [](const std::fs::path &basePath, hex::Tar &tar) -> bool {
            nlohmann::json overrides = nlohmann::json::object();
            for (const auto &[addr, name] : s_gfx_state.tile_overrides)
                overrides[fmt::format("0x{:X}", addr)] = name;
            nlohmann::json j = {
                { "offset",         s_gfx_state.offset         },
                { "tiles_wide",     s_gfx_state.tiles_wide     },
                { "zoom",           s_gfx_state.zoom           },
                { "show_grid",      s_gfx_state.show_grid      },
                { "use_palette",    s_gfx_state.use_palette    },
                { "palette_offset", s_gfx_state.palette_offset },
                { "sync_palette",   s_gfx_state.sync_palette   },
                { "saved_palettes", s_gfx_state.saved_palettes },
                { "tile_overrides", overrides                  }
            };
            tar.writeString(basePath / "settings.json", j.dump(4));
            return true;
        }
    });
}
