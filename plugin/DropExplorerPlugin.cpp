#include "DropExplorerPlugin.h"

#include <GWCA/Constants/Maps.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/ItemMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/PlayerMgr.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Item.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/GameEntities/Party.h>
#include <GWCA/Constants/Constants.h>
#include <GWCA/Constants/UIMessages.h>
#include <Defines.h>
#include <Utils/GuiUtils.h>
#include <Utils/TextUtils.h>
#include <Utils/ToolboxUtils.h>

#include <glaze/glaze.hpp>
#include <easywsclient.hpp>
#include <imgui.h>
#include <fstream>
#include <algorithm>
#include <shellapi.h>
#include <limits>
#include <random>
#include <ctime>
#include <format>
#include <cmath>

namespace TradeChatApi {
    struct TradeChatRawMessage {
        std::string s;
        std::string m;
        glz::raw_json t;
    };

    struct TradeChatEnvelope {
        std::string query;
        std::vector<TradeChatRawMessage> results;
        std::string s;
        std::string m;
        glz::raw_json t;
    };
}

namespace {
    uint64_t ParseTradeChatTimestamp(std::string_view raw)
    {
        if (raw.empty()) return 0;
        if (raw.front() == '"') {
            std::string value;
            if (glz::read_json(value, raw)) return 0;
            return strtoull(value.c_str(), nullptr, 10) / 1000;
        }
        double value = 0.0;
        if (glz::read_json(value, raw)) return 0;
        return static_cast<uint64_t>(value) / 1000;
    }

    std::string FormatTimestamp(uint64_t ts);

    std::string ToLower(std::string_view s)
    {
        std::string res;
        res.reserve(s.size());
        for (const char c : s) {
            res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return res;
    }

    std::string TrimText(std::string_view text, std::string_view characters = " \t\r\n")
    {
        const auto first = text.find_first_not_of(characters);
        if (first == std::string_view::npos) return {};
        const auto last = text.find_last_not_of(characters);
        return std::string(text.substr(first, last - first + 1));
    }

    std::wstring ToWString(std::string_view s)
    {
        if (s.empty()) return L"";
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring ws(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), ws.data(), len);
        return ws;
    }

    bool CaseInsensitiveContains(std::string_view str, std::string_view sub)
    {
        if (sub.empty()) return true;
        return ToLower(str).find(ToLower(sub)) != std::string::npos;
    }

    std::string StripXmlTags(std::string_view input)
    {
        std::string output;
        output.reserve(input.size());
        bool inside_tag = false;
        for (const auto c : input) {
            if (c == '<') inside_tag = true;
            else if (c == '>') inside_tag = false;
            else if (!inside_tag) output.push_back(c);
        }
        return output;
    }

    DropExplorerPlugin::LinkerRarity GetLinkerItemRarity(const GW::Item* item)
    {
        if (!item) return DropExplorerPlugin::LinkerRarity::White;
        if ((item->interaction & 0x10) != 0) return DropExplorerPlugin::LinkerRarity::Green;
        if ((item->interaction & 0x400000) != 0) return DropExplorerPlugin::LinkerRarity::Purple;
        if ((item->interaction & 0x20000) != 0) return DropExplorerPlugin::LinkerRarity::Gold;
        if (item->single_item_name && item->single_item_name[0] == 0xA3F) return DropExplorerPlugin::LinkerRarity::Blue;
        return DropExplorerPlugin::LinkerRarity::White;
    }

    ImVec4 GetLinkerRarityColor(const DropExplorerPlugin::LinkerRarity rarity)
    {
        switch (rarity) {
            case DropExplorerPlugin::LinkerRarity::Blue: return ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
            case DropExplorerPlugin::LinkerRarity::Purple: return ImVec4(0.75f, 0.5f, 0.95f, 1.0f);
            case DropExplorerPlugin::LinkerRarity::Gold: return ImVec4(1.0f, 0.85f, 0.35f, 1.0f);
            case DropExplorerPlugin::LinkerRarity::Green: return ImVec4(0.35f, 1.0f, 0.4f, 1.0f);
            default: return ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
        }
    }

    const char* GetLinkerRarityName(const DropExplorerPlugin::LinkerRarity rarity)
    {
        switch (rarity) {
            case DropExplorerPlugin::LinkerRarity::Blue: return "Magical (Blue)";
            case DropExplorerPlugin::LinkerRarity::Purple: return "Rare (Purple)";
            case DropExplorerPlugin::LinkerRarity::Gold: return "Elite (Gold)";
            case DropExplorerPlugin::LinkerRarity::Green: return "Unique (Green)";
            default: return "Common (White)";
        }
    }

    std::string CleanRawText(const std::string& input)
    {
        std::string out;
        out.reserve(input.size());
        bool in_tag = false;
        for (const char c : input) {
            if (c == '<') { in_tag = true; continue; }
            if (c == '>') { in_tag = false; continue; }
            if (!in_tag) out.push_back(c);
        }
        return out;
    }

    char GetChannelChar(const int channel_idx)
    {
        switch (channel_idx) {
            case 0: return '#';
            case 1: return '@';
            case 2: return '$';
            case 3: return '!';
            case 4: return '%';
            default: return '#';
        }
    }

    ImVec4 GetRarityColor(DropExplorer::DropRarity rarity)
    {
        switch (rarity) {
            case DropExplorer::DropRarity::Common:
                return {0.85f, 0.85f, 0.85f, 1.0f};
            case DropExplorer::DropRarity::Uncommon:
                return {0.35f, 0.65f, 1.0f, 1.0f};
            case DropExplorer::DropRarity::Rare:
                return {1.0f, 0.84f, 0.0f, 1.0f};
            case DropExplorer::DropRarity::UniqueGreen:
                return {0.2f, 1.0f, 0.3f, 1.0f};
            case DropExplorer::DropRarity::Exotic:
                return {1.0f, 0.45f, 0.85f, 1.0f};
            default:
                return {1.0f, 1.0f, 1.0f, 1.0f};
        }
    }

    void NormalizeUnverifiedData(std::vector<DropExplorer::ZoneInfo>& zones)
    {
        for (auto& zone : zones) {
            for (auto& mob : zone.mobs) {
                for (auto& drop : mob.drops) {
                    drop.est_chance_pct = -1.0f;
                    drop.chance_str = "Unknown";
                    if (drop.stats.starts_with("Max damage/armor (req. 9)")) {
                        drop.stats.clear();
                    }
                }
            }
        }
    }

    uint32_t GetItemRarity(const GW::Item* item)
    {
        if (!item) return 0;
        if ((item->interaction & 0x10) != 0) return 4;
        if ((item->interaction & 0x400000) != 0) return 2;
        if ((item->interaction & 0x20000) != 0) return 3;
        if (item->single_item_name && item->single_item_name[0] == 0xA3F) return 1;
        return 0;
    }

    std::string GenerateAnonymousId()
    {
        std::random_device random;
        const auto a = (static_cast<uint64_t>(random()) << 32) | random();
        const auto b = (static_cast<uint64_t>(random()) << 32) | random();
        return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
                           static_cast<uint32_t>(a >> 32), static_cast<uint16_t>(a >> 16),
                           static_cast<uint16_t>(a), static_cast<uint16_t>(b >> 48), b & 0xffffffffffffULL);
    }
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static DropExplorerPlugin instance;
    return &instance;
}

void DropExplorerPlugin::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    InitAsyncRest();
    telemetry_client_ = std::make_unique<AsyncRestClient>();
    rates_client_ = std::make_unique<AsyncRestClient>();
    vendor_prices_client_ = std::make_unique<AsyncRestClient>();
    auction_client_ = std::make_unique<AsyncRestClient>();
    messages_client_ = std::make_unique<AsyncRestClient>();
    StartTradeChatFeed();

    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::QuotedItemPrice>(&price_quote_entry_, [this](GW::HookStatus*, const GW::Packet::StoC::QuotedItemPrice* packet) {
        OnPriceQuote(packet);
    });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::TransactionDone>(&trans_done_entry_, [this](GW::HookStatus*, const GW::Packet::StoC::TransactionDone* packet) {
        OnTransactionDone(packet);
    });
    if (toolbox_dll) {
        get_item_image_fn_ = reinterpret_cast<GetItemImageByName_pt>(GetProcAddress(toolbox_dll, "GetItemImageByName"));
        set_travel_dest_fn_ = reinterpret_cast<SetTravelDestinationMapId_pt>(GetProcAddress(toolbox_dll, "SetTravelDestinationMapId"));
        set_custom_point_fn_ = reinterpret_cast<SetCustomTravelPointGame_pt>(GetProcAddress(toolbox_dll, "SetCustomTravelPointGame"));
        clear_custom_point_fn_ = reinterpret_cast<ClearCustomTravelPoint_pt>(GetProcAddress(toolbox_dll, "ClearCustomTravelPoint"));
        inventory_add_context_menu_fn_ = reinterpret_cast<InventoryAddContextMenuCallback_pt>(GetProcAddress(toolbox_dll, "InventoryAddContextMenuCallback"));
        inventory_remove_context_menu_fn_ = reinterpret_cast<InventoryRemoveContextMenuCallback_pt>(GetProcAddress(toolbox_dll, "InventoryRemoveContextMenuCallback"));
        if (inventory_add_context_menu_fn_) inventory_add_context_menu_fn_(DrawInventoryContextMenuEntry);
    }

    const auto on_item_click = [](GW::HookStatus* status, GW::UI::UIPacket::kMouseAction* action, GW::Item* gw_item) {
        if (!action || !gw_item || !gw_item->item_id) return;
        auto* plugin = static_cast<DropExplorerPlugin*>(ToolboxPluginInstance());
        if (!plugin) return;

        const auto is_right = (static_cast<uint32_t>(action->current_state) == 999u);
        const auto is_click = (action->current_state == GW::UI::UIPacket::ActionState::MouseClick ||
                               action->current_state == GW::UI::UIPacket::ActionState::MouseUp);

        if (plugin->linker_waiting_for_item_ && (is_right || is_click)) {
            plugin->linker_waiting_for_item_ = false;
            plugin->SetLinkerFocusedItem(gw_item);
            plugin->linker_focus_requested_ = true;
            plugin->current_tab_ = 1;
            if (status) status->blocked = true;
            return;
        }

        if (plugin->auction_waiting_for_item_ && (is_right || is_click)) {
            plugin->auction_waiting_for_item_ = false;
            plugin->PrefillAuctionItem(gw_item);
            plugin->auction_focus_requested_ = true;
            plugin->current_tab_ = 0;
            if (status) status->blocked = true;
            return;
        }
    };
    GW::Items::RegisterItemClickCallback(&item_click_entry_, on_item_click);

    zones_ = DropExplorer::GetBuiltinZones();
    NormalizeUnverifiedData(zones_);
    BuildItemIndex();
}

void DropExplorerPlugin::Terminate()
{
    trade_chat_enabled_ = false;
    trade_chat_thread_.request_stop();
    if (trade_chat_thread_.joinable()) trade_chat_thread_.join();
    GW::Items::RemoveItemClickCallback(&item_click_entry_);
    if (discovery_client_) discovery_client_->Abort();
    discovery_client_.reset();
    if (inventory_remove_context_menu_fn_) inventory_remove_context_menu_fn_(DrawInventoryContextMenuEntry);
    if (linker_decode_state_) {
        linker_decode_state_->cancelled = true;
        linker_decode_state_.reset();
    }
    GW::StoC::RemoveCallback<GW::Packet::StoC::QuotedItemPrice>(&price_quote_entry_);
    GW::StoC::RemoveCallback<GW::Packet::StoC::TransactionDone>(&trans_done_entry_);
    if (telemetry_client_ && telemetry_client_->IsPending()) telemetry_client_->Abort();
    if (rates_client_ && rates_client_->IsPending()) rates_client_->Abort();
    if (vendor_prices_client_ && vendor_prices_client_->IsPending()) vendor_prices_client_->Abort();
    if (auction_client_ && auction_client_->IsPending()) auction_client_->Abort();
    if (messages_client_ && messages_client_->IsPending()) messages_client_->Abort();
    telemetry_client_.reset();
    rates_client_.reset();
    vendor_prices_client_.reset();
    auction_client_.reset();
    messages_client_.reset();
    ShutdownAsyncRest();
    agent_names_cache_.clear();
    ToolboxUIPlugin::Terminate();
}

std::string DropExplorerPlugin::GetResolvedAgentName(const uint32_t agent_id, const GW::Agent* agent)
{
    if (!agent_id) return {};
    const auto* encoded_name = agent ? GW::Agents::GetAgentEncName(agent) : GW::Agents::GetAgentEncName(agent_id);
    if (!encoded_name || !*encoded_name) return {};
    auto it = agent_names_cache_.find(agent_id);
    if (it == agent_names_cache_.end()) {
        it = agent_names_cache_.emplace(agent_id, std::make_unique<PluginUtils::EncString>(encoded_name, true)).first;
    } else {
        it->second->reset(encoded_name);
    }
    return StripXmlTags(it->second->string());
}

void DropExplorerPlugin::Update(const float delta)
{
    trade_chat_enabled_ = show_auction_house_;
    UpdateTradeChatListings();
    UpdateServiceDiscovery();
    if (!crowdsourced_data_disabled_) {
        UpdateCommunityRatesDownload();
        UpdateVendorPricesDownload();
        UpdateDropTelemetry(delta);
    }
    if (show_auction_house_ || show_my_listings_) UpdateAuctionRequest(delta);
    UpdateMessageRequests(delta);

    navigation_refresh_timer_ += delta;
    if (navigation_target_map_id_ && navigation_refresh_timer_ >= 2.0f && GW::Map::GetIsMapLoaded() &&
        static_cast<uint32_t>(GW::Map::GetMapID()) != navigation_target_map_id_) {
        navigation_refresh_timer_ = 0.0f;
        if (set_travel_dest_fn_) set_travel_dest_fn_(navigation_target_map_id_);
    }

    if (linker_waiting_for_item_ || auction_waiting_for_item_) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const auto* item = GW::Items::GetHoveredItem();
            if (item && item->item_id) {
                dismiss_inventory_context_menu_until_ = GetTickCount64() + 250;
                if (linker_waiting_for_item_) {
                    linker_waiting_for_item_ = false;
                    SetLinkerFocusedItem(item);
                    linker_focus_requested_ = true;
                    current_tab_ = 1;
                }
                else if (auction_waiting_for_item_) {
                    auction_waiting_for_item_ = false;
                    PrefillAuctionItem(item);
                    auction_focus_requested_ = true;
                    current_tab_ = 0;
                }
            }
        }
    }

    if (tracked_mob_name_.empty()) return;
    tracking_scan_timer_ += delta;
    if (tracking_scan_timer_ < 0.25f) return;
    tracking_scan_timer_ = 0.0f;
    RefreshTrackedMobMarker();
}

bool DropExplorerPlugin::WndProc(const UINT message, const WPARAM, const LPARAM)
{
    if ((message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_GW_RBUTTONCLICK) &&
        (linker_waiting_for_item_ || auction_waiting_for_item_)) {
        const auto* item = GW::Items::GetHoveredItem();
        if (item && item->item_id) {
            dismiss_inventory_context_menu_until_ = GetTickCount64() + 250;
            if (linker_waiting_for_item_) {
                linker_waiting_for_item_ = false;
                SetLinkerFocusedItem(item);
                linker_focus_requested_ = true;
                current_tab_ = 1;
            }
            else if (auction_waiting_for_item_) {
                auction_waiting_for_item_ = false;
                PrefillAuctionItem(item);
                auction_focus_requested_ = true;
                current_tab_ = 0;
            }
            return true;
        }
    }
    return false;
}

bool DropExplorerPlugin::RefreshTrackedMobMarker()
{
    tracked_mob_visible_ = false;
    tracked_agent_id_ = 0;

    if (!GW::Map::GetIsMapLoaded() ||
        GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable ||
        static_cast<uint32_t>(GW::Map::GetMapID()) != tracked_map_id_) {
        return false;
    }

    const auto* player = GW::Agents::GetControlledCharacter();
    const auto* agents = GW::Agents::GetAgentArray();
    if (!player || !agents) return false;

    const GW::Agent* closest = nullptr;
    auto closest_distance_sq = std::numeric_limits<float>::max();
    for (const auto* agent : *agents) {
        if (!agent || agent->agent_id == player->agent_id) continue;
        const auto name = GetResolvedAgentName(agent->agent_id, agent);
        if (ToLower(name) != ToLower(tracked_mob_name_)) continue;
        if (const auto* living = agent->GetAsAgentLiving(); living && !living->GetIsAlive()) continue;
        const auto dx = agent->pos.x - player->pos.x;
        const auto dy = agent->pos.y - player->pos.y;
        const auto distance_sq = dx * dx + dy * dy;
        if (distance_sq >= closest_distance_sq) continue;
        closest = agent;
        closest_distance_sq = distance_sq;
    }

    if (!closest) return false;
    tracked_mob_visible_ = true;
    tracked_agent_id_ = closest->agent_id;
    const auto marker_dx = closest->pos.x - tracked_marker_x_;
    const auto marker_dy = closest->pos.y - tracked_marker_y_;
    if (set_custom_point_fn_ && (!tracked_marker_set_ || marker_dx * marker_dx + marker_dy * marker_dy >= 4096.0f)) {
        const auto set_custom_point = set_custom_point_fn_;
        const auto x = closest->pos.x;
        const auto y = closest->pos.y;
        tracked_marker_x_ = x;
        tracked_marker_y_ = y;
        tracked_marker_set_ = true;
        GW::GameThread::Enqueue([set_custom_point, x, y] { set_custom_point(x, y); });
    }
    navigation_status_ = std::format("Tracking {} at {:.0f}, {:.0f}", tracked_mob_name_, closest->pos.x, closest->pos.y);
    return true;
}

void DropExplorerPlugin::LoadSettings(const wchar_t* folder)
{
    ToolboxUIPlugin::LoadSettings(folder);
    settings_folder_ = folder ? folder : L"";
    LoadSetting("telemetry_enabled", telemetry_enabled_);
    LoadSetting("download_community_rates", download_community_rates_);
    LoadSetting("telemetry_install_id", telemetry_install_id_);
    auto legacy_auction_disabled = false;
    auto legacy_messages_disabled = false;
    auto legacy_explorer_tools_hidden = false;
    crowdsourced_data_disabled_ = !telemetry_enabled_;
    LoadSetting("auction_disabled", legacy_auction_disabled);
    LoadSetting("messages_disabled", legacy_messages_disabled);
    LoadSetting("explorer_tools_hidden", legacy_explorer_tools_hidden);
    show_auction_house_ = !legacy_auction_disabled;
    show_my_listings_ = !legacy_auction_disabled;
    show_messages_ = !legacy_messages_disabled;
    show_zone_explorer_ = !legacy_explorer_tools_hidden;
    show_item_search_ = !legacy_explorer_tools_hidden;
    LoadSetting("show_auction_house", show_auction_house_);
    LoadSetting("show_my_listings", show_my_listings_);
    LoadSetting("show_messages", show_messages_);
    LoadSetting("show_zone_explorer", show_zone_explorer_);
    LoadSetting("show_item_search", show_item_search_);
    LoadSetting("crowdsourced_data_disabled", crowdsourced_data_disabled_);
    telemetry_enabled_ = !crowdsourced_data_disabled_;
    download_community_rates_ = !crowdsourced_data_disabled_;
    batch_interval_minutes_ = 0.5f;
    auto gh_url = std::string{};
    LoadSetting("github_base_url", gh_url);
    if (!gh_url.empty()) {
        PluginUtils::StrCopy(github_base_url_, gh_url.c_str(), IM_ARRAYSIZE(github_base_url_));
    }

    if (telemetry_install_id_.empty()) telemetry_install_id_ = GenerateAnonymousId();

    const auto db_path = std::filesystem::path(settings_folder_) / "drops_database.json";
    if (std::filesystem::exists(db_path)) {
        LoadExternalDatabase(db_path);
    }
    if (!crowdsourced_data_disabled_) {
        StartCommunityRatesDownload();
        StartVendorPricesDownload();
    }
    LoadLocalMessages();
    LoadPendingAuctionListings();
}

void DropExplorerPlugin::SaveSettings(const wchar_t* folder)
{
    SaveSetting("telemetry_enabled", telemetry_enabled_);
    SaveSetting("download_community_rates", download_community_rates_);
    SaveSetting("show_auction_house", show_auction_house_);
    SaveSetting("show_my_listings", show_my_listings_);
    SaveSetting("show_messages", show_messages_);
    SaveSetting("show_zone_explorer", show_zone_explorer_);
    SaveSetting("show_item_search", show_item_search_);
    SaveSetting("crowdsourced_data_disabled", crowdsourced_data_disabled_);
    SaveSetting("telemetry_install_id", telemetry_install_id_);
    SaveSetting("batch_interval_minutes", batch_interval_minutes_);
    SaveSetting("github_base_url", std::string(github_base_url_));
    ToolboxUIPlugin::SaveSettings(folder);
}

void DropExplorerPlugin::DrawSettings()
{
    ImGui::TextDisabled("AuctionHouse&DropFinder Configuration");
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Visible tabs");
    ImGui::Checkbox("Show Auction House", &show_auction_house_);
    ImGui::Checkbox("Show My Listings", &show_my_listings_);
    ImGui::Checkbox("Show Messages", &show_messages_);
    ImGui::Checkbox("Show Zone Explorer", &show_zone_explorer_);
    ImGui::Checkbox("Show Item Search", &show_item_search_);
    if (!show_auction_house_ && !show_my_listings_) {
        if (auction_client_ && auction_client_->IsPending()) auction_client_->Abort();
        auction_request_kind_ = AuctionRequestKind::None;
        pending_auction_inflight_index_ = static_cast<size_t>(-1);
    }
    ImGui::TextDisabled("When Messages is hidden, cleanup checks delete delivered server mail without saving it locally.");
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Community data");
    if (ImGui::Checkbox("I don't want to send or receive crowdsourced drop-rate data", &crowdsourced_data_disabled_)) {
        telemetry_enabled_ = !crowdsourced_data_disabled_;
        download_community_rates_ = !crowdsourced_data_disabled_;
        if (crowdsourced_data_disabled_) {
            ResetDropTelemetry();
            upload_queue_.clear();
            vendor_upload_queue_.clear();
            if (telemetry_client_ && telemetry_client_->IsPending()) telemetry_client_->Abort();
            if (rates_client_ && rates_client_->IsPending()) rates_client_->Abort();
            if (vendor_prices_client_ && vendor_prices_client_->IsPending()) vendor_prices_client_->Abort();
        }
        else {
            StartCommunityRatesDownload();
            StartVendorPricesDownload();
        }
    }
    ImGui::TextWrapped("Sharing anonymous kill and drop observations helps the community calculate reliable drop rates and improve farming methods. When disabled, kills and drops are not monitored at all.");
    ImGui::Separator();
    ImGui::TextWrapped("Server: %s", service_base_url_.empty() ? "Connecting..." : service_base_url_.c_str());
    ImGui::TextDisabled("Drop upload: %s | Community sync: %s", last_upload_status_.c_str(), last_sync_status_.c_str());
}

void DropExplorerPlugin::ResetDropTelemetry()
{
    observed_mobs_.clear();
    observed_items_.clear();
    pending_kills_.clear();
    telemetry_map_id_ = 0;
}

void DropExplorerPlugin::UpdateDropTelemetry(const float delta)
{
    if (telemetry_enabled_) {
        batch_timer_ms_ += delta * 1000.0f;
        const auto interval_ms = batch_interval_minutes_ * 60.0f * 1000.0f;
        if (batch_timer_ms_ >= interval_ms) {
            batch_timer_ms_ = 0.0f;
            TriggerBatchUpload();
        }
    }

    if (telemetry_client_ && (telemetry_inflight_count_ || vendor_inflight_count_)) {
        if (telemetry_client_->IsCompleted()) {
            if (telemetry_client_->IsSuccessful()) {
                upload_queue_.erase(upload_queue_.begin(), upload_queue_.begin() + static_cast<ptrdiff_t>(telemetry_inflight_count_));
                vendor_upload_queue_.erase(vendor_upload_queue_.begin(), vendor_upload_queue_.begin() + static_cast<ptrdiff_t>(vendor_inflight_count_));
                last_upload_status_ = std::format("Uploaded {} kills, {} vendor records", telemetry_inflight_count_, vendor_inflight_count_);
                telemetry_next_retry_ms_ = 0;
            } else {
                telemetry_next_retry_ms_ = GetTickCount64() + 30000;
                last_upload_status_ = "Upload failed (retry in 30s)";
            }
            telemetry_client_->Clear();
            telemetry_inflight_count_ = 0;
            vendor_inflight_count_ = 0;
        }
    }

    if (!telemetry_enabled_) {
        if (!observed_mobs_.empty() || !observed_items_.empty() || !pending_kills_.empty()) ResetDropTelemetry();
        return;
    }
    if (!GW::Map::GetIsMapLoaded() || GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable) {
        ResetDropTelemetry();
        return;
    }

    const auto map_id = static_cast<uint32_t>(GW::Map::GetMapID());
    if (telemetry_map_id_ != map_id) {
        ResetDropTelemetry();
        telemetry_map_id_ = map_id;
    }
    const auto now_ms = GetTickCount64();
    static uint64_t last_scan_ms = 0;
    if (now_ms - last_scan_ms < 100) return;
    last_scan_ms = now_ms;

    const auto* agents = GW::Agents::GetAgentArray();
    const auto player_id = GW::Agents::GetControlledCharacterId();
    if (!agents || !player_id) return;

    for (const auto* agent : *agents) {
        if (!agent) continue;
        if (const auto* living = agent->GetAsAgentLiving(); living && living->IsNPC() &&
            living->allegiance == GW::Constants::Allegiance::Enemy && living->player_number) {
            const auto alive = living->GetIsAlive();
            auto [it, inserted] = observed_mobs_.try_emplace(agent->agent_id);
            auto& observed = it->second;
            if (inserted) {
                observed.model_id = living->player_number;
                observed.alive = alive;
            }
            const auto resolved_name = GetResolvedAgentName(agent->agent_id, agent);
            if (!resolved_name.empty()) observed.name = resolved_name;
            observed.x = agent->pos.x;
            observed.y = agent->pos.y;
            if (!inserted && observed.alive && !alive && !observed.name.empty()) {
                PendingKill kill;
                kill.event.event_id = GenerateAnonymousId();
                kill.event.observed_at = static_cast<uint64_t>(std::time(nullptr));
                kill.event.map_id = map_id;
                kill.event.hard_mode = GW::PartyMgr::GetIsPartyInHardMode();
                kill.event.party_size = std::max(1u, GW::PartyMgr::GetPartySize());
                kill.event.mob_model_id = observed.model_id;
                kill.event.mob_name = observed.name;
                kill.death_ms = now_ms;
                kill.x = observed.x;
                kill.y = observed.y;
                pending_kills_.push_back(std::move(kill));
            }
            observed.alive = alive;
            continue;
        }

        const auto* agent_item = agent->GetAsAgentItem();
        if (!agent_item) continue;
        const auto* item = GW::Items::GetItemById(agent_item->item_id);
        if (!item || !item->model_id || item->type == GW::Constants::ItemType::Bundle ||
            item->type == GW::Constants::ItemType::Quest_Item || item->type == GW::Constants::ItemType::Gold_Coin) {
            continue;
        }
        auto [it, inserted] = observed_items_.try_emplace(agent->agent_id);
        auto& observed = it->second;
        if (inserted) {
            observed.item_id = item->item_id;
            observed.model_id = item->model_id;
            observed.item_type = static_cast<uint32_t>(item->type);
            observed.rarity = GetItemRarity(item);
            observed.first_seen_ms = now_ms;
            observed.name = std::make_unique<PluginUtils::EncString>(item->name_enc, true);
        }
        observed.owner_id = agent_item->owner;
        observed.x = agent->pos.x;
        observed.y = agent->pos.y;
    }

    for (auto& [agent_id, item] : observed_items_) {
        if (item.handled || item.owner_id != player_id) continue;
        std::vector<size_t> candidates;
        for (size_t index = 0; index < pending_kills_.size(); ++index) {
            const auto& kill = pending_kills_[index];
            if (item.first_seen_ms + 250 < kill.death_ms || item.first_seen_ms > kill.death_ms + 5000) continue;
            const auto dx = item.x - kill.x;
            const auto dy = item.y - kill.y;
            if (dx * dx + dy * dy <= 490000.0f) candidates.push_back(index);
        }
        if (candidates.size() == 1) {
            auto item_name = item.name ? item.name->string() : std::string{};
            if (item_name.empty()) item_name = std::format("Item model {}", item.model_id);
            pending_kills_[candidates.front()].event.drops.push_back({
                item.model_id, StripXmlTags(item_name), item.item_type, item.rarity
            });
            item.handled = true;
        }
        else if (candidates.size() > 1) {
            for (const auto index : candidates) pending_kills_[index].event.confidence = "ambiguous";
            item.handled = true;
        }
    }

    for (auto it = pending_kills_.begin(); it != pending_kills_.end();) {
        if (now_ms < it->death_ms + 5000) {
            ++it;
            continue;
        }
        if (upload_queue_.size() < 1000) upload_queue_.push_back(std::move(it->event));
        it = pending_kills_.erase(it);
    }
    std::erase_if(observed_items_, [now_ms](const auto& entry) {
        return entry.second.first_seen_ms + 10000 < now_ms;
    });
}

void DropExplorerPlugin::TriggerBatchUpload()
{
    if (!telemetry_client_ || !telemetry_enabled_ || service_base_url_.empty()) return;
    if (upload_queue_.empty() && vendor_upload_queue_.empty()) return;
    if (telemetry_inflight_count_ || vendor_inflight_count_) return;

    const auto now_ms = GetTickCount64();
    if (now_ms < telemetry_next_retry_ms_) return;

    TelemetryEnvelope envelope;
    envelope.schema_version = 2;
    envelope.install_id = telemetry_install_id_;

    telemetry_inflight_count_ = std::min<size_t>(250, upload_queue_.size());
    envelope.events.assign(upload_queue_.begin(), upload_queue_.begin() + static_cast<ptrdiff_t>(telemetry_inflight_count_));

    vendor_inflight_count_ = std::min<size_t>(500, vendor_upload_queue_.size());
    envelope.vendor_events.assign(vendor_upload_queue_.begin(), vendor_upload_queue_.begin() + static_cast<ptrdiff_t>(vendor_inflight_count_));

    const auto payload = glz::write_json(envelope).value_or(std::string{});
    if (payload.empty()) {
        telemetry_inflight_count_ = 0;
        vendor_inflight_count_ = 0;
        telemetry_next_retry_ms_ = now_ms + 30000;
        return;
    }

    const auto endpoint = GetServiceBaseUrl() + "/v1/mobdroptelemetry";

    telemetry_client_->Clear();
    telemetry_client_->SetUrl(endpoint.c_str());
    telemetry_client_->SetMethod(HttpMethod::Post);
    telemetry_client_->SetHeader("Content-Type", "application/json");
    telemetry_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    telemetry_client_->SetPostContent(payload, ContentFlag::Copy);
    telemetry_client_->SetConnectTimeoutSec(5);
    telemetry_client_->SetTimeoutSec(15);
    telemetry_client_->ExecuteAsync();
}

void DropExplorerPlugin::OnPriceQuote(const GW::Packet::StoC::QuotedItemPrice* packet)
{
    if (!telemetry_enabled_ || !packet || !packet->itemid) return;
    const auto* item = GW::Items::GetItemById(packet->itemid);
    if (!item || !item->model_id) return;

    std::string vendor_type = "merchant";
    const auto* target = GW::Agents::GetTarget();
    if (target) {
        const auto target_name = ToLower(GetResolvedAgentName(target->agent_id, target));
        if (target_name.find("rare material") != std::string::npos) {
            vendor_type = "rare_material_trader";
        } else if (target_name.find("material") != std::string::npos) {
            vendor_type = "material_trader";
        } else if (target_name.find("rune") != std::string::npos) {
            vendor_type = "rune_trader";
        } else if (target_name.find("dye") != std::string::npos) {
            vendor_type = "dye_trader";
        } else if (target_name.find("scroll") != std::string::npos) {
            vendor_type = "scroll_trader";
        } else if (target_name.find("weaponsmith") != std::string::npos) {
            vendor_type = "weaponsmith";
        } else if (target_name.find("armorer") != std::string::npos) {
            vendor_type = "armorer";
        } else if (target_name.find("collector") != std::string::npos) {
            vendor_type = "collector";
        }
    }

    const bool in_inventory = item->bag && item->bag->IsInventoryBag();
    const std::string transaction_type = in_inventory ? "quote_sell" : "quote_buy";

    auto item_enc = PluginUtils::EncString(item->name_enc, true);
    auto item_name = item_enc.string();
    if (item_name.empty()) item_name = std::format("Item model {}", item->model_id);

    TelemetryVendorEvent ev;
    ev.transaction_id = GenerateAnonymousId();
    ev.observed_at = static_cast<uint64_t>(std::time(nullptr));
    ev.map_id = static_cast<uint32_t>(GW::Map::GetMapID());
    ev.vendor_type = vendor_type;
    ev.transaction_type = transaction_type;
    ev.item_model_id = item->model_id;
    ev.item_name = StripXmlTags(item_name);
    ev.unit_price = packet->price;
    ev.quantity = std::max<uint32_t>(1u, item->quantity);

    last_quoted_event_ = ev;
    last_quoted_ms_ = GetTickCount64();

    if (vendor_upload_queue_.size() < 1000) {
        vendor_upload_queue_.push_back(std::move(ev));
    }
}

void DropExplorerPlugin::OnTransactionDone(const GW::Packet::StoC::TransactionDone*)
{
    if (!telemetry_enabled_ || last_quoted_event_.transaction_id.empty()) return;
    if (GetTickCount64() - last_quoted_ms_ > 4000) return;

    TelemetryVendorEvent confirmed = last_quoted_event_;
    confirmed.transaction_id = GenerateAnonymousId();
    confirmed.observed_at = static_cast<uint64_t>(std::time(nullptr));
    confirmed.transaction_type = (last_quoted_event_.transaction_type == "quote_buy") ? "buy" : "sell";

    if (vendor_upload_queue_.size() < 1000) {
        vendor_upload_queue_.push_back(std::move(confirmed));
    }
    last_quoted_event_.transaction_id.clear();
}

void DropExplorerPlugin::StartVendorPricesDownload()
{
    if (!download_community_rates_ || !vendor_prices_client_ || vendor_prices_client_->IsPending()) return;
    vendor_prices_client_->Clear();
    auto url = std::string(github_base_url_);
    while (url.ends_with('/')) url.pop_back();
    url += "/data/vendor-prices.json";
    vendor_prices_client_->SetUrl(url.c_str());
    vendor_prices_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    vendor_prices_client_->SetConnectTimeoutSec(3);
    vendor_prices_client_->SetTimeoutSec(5);
    vendor_prices_client_->ExecuteAsync();
    vendor_prices_request_started_ = true;
}

void DropExplorerPlugin::UpdateVendorPricesDownload()
{
    if (!vendor_prices_request_started_ || !vendor_prices_client_ || !vendor_prices_client_->IsCompleted()) return;
    vendor_prices_request_started_ = false;
    if (vendor_prices_client_->IsSuccessful()) {
        VendorPricesDocument document;
        if (!glz::read_json(document, vendor_prices_client_->GetContent()) && document.schema_version == 1) {
            ApplyVendorPrices(document);
        }
    }
    vendor_prices_client_->Clear();
}

void DropExplorerPlugin::ApplyVendorPrices(const VendorPricesDocument& document)
{
    vendor_prices_by_model_id_.clear();
    vendor_prices_by_name_.clear();
    for (const auto& rec : document.prices) {
        if (rec.item_model_id > 0) {
            vendor_prices_by_model_id_[rec.item_model_id].push_back(rec);
        }
        if (!rec.item_name.empty()) {
            vendor_prices_by_name_[ToLower(rec.item_name)].push_back(rec);
        }
    }
    last_sync_status_ = std::format("Synced {} vendor prices ({} samples)",
        document.prices.size(), document.record_count);
}

void DropExplorerPlugin::StartCommunityRatesDownload()
{
    if (!download_community_rates_ || !rates_client_ || rates_client_->IsPending()) return;
    rates_client_->Clear();
    auto url = std::string(github_base_url_);
    while (url.ends_with('/')) url.pop_back();
    url += "/data/community-rates.json";
    rates_client_->SetUrl(url.c_str());
    rates_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    rates_client_->SetConnectTimeoutSec(3);
    rates_client_->SetTimeoutSec(5);
    rates_client_->ExecuteAsync();
    rates_request_started_ = true;
}

void DropExplorerPlugin::UpdateCommunityRatesDownload()
{
    if (!rates_request_started_ || !rates_client_ || !rates_client_->IsCompleted()) return;
    rates_request_started_ = false;
    if (rates_client_->IsSuccessful()) {
        CommunityRatesDocument document;
        if (!glz::read_json(document, rates_client_->GetContent()) && document.schema_version == 1) {
            ApplyCommunityRates(document);
        }
    }
    rates_client_->Clear();
}

void DropExplorerPlugin::ApplyCommunityRates(const CommunityRatesDocument& document)
{
    std::unordered_map<std::string, const CommunityRate*> best_rates;
    for (const auto& rate : document.records) {
        if (rate.eligible_kills < 100 || rate.confirmed_drops > rate.eligible_kills) continue;
        const auto key = std::format("{}|{}|{}", rate.map_id, ToLower(rate.mob_name), ToLower(rate.item_name));
        const auto found = best_rates.find(key);
        if (found == best_rates.end() || found->second->eligible_kills < rate.eligible_kills) best_rates[key] = &rate;
    }
    for (auto& zone : zones_) {
        for (auto& mob : zone.mobs) {
            for (auto& drop : mob.drops) {
                const auto key = std::format("{}|{}|{}", zone.map_id, ToLower(mob.name), ToLower(drop.name));
                const auto found = best_rates.find(key);
                if (found == best_rates.end()) continue;
                const auto& rate = *found->second;
                drop.est_chance_pct = static_cast<float>(rate.rate_pct);
                drop.chance_str = std::format("{:.3f}% observed ({} {}, n={})", rate.rate_pct,
                                              rate.hard_mode ? "HM" : "NM", rate.confidence, rate.eligible_kills);
                drop.notes += std::format("\nCommunity sample: {}/{} drops; 95% CI {:.3f}%–{:.3f}%.",
                                          rate.confirmed_drops, rate.eligible_kills,
                                          rate.confidence_low_pct, rate.confidence_high_pct);
            }
        }
    }
    BuildItemIndex();
}

void DropExplorerPlugin::BuildItemIndex()
{
    item_index_.clear();
    for (const auto& zone : zones_) {
        for (const auto& mob : zone.mobs) {
            for (const auto& drop : mob.drops) {
                DropExplorer::ItemLookupResult entry;
                entry.item_name = drop.name;
                entry.item_category = drop.category;
                entry.rarity = drop.rarity;
                entry.est_chance_pct = drop.est_chance_pct;
                entry.chance_str = drop.chance_str;
                entry.stats = drop.stats;
                entry.description = drop.description;
                entry.notes = drop.notes;
                entry.source_url = drop.source_url;
                entry.mob_name = mob.name;
                entry.mob_level = mob.level_str;
                entry.mob_prof = mob.profession;
                entry.mob_is_boss = mob.is_boss;
                entry.zone_name = zone.name;
                entry.region = zone.region;
                entry.campaign = zone.campaign;
                entry.map_id = zone.map_id;
                entry.nearest_outpost_id = zone.nearest_outpost_id;
                entry.nearest_outpost_name = zone.nearest_outpost_name;
                item_index_.push_back(std::move(entry));
            }
        }
    }
    SortItemIndex();
}

void DropExplorerPlugin::SortItemIndex()
{
    switch (current_sort_mode_) {
        case SortMode::NameAsc:
            std::sort(item_index_.begin(), item_index_.end(), [](const auto& a, const auto& b) {
                return ToLower(a.item_name) < ToLower(b.item_name);
            });
            break;
        case SortMode::NameDesc:
            std::sort(item_index_.begin(), item_index_.end(), [](const auto& a, const auto& b) {
                return ToLower(a.item_name) > ToLower(b.item_name);
            });
            break;
        case SortMode::ChanceDesc:
            std::sort(item_index_.begin(), item_index_.end(), [](const auto& a, const auto& b) {
                return a.est_chance_pct > b.est_chance_pct;
            });
            break;
        case SortMode::ChanceAsc:
            std::sort(item_index_.begin(), item_index_.end(), [](const auto& a, const auto& b) {
                return a.est_chance_pct < b.est_chance_pct;
            });
            break;
        case SortMode::ZoneAsc:
            std::sort(item_index_.begin(), item_index_.end(), [](const auto& a, const auto& b) {
                if (a.zone_name != b.zone_name) return ToLower(a.zone_name) < ToLower(b.zone_name);
                return ToLower(a.item_name) < ToLower(b.item_name);
            });
            break;
        case SortMode::RarityDesc:
            std::sort(item_index_.begin(), item_index_.end(), [](const auto& a, const auto& b) {
                if (a.rarity == DropExplorer::DropRarity::Unknown) return false;
                if (b.rarity == DropExplorer::DropRarity::Unknown) return true;
                return static_cast<uint8_t>(a.rarity) > static_cast<uint8_t>(b.rarity);
            });
            break;
    }
}

void DropExplorerPlugin::LoadExternalDatabase(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string json_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (json_content.empty()) return;

    std::vector<DropExplorer::ZoneInfo> external_zones;
    if (!glz::read_json(external_zones, json_content) && !external_zones.empty()) {
        NormalizeUnverifiedData(external_zones);
        zones_ = std::move(external_zones);
    }

    BuildItemIndex();
}

void DropExplorerPlugin::OpenWorldMap()
{
    GW::GameThread::Enqueue([] {
        if (!GW::UI::GetIsWorldMapShowing()) {
            GW::UI::Keypress(GW::UI::ControlAction_OpenWorldMap);
        }
    });
}

void DropExplorerPlugin::CenterWorldMapOnZone(const uint32_t map_id)
{
    if (!map_id) return;
    const auto* area = GW::Map::GetMapInfo(static_cast<GW::Constants::MapID>(map_id));
    if (!area) return;

    GW::GameThread::Enqueue([area] {
        if (!GW::UI::GetIsWorldMapShowing()) {
            GW::UI::Keypress(GW::UI::ControlAction_OpenWorldMap);
        }
        auto* wmc = GW::Map::GetWorldMapContext();
        if (!wmc) return;

        wmc->continent = area->continent;
        float target_x = 0.0f;
        float target_y = 0.0f;
        if (area->x && area->y) {
            target_x = static_cast<float>(area->x);
            target_y = static_cast<float>(area->y);
        }
        else if (area->icon_start_x && area->icon_start_y) {
            target_x = static_cast<float>(area->icon_start_x + (area->icon_end_x - area->icon_start_x) / 2);
            target_y = static_cast<float>(area->icon_start_y + (area->icon_end_y - area->icon_start_y) / 2);
        }

        if (target_x > 0.0f && target_y > 0.0f) {
            const auto span_x = wmc->bottom_right.x - wmc->top_left.x;
            const auto span_y = wmc->bottom_right.y - wmc->top_left.y;
            wmc->top_left.x = target_x - span_x * 0.5f;
            wmc->top_left.y = target_y - span_y * 0.5f;
            wmc->bottom_right.x = target_x + span_x * 0.5f;
            wmc->bottom_right.y = target_y + span_y * 0.5f;
        }
    });
}

void DropExplorerPlugin::ViewZone(const DropExplorer::ZoneInfo& zone)
{
    SelectZoneByName(zone.name);
    navigation_target_map_id_ = zone.map_id;
    navigation_refresh_timer_ = 0.0f;

    if (set_travel_dest_fn_ && zone.map_id > 0) {
        navigation_status_ = set_travel_dest_fn_(zone.map_id)
            ? std::format("Waypoint set for {}", zone.name)
            : std::format("Could not resolve a waypoint for {}", zone.name);
    }
    else {
        navigation_status_ = "Travel waypoint support is unavailable in the loaded GWToolbox build";
    }

    CenterWorldMapOnZone(zone.map_id);
}

void DropExplorerPlugin::TravelToZone(const DropExplorer::ZoneInfo& zone)
{
    navigation_target_map_id_ = zone.map_id;
    navigation_refresh_timer_ = 2.0f;
    if (set_travel_dest_fn_ && zone.map_id > 0) {
        set_travel_dest_fn_(zone.map_id);
    }

    if (GW::Map::GetIsMapLoaded() && static_cast<uint32_t>(GW::Map::GetMapID()) == zone.map_id) {
        navigation_status_ = std::format("Already in {}; tracking the selected source", zone.name);
        return;
    }
    const auto dest_id = (zone.nearest_outpost_id > 0) ? zone.nearest_outpost_id : zone.map_id;
    if (dest_id > 0) {
        navigation_status_ = std::format("Travelling to {} for {}", zone.nearest_outpost_name, zone.name);
        GW::Map::Travel(static_cast<GW::Constants::MapID>(dest_id));
    }
}

void DropExplorerPlugin::TrackMob(const DropExplorer::ZoneInfo& zone, const DropExplorer::MobInfo& mob, const bool travel)
{
    if (clear_custom_point_fn_) clear_custom_point_fn_();
    highlighted_mob_name_ = mob.name;
    SelectZoneByName(zone.name);
    current_tab_ = 3;
    zone_explorer_focus_requested_ = true;

    if (!mob.trackable) {
        tracked_mob_name_.clear();
        tracked_agent_id_ = 0;
        tracked_mob_visible_ = false;
        tracked_marker_set_ = false;
        if (travel) TravelToZone(zone);
        else ViewZone(zone);
        return;
    }
    tracked_mob_name_ = mob.name;
    tracked_map_id_ = zone.map_id;
    tracked_agent_id_ = 0;
    tracked_mob_visible_ = false;
    tracked_marker_set_ = false;
    tracking_scan_timer_ = 0.25f;
    navigation_target_map_id_ = zone.map_id;
    if (RefreshTrackedMobMarker()) {
        if (!travel) CenterWorldMapOnZone(zone.map_id);
        return;
    }
    if (travel) TravelToZone(zone);
    else ViewZone(zone);
}

void DropExplorerPlugin::SelectZoneByMapId(const uint32_t map_id)
{
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i].map_id == map_id) {
            selected_zone_idx_ = static_cast<int>(i);
            current_tab_ = 3;
            return;
        }
    }
}

void DropExplorerPlugin::SelectZoneByName(const std::string& zone_name)
{
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i].name == zone_name) {
            selected_zone_idx_ = static_cast<int>(i);
            current_tab_ = 3;
            return;
        }
    }
}

void DropExplorerPlugin::DrawItemTooltip(const std::string& name,
                                         const std::string& category,
                                         const DropExplorer::DropRarity rarity,
                                         const std::string& chance_str,
                                         const std::string& stats,
                                         const std::string& description,
                                         const std::string& notes,
                                         const std::string& source_url,
                                         const uint32_t model_id)
{
    ImGui::BeginTooltip();

    IDirect3DTexture9** tex_ptr = nullptr;
    if (get_item_image_fn_) {
        const auto name_ws = ToWString(name);
        tex_ptr = get_item_image_fn_(name_ws.c_str());
    }

    if (tex_ptr && *tex_ptr) {
        ImGui::Image(*tex_ptr, ImVec2(56.0f, 56.0f));
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ImGui::TextColored(GetRarityColor(rarity), "%s", name.c_str());
    ImGui::TextDisabled("[%s] - %s", category.c_str(), DropExplorer::GetRarityName(rarity));
    ImGui::Text("Published Drop Rate: %s", chance_str.c_str());
    ImGui::EndGroup();

    if (!stats.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "Weapon / Item Stats:");
        ImGui::Text("%s", stats.c_str());
    }

    if (!description.empty()) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(320.0f);
        ImGui::Text("%s", description.c_str());
        ImGui::PopTextWrapPos();
    }

    if (!notes.empty()) {
        ImGui::TextDisabled("Note: %s", notes.c_str());
    }

    const std::vector<VendorPriceRecord>* vendor_records = nullptr;
    if (model_id > 0) {
        const auto it = vendor_prices_by_model_id_.find(model_id);
        if (it != vendor_prices_by_model_id_.end()) vendor_records = &it->second;
    }
    if (!vendor_records) {
        const auto it = vendor_prices_by_name_.find(ToLower(name));
        if (it != vendor_prices_by_name_.end()) vendor_records = &it->second;
    }
    if (vendor_records && !vendor_records->empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "Vendor / Trader Prices (Crowdsourced):");
        for (const auto& rec : *vendor_records) {
            ImGui::BulletText("%s (%s): avg %.0fg (min %ug, max %ug, n=%u)",
                              rec.vendor_type.c_str(), rec.transaction_type.c_str(),
                              rec.avg_price, rec.min_price, rec.max_price, rec.sample_count);
        }
    }

    if (!source_url.empty()) {
        ImGui::Separator();
        if (ImGui::SmallButton("Open Guild Wars Wiki source")) {
            ShellExecuteA(nullptr, "open", source_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    ImGui::EndTooltip();
}

void DropExplorerPlugin::Draw(IDirect3DDevice9*)
{
    const auto* vis = GetVisiblePtr();
    if (vis && !*vis) return;

    ImGui::SetNextWindowSize(ImVec2(880.0f, 580.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags())) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("DropExplorerTabs")) {
        if (show_auction_house_) {
            const auto auction_flags = auction_focus_requested_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Auction House", nullptr, auction_flags)) {
                auction_focus_requested_ = false;
                current_tab_ = 0;
                DrawAuctionHouseView();
                ImGui::EndTabItem();
            }
        }
        if (show_my_listings_) {
            if (ImGui::BeginTabItem("My Listings")) {
                current_tab_ = 1;
                DrawMyListingsView();
                ImGui::EndTabItem();
            }
        }

        if (show_messages_) {
            size_t unread_count = 0;
            for (const auto& msg : local_messages_) {
                if (!msg.is_read) unread_count++;
            }
            const std::string msg_title = unread_count > 0 ? std::format("Messages ({})###MessagesTab", unread_count) : "Messages###MessagesTab";
            const auto msg_flags = message_tab_focus_requested_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(msg_title.c_str(), nullptr, msg_flags)) {
                message_tab_focus_requested_ = false;
                current_tab_ = 2;
                DrawMessagesView();
                ImGui::EndTabItem();
            }
        }

        if (show_zone_explorer_) {
            const auto zone_flags = zone_explorer_focus_requested_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Zone Explorer", nullptr, zone_flags)) {
                zone_explorer_focus_requested_ = false;
                current_tab_ = 3;
                DrawZoneExplorerView();
                ImGui::EndTabItem();
            }
        }
        if (show_item_search_) {
            if (ImGui::BeginTabItem("Item Search")) {
                current_tab_ = 4;
                DrawItemSearchView();
                ImGui::EndTabItem();
            }
        }

        if (ImGui::BeginTabItem("Settings")) {
            current_tab_ = 5;
            DrawSettings();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void DropExplorerPlugin::DrawZoneExplorerView()
{
    const char* campaigns[] = {"All Campaigns", "Prophecies", "Factions", "Nightfall", "Eye of the North", "Elite / Core"};
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("##CampaignFilter", &selected_campaign_idx_, campaigns, IM_ARRAYSIZE(campaigns));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##ZoneSearch", "Filter zones...", zone_search_buf_, IM_ARRAYSIZE(zone_search_buf_));

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        zone_search_buf_[0] = '\0';
        selected_campaign_idx_ = 0;
    }

    if (!tracked_mob_name_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(
            tracked_mob_visible_ ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
            tracked_mob_visible_ ? "Tracking live spawn: %s" : "Waiting for spawn in client range: %s",
            tracked_mob_name_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop Tracking")) {
            tracked_mob_name_.clear();
            tracked_agent_id_ = 0;
            tracked_mob_visible_ = false;
            tracked_marker_set_ = false;
            if (clear_custom_point_fn_) clear_custom_point_fn_();
        }
    }

    ImGui::Separator();

    const auto avail_width = ImGui::GetContentRegionAvail().x;
    const auto list_width = std::max(240.0f, avail_width * 0.32f);

    // Left pane: Zones list
    ImGui::BeginChild("ZonesListPane", ImVec2(list_width, 0.0f), true);
    for (size_t i = 0; i < zones_.size(); ++i) {
        const auto& zone = zones_[i];

        if (selected_campaign_idx_ > 0) {
            const auto filter_camp = static_cast<DropExplorer::Campaign>(selected_campaign_idx_);
            if (zone.campaign != filter_camp) continue;
        }

        if (zone_search_buf_[0] != '\0' &&
            !CaseInsensitiveContains(zone.name, zone_search_buf_) &&
            !CaseInsensitiveContains(zone.region, zone_search_buf_)) {
            continue;
        }

        ImGui::PushID(static_cast<int>(i));
        const bool is_selected = (selected_zone_idx_ == static_cast<int>(i));
        const std::string label = zone.name + " (" + zone.region + ")";
        if (ImGui::Selectable(label.c_str(), is_selected)) {
            selected_zone_idx_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane: Selected zone details
    ImGui::BeginChild("ZoneDetailPane", ImVec2(0.0f, 0.0f), true);
    if (selected_zone_idx_ >= 0 && selected_zone_idx_ < static_cast<int>(zones_.size())) {
        const auto& zone = zones_[selected_zone_idx_];

        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", zone.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("| %s - %s", DropExplorer::GetCampaignName(zone.campaign), zone.region.c_str());

        if (zone.nearest_outpost_id > 0) {
            ImGui::SameLine();
            const std::string travel_label = "Travel (" + zone.nearest_outpost_name + ")";
            if (ImGui::SmallButton(travel_label.c_str())) {
                TravelToZone(zone);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Fast travel to %s and set a waypoint on %s", zone.nearest_outpost_name.c_str(), zone.name.c_str());
            }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("View on Map / Waypoint")) {
            ViewZone(zone);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open World Map and set destination waypoint on %s", zone.name.c_str());
        }
        if (!navigation_status_.empty()) {
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), "%s", navigation_status_.c_str());
        }

        ImGui::Separator();

        if (zone.mobs.empty()) {
            ImGui::TextDisabled("No mob records cataloged for this area yet.");
        }

        std::vector<const DropExplorer::MobInfo*> bosses;
        std::vector<const DropExplorer::MobInfo*> regular_mobs;
        for (const auto& mob : zone.mobs) {
            if (mob.is_boss) bosses.push_back(&mob);
            else regular_mobs.push_back(&mob);
        }

        if (!bosses.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::Text("Area Bosses (%zu)", bosses.size());
            ImGui::PopStyleColor();

            for (const auto* boss : bosses) {
                ImGui::PushID(boss->name.c_str());
                const auto is_highlighted = !highlighted_mob_name_.empty() && boss->name == highlighted_mob_name_;
                std::string header = "[BOSS] " + boss->name + " (" + boss->level_str + " " + boss->profession + ")";
                if (!boss->boss_skill.empty()) {
                    header += " - Elite: " + boss->boss_skill;
                }
                if (is_highlighted) header += " [TARGET MOB]";

                if (is_highlighted && zone_explorer_focus_requested_) {
                    ImGui::SetScrollHereY(0.25f);
                }

                if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (!boss->description.empty()) ImGui::TextWrapped("%s", boss->description.c_str());
                    if (!boss->movement_notes.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Movement: %s", boss->movement_notes.c_str());
                    }
                    if (ImGui::SmallButton("View / Track")) TrackMob(zone, *boss, false);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Travel & Track")) TrackMob(zone, *boss, true);
                    if (!boss->source_url.empty()) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Mob Wiki")) ShellExecuteA(nullptr, "open", boss->source_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    if (boss->drops.empty()) ImGui::TextDisabled("No drops are documented on this mob's source page.");
                    if (ImGui::BeginTable("BossDropTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Item Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableSetupColumn("Published Rate", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                        ImGui::TableSetupColumn("Fixed Stats", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                        ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthFixed, 104.0f);
                        ImGui::TableHeadersRow();

                        for (const auto& drop : boss->drops) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextColored(GetRarityColor(drop.rarity), "%s", drop.name.c_str());
                            if (ImGui::IsItemHovered()) {
                                DrawItemTooltip(drop.name, drop.category, drop.rarity, drop.chance_str, drop.stats, drop.description, drop.notes, drop.source_url);
                            }

                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", drop.category.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", drop.chance_str.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextWrapped("%s", drop.stats.empty() ? "Not published in dataset" : drop.stats.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextWrapped("%s", drop.notes.c_str());
                            ImGui::TableNextColumn();
                            ImGui::PushID(drop.name.c_str());
                            if (ImGui::SmallButton("View")) TrackMob(zone, *boss, false);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Travel")) TrackMob(zone, *boss, true);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::PopID();
                ImGui::Spacing();
            }
        }

        if (!regular_mobs.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            ImGui::Text("Monsters (%zu)", regular_mobs.size());
            ImGui::PopStyleColor();

            for (const auto* mob : regular_mobs) {
                ImGui::PushID(mob->name.c_str());
                const auto is_highlighted = !highlighted_mob_name_.empty() && mob->name == highlighted_mob_name_;
                std::string header = mob->name + " (" + mob->level_str + " " + mob->profession + ")";
                if (is_highlighted) header += " [TARGET MOB]";

                if (is_highlighted && zone_explorer_focus_requested_) {
                    ImGui::SetScrollHereY(0.25f);
                }

                if (ImGui::CollapsingHeader(header.c_str(), is_highlighted ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
                    if (!mob->description.empty()) ImGui::TextWrapped("%s", mob->description.c_str());
                    if (!mob->movement_notes.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Movement: %s", mob->movement_notes.c_str());
                    }
                    if (ImGui::SmallButton("View / Track")) TrackMob(zone, *mob, false);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Travel & Track")) TrackMob(zone, *mob, true);
                    if (!mob->source_url.empty()) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Mob Wiki")) ShellExecuteA(nullptr, "open", mob->source_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    if (mob->drops.empty()) ImGui::TextDisabled("No drops are documented on this mob's source page.");
                    if (ImGui::BeginTable("MobDropTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Item Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableSetupColumn("Published Rate", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                        ImGui::TableSetupColumn("Fixed Stats", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                        ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthFixed, 104.0f);
                        ImGui::TableHeadersRow();

                        for (const auto& drop : mob->drops) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextColored(GetRarityColor(drop.rarity), "%s", drop.name.c_str());
                            if (ImGui::IsItemHovered()) {
                                DrawItemTooltip(drop.name, drop.category, drop.rarity, drop.chance_str, drop.stats, drop.description, drop.notes, drop.source_url);
                            }

                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", drop.category.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", drop.chance_str.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextWrapped("%s", drop.stats.empty() ? "Not published in dataset" : drop.stats.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextWrapped("%s", drop.notes.c_str());
                            ImGui::TableNextColumn();
                            ImGui::PushID(drop.name.c_str());
                            if (ImGui::SmallButton("View")) TrackMob(zone, *mob, false);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Travel")) TrackMob(zone, *mob, true);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
    } else {
        ImGui::TextDisabled("Select an explorable area on the left to view mob and drop tables.");
    }
    ImGui::EndChild();
}

void DropExplorerPlugin::DrawItemSearchView()
{
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##ItemSearchBox", "Search items (e.g. Chaos Axe, Ectoplasm)...", item_search_buf_, IM_ARRAYSIZE(item_search_buf_));

    ImGui::SameLine();
    const char* categories[] = {"All Categories", "Unique Weapon", "Non-Unique Weapon", "Documented Drop", "Chest Contents", "Trophy", "Inscribable Weapon", "Exotic Weapon", "Rare Material", "Common Material", "Scroll", "Consumable", "Exotic Currency"};
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("##ItemCategoryFilter", &item_category_filter_idx_, categories, IM_ARRAYSIZE(categories));

    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        item_search_buf_[0] = '\0';
        item_category_filter_idx_ = 0;
    }

    // Sort buttons row
    ImGui::Spacing();
    ImGui::TextDisabled("Sort By:");
    ImGui::SameLine();
    if (ImGui::SmallButton("A-Z")) {
        current_sort_mode_ = (current_sort_mode_ == SortMode::NameAsc) ? SortMode::NameDesc : SortMode::NameAsc;
        SortItemIndex();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle sort by item name A-Z / Z-A");

    ImGui::SameLine();
    if (ImGui::SmallButton("Zone Name")) {
        current_sort_mode_ = SortMode::ZoneAsc;
        SortItemIndex();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Group and sort alphabetically by Explorable Zone");

    ImGui::SameLine();
    if (ImGui::SmallButton("Rarity")) {
        current_sort_mode_ = SortMode::RarityDesc;
        SortItemIndex();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by rarity (Exotic -> Green -> Rare -> Uncommon -> Common)");

    ImGui::Separator();

    if (ImGui::BeginTable("ItemSearchResults", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Item Name", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Dropped By", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Zone / Area", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Campaign", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Published Rate", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Fixed Stats", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        int matched_count = 0;
        for (size_t i = 0; i < item_index_.size(); ++i) {
            const auto& item = item_index_[i];

            if (item_category_filter_idx_ > 0) {
                const char* target_cat = categories[item_category_filter_idx_];
                if (item.item_category != target_cat) continue;
            }

            if (item_search_buf_[0] != '\0' &&
                !CaseInsensitiveContains(item.item_name, item_search_buf_) &&
                !CaseInsensitiveContains(item.mob_name, item_search_buf_) &&
                !CaseInsensitiveContains(item.zone_name, item_search_buf_)) {
                continue;
            }

            matched_count++;
            ImGui::TableNextRow();

            // Item name with thumbnail & stats tooltip
            ImGui::TableNextColumn();
            ImGui::TextColored(GetRarityColor(item.rarity), "%s", item.item_name.c_str());
            if (ImGui::IsItemHovered()) {
                DrawItemTooltip(item.item_name, item.item_category, item.rarity, item.chance_str, item.stats, item.description, item.notes, item.source_url);
            }

            // Category
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", item.item_category.c_str());

            // Dropped By
            ImGui::TableNextColumn();
            if (item.mob_is_boss) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[Boss] %s", item.mob_name.c_str());
            } else {
                ImGui::Text("%s", item.mob_name.c_str());
            }

            // Zone
            ImGui::TableNextColumn();
            ImGui::Text("%s", item.zone_name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Region: %s", item.region.c_str());
            }

            // Campaign
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", DropExplorer::GetCampaignName(item.campaign));

            ImGui::TableNextColumn();
            ImGui::Text("%s", item.chance_str.c_str());

            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", item.stats.empty() ? "Not published in dataset" : item.stats.c_str());

            // Action
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("View")) {
                for (const auto& z : zones_) {
                    if (z.name == item.zone_name) {
                        const auto mob = std::find_if(z.mobs.begin(), z.mobs.end(), [&item](const auto& candidate) {
                            return candidate.name == item.mob_name;
                        });
                        if (mob != z.mobs.end()) TrackMob(z, *mob, false);
                        break;
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("View %s in Zone Explorer, open World Map, and set waypoint", item.zone_name.c_str());
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Travel")) {
                for (const auto& z : zones_) {
                    if (z.name == item.zone_name) {
                        const auto mob = std::find_if(z.mobs.begin(), z.mobs.end(), [&item](const auto& candidate) {
                            return candidate.name == item.mob_name;
                        });
                        if (mob != z.mobs.end()) TrackMob(z, *mob, true);
                        break;
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Fast travel to %s and set waypoint on %s for %s", item.nearest_outpost_name.c_str(), item.zone_name.c_str(), item.item_name.c_str());
            }
            ImGui::PopID();
        }

        ImGui::EndTable();

        if (matched_count == 0) {
            ImGui::TextDisabled("No matching items found for '%s'.", item_search_buf_);
        }
    }
}

std::string DropExplorerPlugin::GetServiceBaseUrl() const
{
    return service_base_url_;
}

void DropExplorerPlugin::UpdateServiceDiscovery()
{
    const auto now = GetTickCount64();
    if (discovery_client_ && discovery_client_->IsCompleted()) {
        if (discovery_client_->IsSuccessful()) {
            auto url = discovery_client_->GetContent();
            while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) url.pop_back();
            constexpr auto prefix = std::string_view("https://");
            constexpr auto suffix = std::string_view(".trycloudflare.com");
            if (url.starts_with(prefix) && url.ends_with(suffix)) {
                const auto hostname = std::string_view(url).substr(prefix.size(), url.size() - prefix.size() - suffix.size());
                if (!hostname.empty() && std::ranges::all_of(hostname, [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; })) {
                    if (service_base_url_ != url) {
                        service_base_url_ = std::move(url);
                        auction_refresh_timer_ = 60.0f;
                    }
                }
            }
        }
        discovery_client_.reset();
    }
    if (discovery_client_ || now < discovery_next_ms_) return;
    discovery_next_ms_ = now + 300000;
    discovery_client_ = std::make_unique<AsyncRestClient>();
    constexpr auto url = "https://api.github.com/repos/joshuwamccoy23-glitch/gw-drops/contents/data/auction-droplistings.txt?ref=main";
    discovery_client_->SetUrl(url);
    discovery_client_->SetHeader("Accept", "application/vnd.github.raw+json");
    discovery_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    discovery_client_->SetConnectTimeoutSec(5);
    discovery_client_->SetTimeoutSec(10);
    discovery_client_->ExecuteAsync();
}

void DropExplorerPlugin::StartTradeChatFeed()
{
    trade_chat_thread_ = std::jthread([this](const std::stop_token stop) {
        const auto wait = [&](const uint32_t milliseconds) {
            for (uint32_t elapsed = 0; elapsed < milliseconds && !stop.stop_requested(); elapsed += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        };
        constexpr glz::opts json_options{.error_on_unknown_keys = false};
        while (!stop.stop_requested()) {
            if (!trade_chat_enabled_) {
                trade_chat_connected_ = false;
                wait(500);
                continue;
            }
            std::unique_ptr<easywsclient::WebSocket> socket(easywsclient::WebSocket::from_url("wss://kamadan.gwtoolbox.com"));
            if (!socket) {
                trade_chat_connected_ = false;
                wait(5000);
                continue;
            }
            trade_chat_connected_ = true;
            socket->send(R"({"query":" "})");
            while (!stop.stop_requested() && trade_chat_enabled_ && socket->getReadyState() != easywsclient::WebSocket::CLOSED) {
                socket->poll(200);
                socket->dispatch([&](const std::string& data) {
                    TradeChatApi::TradeChatEnvelope envelope;
                    if (glz::read<json_options>(envelope, data)) return;
                    std::vector<TradeChatMessage> received;
                    if (!envelope.results.empty()) {
                        received.reserve(envelope.results.size());
                        for (const auto& raw : envelope.results) {
                            const auto timestamp = ParseTradeChatTimestamp(raw.t.str);
                            if (!raw.s.empty() && !raw.m.empty() && timestamp) received.push_back({raw.s, raw.m, timestamp});
                        }
                    }
                    else {
                        const auto timestamp = ParseTradeChatTimestamp(envelope.t.str);
                        if (!envelope.s.empty() && !envelope.m.empty() && timestamp) received.push_back({envelope.s, envelope.m, timestamp});
                    }
                    if (received.empty()) return;
                    std::scoped_lock lock(trade_chat_mutex_);
                    trade_chat_inbox_.insert(trade_chat_inbox_.end(),
                        std::make_move_iterator(received.begin()), std::make_move_iterator(received.end()));
                });
            }
            trade_chat_connected_ = false;
            wait(1000);
        }
    });
}

void DropExplorerPlugin::UpdateTradeChatListings()
{
    std::vector<TradeChatMessage> received;
    {
        std::scoped_lock lock(trade_chat_mutex_);
        received.swap(trade_chat_inbox_);
    }
    for (const auto& message : received) IngestTradeChatMessage(message);

    const auto now = static_cast<uint64_t>(std::time(nullptr));
    std::erase_if(trade_chat_listings_, [&](const AuctionListing& listing) { return listing.expires_at <= now; });
}

void DropExplorerPlugin::IngestTradeChatMessage(const TradeChatMessage& message)
{
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    if (message.timestamp + 600 <= now) return;
    const auto clean = StripXmlTags(message.message);
    const std::regex marker_regex(R"(\b(WTB|WTS)\b)", std::regex_constants::icase);
    const std::regex price_regex(R"((\d+(?:[\.,]\d+)?)\s*(ectoplasm(?:s)?|ectos?|e|armbrace(?:s)?|arms?|a|platinum|plat|k|gold|g)\b)", std::regex_constants::icase);
    const std::regex quantity_regex(R"(^\s*(?:x\s*(\d+)|(\d+)\s*x)\s*)", std::regex_constants::icase);
    const std::regex stat_regex(R"(\b(?:q|req)\s*\d+\b|(?:\+|-)\d+\s*(?:/|\^)\s*-?\d+|\+\d+\b)", std::regex_constants::icase);
    const std::regex parenthetical_regex(R"(\([^)]{1,80}\))");
    const std::regex noise_regex(R"(\b(?:stack|stacks|ea|each|only|pm|me|offer|offers|open\s+trade|pls|please)\b)", std::regex_constants::icase);
    const std::regex whitespace_regex(R"(\s+)");

    std::vector<std::pair<size_t, std::string>> markers;
    for (auto it = std::sregex_iterator(clean.begin(), clean.end(), marker_regex); it != std::sregex_iterator(); ++it) {
        markers.emplace_back(static_cast<size_t>(it->position()), ToLower((*it)[1].str()));
    }
    for (size_t marker_index = 0; marker_index < markers.size(); ++marker_index) {
        const auto begin = markers[marker_index].first + 3;
        const auto end = marker_index + 1 < markers.size() ? markers[marker_index + 1].first : clean.size();
        auto section = clean.substr(begin, end - begin);
        for (size_t i = 0; i < section.size(); ++i) {
            const auto comma_between_digits = section[i] == ',' && i > 0 && i + 1 < section.size() &&
                std::isdigit(static_cast<unsigned char>(section[i - 1])) && std::isdigit(static_cast<unsigned char>(section[i + 1]));
            if ((section[i] == ',' && !comma_between_digits) || section[i] == ';' || section[i] == '|') section[i] = '\n';
            if (section[i] == ':' && i + 1 < section.size() && section[i + 1] == ':') {
                section[i] = '\n';
                section[i + 1] = ' ';
            }
        }

        std::stringstream fragments(section);
        std::string fragment;
        while (std::getline(fragments, fragment)) {
            fragment = TrimText(fragment, " \t\r\n-=:~^");
            std::smatch price_match;
            if (!std::regex_search(fragment, price_match, price_regex)) continue;

            auto descriptor = fragment.substr(0, static_cast<size_t>(price_match.position()));
            auto quantity = uint32_t{1};
            std::smatch quantity_match;
            if (std::regex_search(descriptor, quantity_match, quantity_regex)) {
                const auto raw_quantity = quantity_match[1].matched ? quantity_match[1].str() : quantity_match[2].str();
                quantity = static_cast<uint32_t>(std::clamp(strtoul(raw_quantity.c_str(), nullptr, 10), 1ul, 250ul));
                descriptor = quantity_match.suffix().str();
            }

            std::vector<std::string> stats;
            for (auto it = std::sregex_iterator(descriptor.begin(), descriptor.end(), stat_regex); it != std::sregex_iterator(); ++it) {
                stats.push_back(it->str());
            }
            for (auto it = std::sregex_iterator(descriptor.begin(), descriptor.end(), parenthetical_regex); it != std::sregex_iterator(); ++it) {
                stats.push_back(TrimText(it->str(), " ()"));
            }
            auto item_name = std::regex_replace(descriptor, stat_regex, " ");
            item_name = std::regex_replace(item_name, parenthetical_regex, " ");
            item_name = std::regex_replace(item_name, noise_regex, " ");
            item_name = std::regex_replace(item_name, whitespace_regex, " ");
            item_name = TrimText(item_name, " \t\r\n-=:~^()[]{}");
            if (item_name.size() < 2) continue;

            auto value_text = price_match[1].str();
            std::ranges::replace(value_text, ',', '.');
            const auto value = strtod(value_text.c_str(), nullptr);
            if (value <= 0.0) continue;
            const auto currency_token = ToLower(price_match[2].str());
            auto currency_type = std::string{"gold"};
            auto currency_label = std::string{"Gold"};
            if (currency_token == "k" || currency_token.starts_with("plat")) {
                currency_type = "platinum";
                currency_label = "Platinum";
            }
            else if (currency_token == "a" || currency_token.starts_with("arm")) {
                currency_type = "armbrace";
                currency_label = value == 1.0 ? "Armbrace" : "Armbraces";
            }
            else if (currency_token == "e" || currency_token.starts_with("ecto")) {
                currency_type = "ectoplasm";
                currency_label = "Ectoplasm";
            }

            std::string modifiers;
            for (const auto& stat : stats) {
                if (!modifiers.empty()) modifiers += " | ";
                modifiers += stat;
            }
            const auto key = ToLower(message.sender) + '|' + markers[marker_index].second + '|' + ToLower(item_name);
            AuctionListing listing;
            listing.listing_id = std::format("trade-{:016x}", std::hash<std::string>{}(key));
            listing.seller_name = message.sender;
            listing.listing_type = markers[marker_index].second == "wtb" ? "buy" : "sell";
            listing.item_name = item_name;
            listing.quantity = quantity;
            listing.unit_price = static_cast<uint32_t>(std::clamp(std::llround(value), 0ll, 100000000ll));
            listing.currency_type = currency_type;
            listing.modifiers = modifiers;
            listing.created_at = message.timestamp;
            listing.expires_at = message.timestamp + 600;
            listing.is_trade_chat = true;
            listing.price_display = std::format("{} {}", value_text, currency_label);
            listing.sort_price = value;
            const auto existing = std::ranges::find(trade_chat_listings_, listing.listing_id, &AuctionListing::listing_id);
            if (existing == trade_chat_listings_.end()) trade_chat_listings_.push_back(std::move(listing));
            else if (existing->created_at <= listing.created_at) *existing = std::move(listing);
        }
    }
}

void DropExplorerPlugin::RefreshAuctionListings()
{
    if (auction_client_ && auction_client_->IsPending()) return;
    const auto base = GetServiceBaseUrl();
    if (base.empty()) {
        auction_refresh_timer_ = 0.0f;
        auction_status_ = "Connecting to listings server; retrying automatically";
        return;
    }
    auction_refresh_timer_ = 0.0f;
    auction_client_ = std::make_unique<AsyncRestClient>();
    const auto url = std::format("{}/v1/listings?limit=200", base);
    auction_client_->SetUrl(url.c_str());
    auction_client_->SetMethod(HttpMethod::Get);
    auction_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    auction_client_->SetConnectTimeoutSec(5);
    auction_client_->SetTimeoutSec(10);
    auction_client_->ExecuteAsync();
    auction_request_kind_ = AuctionRequestKind::Refresh;
    auction_status_ = "Loading listings...";
}

void DropExplorerPlugin::RefreshMyListings()
{
    if (auction_client_ && auction_client_->IsPending()) return;
    const auto base = GetServiceBaseUrl();
    if (base.empty() || telemetry_install_id_.empty()) return;
    auction_client_ = std::make_unique<AsyncRestClient>();
    const auto url = std::format("{}/v1/listings?limit=200&mine=true&install_id={}",
        base, PluginUtils::UrlEncode(telemetry_install_id_));
    auction_client_->SetUrl(url.c_str());
    auction_client_->SetMethod(HttpMethod::Get);
    auction_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    auction_client_->SetConnectTimeoutSec(5);
    auction_client_->SetTimeoutSec(10);
    auction_client_->ExecuteAsync();
    auction_request_kind_ = AuctionRequestKind::RefreshMine;
    auction_status_ = "Loading your listings...";
}

void DropExplorerPlugin::CreateAuctionListing()
{
    const auto* player_name = GW::PlayerMgr::GetPlayerName();
    if (!player_name || !*player_name || !auction_item_buf_[0] || !auction_publish_name_confirmed_) {
        auction_status_ = "Item, character name, and publishing confirmation are required";
        return;
    }
    AuctionListingRequest request;
    request.install_id = telemetry_install_id_.empty() ? GenerateAnonymousId() : telemetry_install_id_;
    request.seller_name = PluginUtils::WStringToString(player_name);
    request.listing_type = auction_type_idx_ == 0 ? "sell" : "buy";
    request.item_name = StripXmlTags(auction_item_buf_);
    request.item_model_id = auction_item_model_id_;
    request.quantity = static_cast<uint32_t>(std::max(1, auction_quantity_));
    request.unit_price = static_cast<uint32_t>(std::max(0, auction_unit_price_));
    constexpr const char* currency_types[] = {"gold", "platinum", "armbrace", "ectoplasm", "other"};
    request.currency_type = currency_types[std::clamp(auction_currency_idx_, 0, 4)];
    request.currency_item = auction_currency_idx_ == 4 ? auction_other_currency_buf_ : "";
    if (request.currency_type == "other" && request.currency_item.empty()) {
        auction_status_ = "Type the requested currency item";
        return;
    }
    request.modifiers = StripXmlTags(auction_modifiers_buf_);
    request.duration_hours = static_cast<uint32_t>(std::clamp(auction_duration_hours_, 1, 168));

    const auto local_limit = request.listing_type == "sell" ? size_t{30} : size_t{10};
    const auto pending_count = static_cast<size_t>(std::ranges::count_if(pending_auction_listings_, [&](const PendingAuctionListing& listing) {
        return listing.request.listing_type == request.listing_type;
    }));
    if (pending_count >= local_limit) {
        auction_status_ = std::format("Local {} queue limit reached ({})", request.listing_type, local_limit);
        return;
    }

    pending_auction_listings_.push_back({std::move(request), static_cast<uint64_t>(std::time(nullptr)) + 300});
    SavePendingAuctionListings();
    auction_status_ = std::format("{} queued locally for 5 minutes", auction_type_idx_ == 0 ? "Sell listing" : "Buy order");
    auction_publish_name_confirmed_ = false;
    auction_item_buf_[0] = 0;
    auction_modifiers_buf_[0] = 0;
    auction_other_currency_buf_[0] = 0;
    auction_item_model_id_ = 0;
    auction_quantity_ = 1;
    auction_currency_idx_ = 0;
    auction_show_matches_ = false;
    auction_item_from_inventory_ = false;
    auction_new_listing_open_ = false;
}

void DropExplorerPlugin::SendPendingAuctionListing()
{
    if (pending_auction_listings_.empty() || auction_request_kind_ != AuctionRequestKind::None ||
        (auction_client_ && auction_client_->IsPending()) || service_base_url_.empty()) return;
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    const auto it = std::ranges::min_element(pending_auction_listings_, {}, &PendingAuctionListing::send_at);
    if (it == pending_auction_listings_.end() || it->send_at > now) return;

    const auto payload = glz::write_json(it->request).value_or(std::string{});
    if (payload.empty()) return;
    pending_auction_inflight_index_ = static_cast<size_t>(std::distance(pending_auction_listings_.begin(), it));
    auction_client_ = std::make_unique<AsyncRestClient>();
    auction_client_->SetUrl((service_base_url_ + "/v1/listings").c_str());
    auction_client_->SetMethod(HttpMethod::Post);
    auction_client_->SetHeader("Content-Type", "application/json");
    auction_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    auction_client_->SetPostContent(payload, ContentFlag::Copy);
    auction_client_->SetConnectTimeoutSec(5);
    auction_client_->SetTimeoutSec(10);
    auction_client_->ExecuteAsync();
    auction_request_kind_ = AuctionRequestKind::Create;
    auction_status_ = "Publishing locally queued listing...";
}

void DropExplorerPlugin::LoadPendingAuctionListings()
{
    if (settings_folder_.empty()) return;
    std::ifstream file(std::filesystem::path(settings_folder_) / "pending_auction_listings.json");
    if (!file.is_open()) return;
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    PendingAuctionStore store;
    if (!glz::read_json(store, content)) pending_auction_listings_ = std::move(store.listings);
}

void DropExplorerPlugin::SavePendingAuctionListings()
{
    if (settings_folder_.empty()) return;
    const auto payload = glz::write_json(PendingAuctionStore{pending_auction_listings_}).value_or(std::string{});
    if (payload.empty()) return;
    std::ofstream file(std::filesystem::path(settings_folder_) / "pending_auction_listings.json", std::ios::trunc);
    if (file.is_open()) file << payload;
}

void DropExplorerPlugin::CancelAuctionListing(const std::string& listing_id)
{
    if (auction_client_ && auction_client_->IsPending()) return;
    const auto base = GetServiceBaseUrl();
    if (base.empty()) return;

    const auto install_id = telemetry_install_id_.empty() ? GenerateAnonymousId() : telemetry_install_id_;
    const auto payload = std::format("{{\"listing_id\":\"{}\",\"install_id\":\"{}\"}}", listing_id, install_id);

    auction_client_ = std::make_unique<AsyncRestClient>();
    const auto url = listing_id.empty() ? (base + "/v1/listings") : (base + "/v1/listings/" + listing_id);
    auction_client_->SetUrl(url.c_str());
    auction_client_->SetMethod("DELETE");
    auction_client_->SetHeader("Content-Type", "application/json");
    auction_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    auction_client_->SetPostContent(payload, ContentFlag::Copy);
    auction_client_->SetConnectTimeoutSec(5);
    auction_client_->SetTimeoutSec(10);
    auction_client_->ExecuteAsync();
    auction_request_kind_ = AuctionRequestKind::Cancel;
    auction_status_ = "Cancelling listing...";
}

void DropExplorerPlugin::UpdateAuctionRequest(const float delta)
{
    auction_refresh_timer_ += delta;
    if (auction_item_name_decoder_ && !auction_item_name_decoder_->IsDecoding()) {
        const auto name = StripXmlTags(auction_item_name_decoder_->string());
        if (!name.empty()) PluginUtils::StrCopy(auction_item_buf_, name.c_str(), IM_ARRAYSIZE(auction_item_buf_));
        auction_item_name_decoder_.reset();
    }
    if (auction_item_details_decoder_ && !auction_item_details_decoder_->IsDecoding()) {
        const auto raw_details = StripXmlTags(auction_item_details_decoder_->string());
        if (!raw_details.empty()) {
            std::stringstream ss(raw_details);
            std::string line;
            std::string clean_details;
            while (std::getline(ss, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                while (!line.empty() && line.front() == ' ') line.erase(line.begin());
                if (line.empty() || line.find("Value:") == 0) continue;
                if (!clean_details.empty()) clean_details.push_back('\n');
                clean_details += line;
            }
            PluginUtils::StrCopy(auction_modifiers_buf_, clean_details.c_str(), IM_ARRAYSIZE(auction_modifiers_buf_));
        }
        auction_item_details_decoder_.reset();
    }
    if (auction_request_kind_ == AuctionRequestKind::None) {
        SendPendingAuctionListing();
        if (auction_request_kind_ != AuctionRequestKind::None) return;
        if (auction_refresh_timer_ >= 60.0f) RefreshAuctionListings();
        return;
    }
    if (!auction_client_ || !auction_client_->IsCompleted()) return;
    const auto completed_kind = auction_request_kind_;
    auction_request_kind_ = AuctionRequestKind::None;
    const auto successful = auction_client_->IsSuccessful();
    if (completed_kind == AuctionRequestKind::Refresh) {
        auction_refresh_timer_ = 0.0f;
        if (successful) {
            AuctionListingsDocument document;
            if (!glz::read_json(document, auction_client_->GetContent()) && document.schema_version == 2) {
                auction_listings_ = std::move(document.listings);
                auction_status_ = std::format("{} active listings", auction_listings_.size());
                RefreshMyListings();
            }
            else {
                auction_status_ = "Server returned an invalid listing document";
            }
        }
        else {
            discovery_next_ms_ = 0;
            auction_status_ = std::format("Failed to load listings (HTTP {}); reconnecting", auction_client_->GetStatusCode());
        }
    }
    else if (completed_kind == AuctionRequestKind::RefreshMine) {
        if (successful) {
            AuctionListingsDocument document;
            if (!glz::read_json(document, auction_client_->GetContent()) && document.schema_version == 2) {
                my_auction_listings_ = std::move(document.listings);
                auction_status_ = std::format("{} active listings; {} yours", auction_listings_.size(), my_auction_listings_.size());
            }
            else {
                auction_status_ = "Server returned an invalid account listing document";
            }
        }
        else {
            discovery_next_ms_ = 0;
            auction_status_ = std::format("Failed to load your listings (HTTP {}); reconnecting", auction_client_->GetStatusCode());
        }
    }
    else if (successful && completed_kind == AuctionRequestKind::Create) {
        auction_status_ = "Listing published";
        if (pending_auction_inflight_index_ < pending_auction_listings_.size()) {
            pending_auction_listings_.erase(pending_auction_listings_.begin() + static_cast<ptrdiff_t>(pending_auction_inflight_index_));
            SavePendingAuctionListings();
        }
        pending_auction_inflight_index_ = static_cast<size_t>(-1);
        RefreshAuctionListings();
    }
    else if (successful && completed_kind == AuctionRequestKind::Cancel) {
        auction_status_ = "Listing cancelled";
        RefreshAuctionListings();
    }
    else {
        discovery_next_ms_ = 0;
        if (completed_kind == AuctionRequestKind::Create && pending_auction_inflight_index_ < pending_auction_listings_.size()) {
            pending_auction_listings_[pending_auction_inflight_index_].send_at = static_cast<uint64_t>(std::time(nullptr)) + 30;
            SavePendingAuctionListings();
            pending_auction_inflight_index_ = static_cast<size_t>(-1);
        }
        auction_status_ = std::format("Auction request failed (HTTP {})", auction_client_->GetStatusCode());
    }
}

void DropExplorerPlugin::PrefillAuctionItem(const GW::Item* item)
{
    if (!item) return;
    auction_item_model_id_ = item->model_id;
    auction_quantity_ = std::max<int>(1, item->quantity);
    auction_unit_price_ = 0;
    auction_type_idx_ = 0;
    auction_item_buf_[0] = 0;
    auction_modifiers_buf_[0] = 0;
    auction_show_matches_ = false;
    auction_item_from_inventory_ = true;
    auction_new_listing_open_ = true;
    current_tab_ = 0;
    auction_focus_requested_ = true;

    SetLinkerFocusedItem(item);

    const auto* name_enc = item->complete_name_enc && *item->complete_name_enc ? item->complete_name_enc :
        (item->single_item_name && *item->single_item_name ? item->single_item_name : item->name_enc);
    if (name_enc && *name_enc) {
        auction_item_name_decoder_ = std::make_unique<PluginUtils::EncString>(name_enc, true);
    }
    if (item->info_string && *item->info_string) {
        auction_item_details_decoder_ = std::make_unique<PluginUtils::EncString>(item->info_string, true);
    }
}

void DropExplorerPlugin::PrefillAuctionFromLinker(const LinkerDecodeState& state)
{
    auction_item_model_id_ = state.model_id;
    auction_quantity_ = std::max<int>(1, state.quantity);
    auction_unit_price_ = 0;
    auction_type_idx_ = 0;
    const auto clean_name = StripXmlTags(state.name_done ? state.name : "Item");
    PluginUtils::StrCopy(auction_item_buf_, clean_name.c_str(), IM_ARRAYSIZE(auction_item_buf_));

    std::string clean_mods;
    if (state.stats_done) {
        std::stringstream ss(StripXmlTags(state.stats));
        std::string line;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            while (!line.empty() && line.front() == ' ') line.erase(line.begin());
            if (line.empty() || line.find("Value:") == 0) continue;
            if (!clean_mods.empty()) clean_mods.push_back('\n');
            clean_mods += line;
        }
    }
    PluginUtils::StrCopy(auction_modifiers_buf_, clean_mods.c_str(), IM_ARRAYSIZE(auction_modifiers_buf_));
    auction_item_from_inventory_ = true;
    auction_show_matches_ = false;
    current_tab_ = 0;
    auction_focus_requested_ = true;
}

void DropExplorerPlugin::SetLinkerFocusedItem(const GW::Item* item)
{
    if (!item || !item->item_id) return;
    if (linker_decode_state_ && linker_decode_state_->item_id == item->item_id) return;

    if (linker_decode_state_) {
        linker_decode_state_->cancelled = true;
    }

    auto state = std::make_shared<LinkerDecodeState>();
    state->item_id = item->item_id;
    state->model_id = item->model_id;
    state->quantity = item->quantity;
    state->value = item->value;
    state->type = item->type;
    state->rarity = GetLinkerItemRarity(item);
    linker_decode_state_ = state;

    const auto* enc_name = item->complete_name_enc && *item->complete_name_enc ? item->complete_name_enc : item->name_enc;
    if (enc_name && *enc_name) {
        GW::UI::AsyncDecodeStr(enc_name, [](void* param, const wchar_t* s) {
            auto* ctx = static_cast<std::shared_ptr<LinkerDecodeState>*>(param);
            if (ctx && *ctx) {
                auto st = *ctx;
                if (!st->cancelled && s) {
                    st->name = StripXmlTags(PluginUtils::WStringToString(s));
                    st->name_done = true;
                    auto* plugin = static_cast<DropExplorerPlugin*>(ToolboxPluginInstance());
                    if (plugin) {
                        plugin->TriggerLinkerPendingSendIfReady(st);
                        if (plugin->auction_item_from_inventory_ && plugin->auction_item_buf_[0] == 0) {
                            PluginUtils::StrCopy(plugin->auction_item_buf_, st->name.c_str(), IM_ARRAYSIZE(plugin->auction_item_buf_));
                        }
                    }
                }
            }
            delete ctx;
        }, new std::shared_ptr<LinkerDecodeState>(state));
    }
    else {
        state->name = "Item";
        state->name_done = true;
    }

    if (item->info_string && *item->info_string) {
        GW::UI::AsyncDecodeStr(item->info_string, [](void* param, const wchar_t* s) {
            auto* ctx = static_cast<std::shared_ptr<LinkerDecodeState>*>(param);
            if (ctx && *ctx) {
                auto st = *ctx;
                if (!st->cancelled && s) {
                    st->stats = StripXmlTags(PluginUtils::WStringToString(s));
                    st->stats_done = true;
                    auto* plugin = static_cast<DropExplorerPlugin*>(ToolboxPluginInstance());
                    if (plugin) {
                        plugin->TriggerLinkerPendingSendIfReady(st);
                        if (plugin->auction_item_from_inventory_ && plugin->auction_modifiers_buf_[0] == 0) {
                            std::stringstream ss(st->stats);
                            std::string line;
                            std::string clean_details;
                            while (std::getline(ss, line)) {
                                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                                while (!line.empty() && line.front() == ' ') line.erase(line.begin());
                                if (line.empty() || line.find("Value:") == 0) continue;
                                if (!clean_details.empty()) clean_details.push_back('\n');
                                clean_details += line;
                            }
                            PluginUtils::StrCopy(plugin->auction_modifiers_buf_, clean_details.c_str(), IM_ARRAYSIZE(plugin->auction_modifiers_buf_));
                        }
                    }
                }
            }
            delete ctx;
        }, new std::shared_ptr<LinkerDecodeState>(state));
    }
    else {
        state->stats_done = true;
    }
}

std::vector<std::string> DropExplorerPlugin::BuildLinkerChatLines(const std::string& name_raw, const std::string& stats_raw, const uint32_t quantity, const std::string& note)
{
    std::vector<std::string> lines;
    const auto clean_name = CleanRawText(name_raw.empty() ? "Item" : name_raw);
    const auto item_tag = (quantity > 1)
        ? std::format("[x{}] [{}]", quantity, clean_name)
        : std::format("[{}]", clean_name);

    std::vector<std::string> tokens;
    std::stringstream ss(CleanRawText(stats_raw));
    std::string line_item;
    while (std::getline(ss, line_item)) {
        while (!line_item.empty() && (line_item.back() == '\r' || line_item.back() == ' ')) line_item.pop_back();
        while (!line_item.empty() && line_item.front() == ' ') line_item.erase(line_item.begin());
        if (line_item.empty()) continue;
        if (line_item.find("Value:") == 0) continue;
        tokens.push_back(line_item);
    }

    const auto clean_note = CleanRawText(note);
    if (!clean_note.empty()) tokens.push_back(clean_note);

    if (tokens.empty()) {
        lines.push_back(item_tag);
        return lines;
    }

    std::string current_line = item_tag;
    for (const auto& t : tokens) {
        const auto candidate = current_line + " | " + t;
        if (candidate.length() > 112 && current_line != item_tag) {
            lines.push_back(current_line);
            current_line = item_tag + " " + t;
        }
        else {
            current_line = candidate;
        }
    }
    if (!current_line.empty()) {
        if (current_line.length() > 118) current_line = current_line.substr(0, 115) + "...";
        lines.push_back(current_line);
    }
    return lines;
}

void DropExplorerPlugin::DispatchChatLine(const char channel, const std::string& message, const std::string& whisper_target)
{
    if (message.empty()) return;
    const auto wmsg = PluginUtils::StringToWString(message);
    if (!whisper_target.empty()) {
        const auto wtarget = PluginUtils::StringToWString(whisper_target);
        GW::Chat::SendChat(wtarget.c_str(), wmsg.c_str());
    }
    else {
        GW::Chat::SendChat(channel, wmsg.c_str());
    }
}

void DropExplorerPlugin::LinkItem(const GW::Item* item, const int channel_index, const std::string& note)
{
    if (!item || !item->item_id) return;
    SetLinkerFocusedItem(item);

    auto state = linker_decode_state_;
    if (!state) return;

    state->pending_link_channel = channel_index;
    state->pending_link_note = note;
    state->pending_link_whisper = (channel_index == 5) ? linker_whisper_target_buf_ : "";

    if (state->name_done && state->stats_done) {
        const auto lines = BuildLinkerChatLines(state->name, state->stats, state->quantity, state->pending_link_note);
        const auto channel = GetChannelChar(channel_index);
        for (const auto& line : lines) {
            DispatchChatLine(channel, line, state->pending_link_whisper);
        }
    }
    else {
        state->should_send_when_ready = true;
    }
}

void DropExplorerPlugin::LinkItemById(const uint32_t item_id, const int channel_index)
{
    const auto* item = GW::Items::GetItemById(item_id);
    if (item) LinkItem(item, channel_index, linker_custom_note_buf_);
}

void DropExplorerPlugin::TriggerLinkerPendingSendIfReady(const std::shared_ptr<LinkerDecodeState>& state)
{
    if (!state || state->cancelled || !state->should_send_when_ready) return;
    if (!state->name_done || !state->stats_done) return;
    state->should_send_when_ready = false;

    GW::GameThread::Enqueue([this, state]() {
        if (state->cancelled) return;
        const auto lines = BuildLinkerChatLines(state->name, state->stats, state->quantity, state->pending_link_note);
        const auto ch = GetChannelChar(state->pending_link_channel);
        for (const auto& line : lines) {
            DispatchChatLine(ch, line, state->pending_link_whisper);
        }
    });
}

void DropExplorerPlugin::DrawItemLinkerView()
{
    if (linker_waiting_for_item_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.45f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
        if (ImGui::Button("Waiting for item (Right-click an item in bags)... [Cancel]##Linker")) {
            linker_waiting_for_item_ = false;
        }
        ImGui::PopStyleColor(2);
    }
    else {
        if (ImGui::Button("Inspect Selected Item")) {
            linker_waiting_for_item_ = true;
            auction_waiting_for_item_ = false;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled(linker_waiting_for_item_ ? "Right-click any item in your inventory to inspect" : "Press, then right-click an inventory item");

    ImGui::Separator();

    auto state = linker_decode_state_;
    if (!state || !state->item_id) {
        ImGui::TextDisabled("Click any item in your bags or equipment to inspect and link.");
        return;
    }

    const auto name = state->name_done ? StripXmlTags(state->name) : "Decoding name...";
    ImGui::TextColored(GetLinkerRarityColor(state->rarity), "%s", name.c_str());

    ImGui::TextDisabled("%s | Quantity: %u | Value: %ug",
        GetLinkerRarityName(state->rarity), state->quantity, state->value);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.7f, 0.3f, 1.0f));
    if (ImGui::Button("List in Auction House (Tab 0)")) {
        PrefillAuctionFromLinker(*state);
    }
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Item Stats & Modifiers:");
    ImGui::BeginChild("LinkerStatsBox", ImVec2(0.0f, 95.0f), true);
    if (!state->stats_done) {
        ImGui::TextDisabled("Decoding item stats...");
    }
    else if (state->stats.empty()) {
        ImGui::TextDisabled("No stats or modifiers.");
    }
    else {
        std::stringstream ss(CleanRawText(state->stats));
        std::string line;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line.find("Value:") != 0) {
                ImGui::BulletText("%s", line.c_str());
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::InputText("Note/Offer##LinkerNote", linker_custom_note_buf_, sizeof(linker_custom_note_buf_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##LinkerClear")) linker_custom_note_buf_[0] = '\0';

    const auto preview_lines = BuildLinkerChatLines(state->name_done ? state->name : "Item", state->stats, state->quantity, linker_custom_note_buf_);
    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Chat Preview (%zu line%s):",
        preview_lines.size(), preview_lines.size() == 1 ? "" : "s");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 1.0f, 1.0f));
    for (const auto& l : preview_lines) {
        ImGui::TextWrapped("%s", l.c_str());
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("One-Click Link to Channel:");

    const float btn_w = (ImGui::GetContentRegionAvail().x - 16.0f) / 5.0f;
    if (ImGui::Button("Party\n(#)##LinkParty", ImVec2(btn_w, 36.0f))) LinkItemById(state->item_id, 0);
    ImGui::SameLine();
    if (ImGui::Button("Guild\n(@)##LinkGuild", ImVec2(btn_w, 36.0f))) LinkItemById(state->item_id, 1);
    ImGui::SameLine();
    if (ImGui::Button("Trade\n($)##LinkTrade", ImVec2(btn_w, 36.0f))) LinkItemById(state->item_id, 2);
    ImGui::SameLine();
    if (ImGui::Button("All\n(!)##LinkAll", ImVec2(btn_w, 36.0f))) LinkItemById(state->item_id, 3);
    ImGui::SameLine();
    if (ImGui::Button("Ally\n(%)##LinkAlly", ImVec2(btn_w, 36.0f))) LinkItemById(state->item_id, 4);

    ImGui::Spacing();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputText("##LinkerWhisperTarget", linker_whisper_target_buf_, sizeof(linker_whisper_target_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Send Whisper##LinkerWhisper") && linker_whisper_target_buf_[0] != '\0') {
        LinkItemById(state->item_id, 5);
    }
}

bool __cdecl DropExplorerPlugin::DrawInventoryContextMenuEntry(const uint32_t item_id, const float width)
{
    const auto* item = GW::Items::GetItemById(item_id);
    if (!item || !item->bag || !item->bag->IsInventoryBag()) return true;
    auto* plugin = static_cast<DropExplorerPlugin*>(ToolboxPluginInstance());
    if (!plugin) return true;

    if (GetTickCount64() <= plugin->dismiss_inventory_context_menu_until_) {
        plugin->dismiss_inventory_context_menu_until_ = 0;
        return false;
    }

    if (plugin->linker_waiting_for_item_) {
        plugin->linker_waiting_for_item_ = false;
        plugin->SetLinkerFocusedItem(item);
        plugin->linker_focus_requested_ = true;
        plugin->current_tab_ = 1;
        return false;
    }
    if (plugin->auction_waiting_for_item_) {
        plugin->auction_waiting_for_item_ = false;
        plugin->PrefillAuctionItem(item);
        plugin->auction_focus_requested_ = true;
        plugin->current_tab_ = 0;
        return false;
    }

    if (ImGui::Button("List in Auction House", ImVec2(width, 0.0f))) {
        plugin->PrefillAuctionItem(item);
        plugin->auction_type_idx_ = 0;
        plugin->auction_focus_requested_ = true;
        plugin->current_tab_ = 0;
        if (const auto visible = plugin->GetVisiblePtr()) *visible = true;
        ImGui::SetWindowCollapsed(plugin->Name(), false);
        return false;
    }
    if (ImGui::Button("Link Item to Chat", ImVec2(width, 0.0f))) {
        plugin->LinkItem(item, 2, plugin->linker_custom_note_buf_);
        return false;
    }
    if (ImGui::Button("Inspect in Item Linker", ImVec2(width, 0.0f))) {
        plugin->SetLinkerFocusedItem(item);
        plugin->linker_focus_requested_ = true;
        plugin->current_tab_ = 1;
        if (const auto visible = plugin->GetVisiblePtr()) *visible = true;
        ImGui::SetWindowCollapsed(plugin->Name(), false);
        return false;
    }
    return true;
}

void DropExplorerPlugin::DrawAuctionHouseView()
{
    if (auction_refresh_timer_ >= 60.0f && auction_request_kind_ == AuctionRequestKind::None) RefreshAuctionListings();
    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f), "PLAYER AUCTION HOUSE");
    ImGui::TextDisabled("Browse community buy orders and sell listings");
    ImGui::Spacing();
    if (ImGui::Button(auction_new_listing_open_ ? "Close New Listing" : "New Listing")) {
        auction_new_listing_open_ = !auction_new_listing_open_;
        if (!auction_new_listing_open_) auction_waiting_for_item_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Listings")) RefreshAuctionListings();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", auction_status_.c_str());

    ImGui::BeginChild("AuctionHousePolicy", ImVec2(0.0f, 62.0f), true);
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "Player-to-player marketplace");
    ImGui::TextWrapped("Listings arrange contact only; the plugin never transfers items or currency. New orders remain local for 5 minutes before publishing. Limits: 30 sell listings and 10 buy orders per account.");
    ImGui::EndChild();

    if (auction_new_listing_open_) {
        ImGui::Spacing();
        ImGui::BeginChild("NewAuctionListing", ImVec2(0.0f, 345.0f), true);
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f), "CREATE A NEW LISTING");
        ImGui::TextDisabled("Complete the details below. Your listing enters a cancellable 5-minute local queue.");
        ImGui::Separator();

        if (auction_waiting_for_item_) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.45f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
            if (ImGui::Button("Waiting for inventory item - click to cancel##Auction")) auction_waiting_for_item_ = false;
            ImGui::PopStyleColor(2);
        }
        else if (ImGui::Button("Select From Inventory##Auction")) {
            auction_waiting_for_item_ = true;
            linker_waiting_for_item_ = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(auction_waiting_for_item_ ? "Right-click an item in your bags" : "Automatically fills the item and its modifiers");

        const char* listing_types[] = {"Sell listing", "Buy order"};
        const char* currencies[] = {"Gold", "Platinum", "Armbraces of Truth", "Globs of Ectoplasm", "Other item"};
        if (ImGui::BeginTable("NewListingFields", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Order type");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::Combo("##AuctionListingType", &auction_type_idx_, listing_types, IM_ARRAYSIZE(listing_types));
            ImGui::TextUnformatted("Item");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputTextWithHint("##AuctionItem", "Type an item name", auction_item_buf_, IM_ARRAYSIZE(auction_item_buf_))) {
                auction_show_matches_ = auction_item_buf_[0] != 0;
                auction_item_from_inventory_ = false;
                auction_item_model_id_ = 0;
            }
            if (auction_show_matches_ && auction_item_buf_[0]) {
                auto shown = 0;
                if (ImGui::BeginChild("##AuctionMatches", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 3.0f), true)) {
                    std::unordered_set<std::string> names;
                    for (const auto& item : item_index_) {
                        if (shown >= 40 || !CaseInsensitiveContains(item.item_name, auction_item_buf_) || !names.emplace(ToLower(item.item_name)).second) continue;
                        if (ImGui::Selectable(item.item_name.c_str())) {
                            PluginUtils::StrCopy(auction_item_buf_, item.item_name.c_str(), IM_ARRAYSIZE(auction_item_buf_));
                            auction_item_model_id_ = 0;
                            auction_show_matches_ = false;
                        }
                        ++shown;
                    }
                }
                ImGui::EndChild();
            }
            ImGui::TextUnformatted("Quantity");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##AuctionQuantity", &auction_quantity_);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Payment currency");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::Combo("##AuctionCurrency", &auction_currency_idx_, currencies, IM_ARRAYSIZE(currencies));
            if (auction_currency_idx_ == 4) {
                ImGui::TextUnformatted("Currency item");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##AuctionOtherCurrency", "Exact item name", auction_other_currency_buf_, IM_ARRAYSIZE(auction_other_currency_buf_));
            }
            ImGui::TextUnformatted("Price per item");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##AuctionUnitPrice", &auction_unit_price_);
            ImGui::TextUnformatted("Listing duration (hours)");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##AuctionDuration", &auction_duration_hours_);
            ImGui::EndTable();
        }

        ImGui::TextUnformatted("Item stats and modifiers");
        ImGui::InputTextMultiline("##AuctionModifiers", auction_modifiers_buf_, IM_ARRAYSIZE(auction_modifiers_buf_), ImVec2(-1.0f, 58.0f));
        ImGui::Checkbox("Publish my current character name so buyers or sellers can whisper me", &auction_publish_name_confirmed_);
        if (!auction_publish_name_confirmed_) ImGui::BeginDisabled();
        if (ImGui::Button("Queue Listing for Publication")) CreateAuctionListing();
        if (!auction_publish_name_confirmed_) ImGui::EndDisabled();
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("ACTIVE LISTINGS");
    DrawListingFilters();
    std::vector<AuctionListing> visible_listings = auction_listings_;
    visible_listings.insert(visible_listings.end(), trade_chat_listings_.begin(), trade_chat_listings_.end());
    DrawListingTable(visible_listings, false);
}

void DropExplorerPlugin::DrawMyListingsView()
{
    const auto sell_count = std::ranges::count_if(my_auction_listings_, [](const AuctionListing& listing) { return listing.listing_type == "sell"; });
    const auto buy_count = std::ranges::count_if(my_auction_listings_, [](const AuctionListing& listing) { return listing.listing_type == "buy"; });
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "My Listings");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh##MyListings")) RefreshMyListings();
    ImGui::SameLine();
    ImGui::TextDisabled("Sell: %zu / 30 | Buy: %zu / 10", sell_count, buy_count);
    ImGui::TextWrapped("These listings belong to this installation/account identity. You can cancel them from any character using this same installation; other installations cannot cancel them.");

    if (!pending_auction_listings_.empty()) {
        ImGui::SeparatorText("Pending locally");
        const auto now = static_cast<uint64_t>(std::time(nullptr));
        for (size_t i = 0; i < pending_auction_listings_.size(); ++i) {
            const auto& pending = pending_auction_listings_[i];
            const auto remaining = pending.send_at > now ? pending.send_at - now : 0;
            ImGui::PushID(static_cast<int>(i));
            ImGui::BulletText("%s: %s - %llum %02llus", pending.request.listing_type == "sell" ? "SELL" : "BUY",
                pending.request.item_name.c_str(), remaining / 60, remaining % 60);
            if (!(auction_request_kind_ == AuctionRequestKind::Create && pending_auction_inflight_index_ == i)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel pending")) {
                    pending_auction_listings_.erase(pending_auction_listings_.begin() + static_cast<ptrdiff_t>(i));
                    SavePendingAuctionListings();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    DrawListingFilters();
    if (auction_filter_type_idx_ != 1) {
        std::vector<AuctionListing> sells;
        std::ranges::copy_if(my_auction_listings_, std::back_inserter(sells), [](const AuctionListing& listing) { return listing.listing_type == "sell"; });
        ImGui::SeparatorText("Sell listings");
        ImGui::PushID("MySellListings");
        DrawListingTable(sells, true);
        ImGui::PopID();
    }
    if (auction_filter_type_idx_ != 2) {
        std::vector<AuctionListing> buys;
        std::ranges::copy_if(my_auction_listings_, std::back_inserter(buys), [](const AuctionListing& listing) { return listing.listing_type == "buy"; });
        ImGui::SeparatorText("Buy orders");
        ImGui::PushID("MyBuyListings");
        DrawListingTable(buys, true);
        ImGui::PopID();
    }
}

void DropExplorerPlugin::DrawListingFilters()
{
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##AuctionSearch", "Search item or modifiers", auction_search_buf_, IM_ARRAYSIZE(auction_search_buf_));
    const char* types[] = {"Buy and sell", "Buy orders only", "Sell listings only"};
    const char* ages[] = {"Newest to oldest", "Oldest to newest"};
    const char* currencies[] = {"All currencies", "Gold", "Platinum", "Armbraces", "Ectoplasm", "Other"};
    const char* values[] = {"Default value order", "Highest to lowest", "Lowest to highest"};
    ImGui::SameLine();
    ImGui::SetNextItemWidth(145.0f);
    ImGui::Combo("##ListingTypeFilter", &auction_filter_type_idx_, types, IM_ARRAYSIZE(types));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(145.0f);
    ImGui::Combo("##ListingAgeFilter", &auction_filter_age_idx_, ages, IM_ARRAYSIZE(ages));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::Combo("##ListingCurrencyFilter", &auction_filter_currency_idx_, currencies, IM_ARRAYSIZE(currencies));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(145.0f);
    ImGui::Combo("##ListingValueFilter", &auction_filter_value_idx_, values, IM_ARRAYSIZE(values));
}

void DropExplorerPlugin::DrawListingTable(const std::vector<AuctionListing>& listings, const bool allow_cancel)
{
    static constexpr const char* currency_ids[] = {"", "gold", "platinum", "armbrace", "ectoplasm", "other"};
    std::vector<const AuctionListing*> filtered;
    filtered.reserve(listings.size());
    for (const auto& listing : listings) {
        if (auction_filter_type_idx_ == 1 && listing.listing_type != "buy") continue;
        if (auction_filter_type_idx_ == 2 && listing.listing_type != "sell") continue;
        if (auction_filter_currency_idx_ > 0 && listing.currency_type != currency_ids[auction_filter_currency_idx_]) continue;
        if (!CaseInsensitiveContains(listing.item_name, auction_search_buf_) &&
            !CaseInsensitiveContains(listing.modifiers, auction_search_buf_)) continue;
        filtered.push_back(&listing);
    }
    std::ranges::sort(filtered, [&](const AuctionListing* lhs, const AuctionListing* rhs) {
        const auto lhs_value = lhs->sort_price > 0.0 ? lhs->sort_price : lhs->unit_price;
        const auto rhs_value = rhs->sort_price > 0.0 ? rhs->sort_price : rhs->unit_price;
        if (auction_filter_value_idx_ == 1 && lhs_value != rhs_value) return lhs_value > rhs_value;
        if (auction_filter_value_idx_ == 2 && lhs_value != rhs_value) return lhs_value < rhs_value;
        return auction_filter_age_idx_ == 1 ? lhs->created_at < rhs->created_at : lhs->created_at > rhs->created_at;
    });

    const auto table_id = allow_cancel ? "MyAuctionListings" : "AuctionListings";
    if (!ImGui::BeginTable(table_id, 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Item");
    ImGui::TableSetupColumn("Mods / Requirements");
    ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Listed", ImGuiTableColumnFlags_WidthFixed, 95.0f);
    ImGui::TableSetupColumn("Player");
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, show_messages_ ? 110.0f : 75.0f);
    ImGui::TableHeadersRow();
    for (const auto* listing : filtered) {
        ImGui::PushID(listing->listing_id.c_str());
        auto price_label = listing->price_display.empty() ? std::format("{} Gold", listing->unit_price) : listing->price_display;
        if (listing->price_display.empty() && listing->currency_type == "platinum") price_label = std::format("{} Platinum", listing->unit_price);
        else if (listing->price_display.empty() && listing->currency_type == "armbrace") price_label = std::format("{} Armbraces", listing->unit_price);
        else if (listing->price_display.empty() && listing->currency_type == "ectoplasm") price_label = std::format("{} Ectoplasm", listing->unit_price);
        else if (listing->price_display.empty() && listing->currency_type == "other") price_label = std::format("{} {}", listing->unit_price, listing->currency_item.empty() ? "Other" : listing->currency_item);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(listing->listing_type == "sell" ? "SELL" : "BUY");
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", listing->item_name.c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", listing->modifiers.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", listing->quantity);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", price_label.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatTimestamp(listing->created_at).c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", listing->seller_name.c_str());
        ImGui::TableNextColumn();
        if (allow_cancel) {
            if (ImGui::SmallButton("Cancel")) CancelAuctionListing(listing->listing_id);
        }
        else {
            if (ImGui::SmallButton("Contact")) {
                const auto message = std::format("Hi, I'm contacting you about your {} listing for {} ({} @ {} each).",
                    listing->listing_type, listing->item_name, listing->quantity, price_label);
                GW::Chat::SendChat(ToWString(listing->seller_name).c_str(), ToWString(message).c_str());
            }
            if (show_messages_ && !listing->is_trade_chat) {
                ImGui::SameLine();
                if (ImGui::SmallButton("DM")) {
                    PluginUtils::StrCopy(message_to_buf_, listing->seller_name.c_str(), IM_ARRAYSIZE(message_to_buf_));
                    const auto dm_text = std::format("Regarding your {} listing: {} ({}x @ {})",
                        listing->listing_type, listing->item_name, listing->quantity, price_label);
                    PluginUtils::StrCopy(message_content_buf_, dm_text.c_str(), IM_ARRAYSIZE(message_content_buf_));
                    message_tab_focus_requested_ = true;
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

namespace {
    std::string FormatTimestamp(const uint64_t ts)
    {
        if (!ts) return "Unknown";
        const auto t = static_cast<std::time_t>(ts);
        std::tm tm_buf{};
        if (localtime_s(&tm_buf, &t) == 0) {
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
            return buf;
        }
        return std::to_string(ts);
    }
}

void DropExplorerPlugin::LoadLocalMessages()
{
    if (settings_folder_.empty()) return;
    const auto file_path = std::filesystem::path(settings_folder_) / "messages.json";
    if (!std::filesystem::exists(file_path)) return;
    std::ifstream file(file_path);
    if (!file.is_open()) return;
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.empty()) return;

    LocalMessagesStore store;
    if (!glz::read_json(store, content)) {
        local_messages_ = std::move(store.messages);
        my_characters_ = std::move(store.my_characters);
        return;
    }

    std::vector<DirectMessage> legacy;
    if (!glz::read_json(legacy, content)) {
        local_messages_ = std::move(legacy);
    }
}

void DropExplorerPlugin::SaveLocalMessages()
{
    if (settings_folder_.empty()) return;
    const auto file_path = std::filesystem::path(settings_folder_) / "messages.json";
    LocalMessagesStore store;
    store.messages = local_messages_;
    store.my_characters = my_characters_;
    const auto payload = glz::write_json(store).value_or(std::string{});
    if (payload.empty()) return;
    std::ofstream file(file_path, std::ios::trunc);
    if (file.is_open()) {
        file << payload;
    }
}

void DropExplorerPlugin::AddMyCharacter(const std::string& name)
{
    if (name.empty()) return;
    const auto it = std::ranges::find_if(my_characters_, [&](const std::string& existing) {
        return ToLower(existing) == ToLower(name);
    });
    if (it == my_characters_.end()) {
        my_characters_.push_back(name);
        SaveLocalMessages();
    }
}

void DropExplorerPlugin::DeleteLocalMessage(const size_t index)
{
    if (index >= local_messages_.size()) return;
    local_messages_.erase(local_messages_.begin() + index);
    SaveLocalMessages();
    if (selected_message_idx_ >= static_cast<int>(local_messages_.size())) {
        selected_message_idx_ = static_cast<int>(local_messages_.size()) - 1;
    }
}

void DropExplorerPlugin::CheckIncomingMessages()
{
    if (messages_client_ && messages_client_->IsPending()) return;
    const auto base = GetServiceBaseUrl();
    if (base.empty()) return;

    const auto* player_name = GW::PlayerMgr::GetPlayerName();
    if (player_name && *player_name) {
        const auto char_name = PluginUtils::WStringToString(player_name);
        if (!char_name.empty()) {
            AddMyCharacter(char_name);
        }
    }

    if (!player_name || !*player_name) return;
    const auto recipients_param = PluginUtils::UrlEncode(PluginUtils::WStringToString(player_name));
    const auto url = std::format("{}/v1/messages?recipients={}", base, recipients_param);

    messages_client_ = std::make_unique<AsyncRestClient>();
    messages_client_->SetUrl(url.c_str());
    messages_client_->SetMethod(HttpMethod::Get);
    messages_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    messages_client_->SetConnectTimeoutSec(5);
    messages_client_->SetTimeoutSec(10);
    messages_client_->ExecuteAsync();
    message_request_kind_ = MessageRequestKind::Fetch;
}

void DropExplorerPlugin::SendDirectMessage()
{
    if (!show_messages_) return;
    if (messages_client_ && messages_client_->IsPending()) return;
    const auto base = GetServiceBaseUrl();
    const auto* player_name = GW::PlayerMgr::GetPlayerName();
    if (base.empty() || !player_name || !*player_name || !message_to_buf_[0] || !message_content_buf_[0]) {
        message_status_ = "Server connection, recipient name, and message content are required";
        return;
    }

    DirectMessageSendRequest req;
    req.sender = PluginUtils::WStringToString(player_name);
    req.recipient = StripXmlTags(message_to_buf_);
    req.content = message_content_buf_;

    const auto payload = glz::write_json(req).value_or(std::string{});
    if (payload.empty()) {
        message_status_ = "Could not encode message";
        return;
    }

    messages_client_ = std::make_unique<AsyncRestClient>();
    messages_client_->SetUrl((base + "/v1/messages").c_str());
    messages_client_->SetMethod(HttpMethod::Post);
    messages_client_->SetHeader("Content-Type", "application/json");
    messages_client_->SetUserAgent("GWToolbox-AuctionHouseDropFinder/1.0");
    messages_client_->SetPostContent(payload, ContentFlag::Copy);
    messages_client_->SetConnectTimeoutSec(5);
    messages_client_->SetTimeoutSec(10);
    messages_client_->ExecuteAsync();
    message_request_kind_ = MessageRequestKind::Send;
    message_status_ = "Sending direct message...";
}

void DropExplorerPlugin::UpdateMessageRequests(const float delta)
{
    message_check_timer_ += delta;
    if (message_request_kind_ == MessageRequestKind::None) {
        if (message_check_timer_ >= 30.0f) {
            message_check_timer_ = 0.0f;
            CheckIncomingMessages();
        }
        return;
    }

    if (!messages_client_ || !messages_client_->IsCompleted()) return;
    const auto completed_kind = message_request_kind_;
    message_request_kind_ = MessageRequestKind::None;
    const auto successful = messages_client_->IsSuccessful();

    if (completed_kind == MessageRequestKind::Fetch) {
        if (successful && show_messages_) {
            DirectMessagesFetchResponse response;
            if (!glz::read_json(response, messages_client_->GetContent())) {
                if (!response.messages.empty()) {
                    for (auto& msg : response.messages) {
                        msg.is_read = false;
                        local_messages_.insert(local_messages_.begin(), std::move(msg));
                    }
                    SaveLocalMessages();
                    message_status_ = std::format("Received {} new direct message(s)", response.messages.size());
                }
            }
        }
    }
    else if (completed_kind == MessageRequestKind::Send) {
        if (successful) {
            message_status_ = "Message sent successfully!";
            message_content_buf_[0] = '\0';
        }
        else {
            message_status_ = std::format("Failed to send message (HTTP {})", messages_client_->GetStatusCode());
        }
    }
}

void DropExplorerPlugin::DrawMessagesView()
{
    const auto* player_name_w = GW::PlayerMgr::GetPlayerName();
    const auto current_player_name = (player_name_w && *player_name_w) ? PluginUtils::WStringToString(player_name_w) : std::string{};

    if (current_player_name.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Direct Messages requires logging into a character.");
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Logged in as: %s", current_player_name.c_str());
    }

    ImGui::SameLine();
    if (ImGui::Button("Check for Messages")) {
        CheckIncomingMessages();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Status: %s", message_status_.c_str());
    ImGui::TextWrapped("Offline messages are held by the server until the addressed character logs in, then deleted from the server after delivery. The server keeps no delivered chat history.");
    ImGui::TextWrapped("Deals are at your own risk. Keep your own transaction records if needed. The service operator is not liable for deals that go sideways; buyer/seller disputes should be directed to ArenaNet.");

    ImGui::Separator();

    const auto avail = ImGui::GetContentRegionAvail();
    const float list_width = std::max(270.0f, avail.x * 0.35f);

    ImGui::BeginChild("MessagesInboxPane", ImVec2(list_width, 0.0f), true);
    ImGui::Text("Inbox (%zu)", local_messages_.size());
    ImGui::Separator();

    if (local_messages_.empty()) {
        ImGui::TextDisabled("No messages saved.");
    } else {
        for (size_t i = 0; i < local_messages_.size(); ++i) {
            auto& msg = local_messages_[i];
            ImGui::PushID(static_cast<int>(i));

            const bool is_selected = (selected_message_idx_ == static_cast<int>(i));
            std::string label = msg.is_read ? msg.sender : ("[NEW] " + msg.sender);
            if (label.empty()) label = "Unknown";

            if (ImGui::Selectable(label.c_str(), is_selected)) {
                selected_message_idx_ = static_cast<int>(i);
                if (!msg.is_read) {
                    msg.is_read = true;
                    SaveLocalMessages();
                }
            }
            ImGui::SameLine(ImGui::GetWindowWidth() - 75.0f);
            const auto ts_str = FormatTimestamp(msg.timestamp);
            ImGui::TextDisabled("%s", ts_str.size() >= 10 ? ts_str.substr(5, 5).c_str() : "");

            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (!local_messages_.empty()) {
        if (ImGui::SmallButton("Mark All Read")) {
            for (auto& msg : local_messages_) msg.is_read = true;
            SaveLocalMessages();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete All")) {
            local_messages_.clear();
            selected_message_idx_ = -1;
            SaveLocalMessages();
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("My Characters (Alts)")) {
        ImGui::TextDisabled("Characters are registered automatically when you log in and cannot be manually added or removed.");
        for (const auto& character : my_characters_) ImGui::BulletText("%s", character.c_str());
    }

    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("MessageDetailPane", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Message Details");
    if (selected_message_idx_ >= 0 && selected_message_idx_ < static_cast<int>(local_messages_.size())) {
        const auto& selected = local_messages_[selected_message_idx_];
        ImGui::Text("From: %s", selected.sender.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140.0f);
        ImGui::TextDisabled("Date: %s", FormatTimestamp(selected.timestamp).c_str());

        const bool is_to_current = !current_player_name.empty() && ToLower(selected.recipient) == ToLower(current_player_name);
        if (is_to_current) {
            ImGui::TextDisabled("To: %s", selected.recipient.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.5f, 1.0f), "To: %s (Alt Character)", selected.recipient.c_str());
        }

        ImGui::Separator();
        ImGui::BeginChild("MessageBodyText", ImVec2(0.0f, 110.0f), true);
        ImGui::TextWrapped("%s", selected.content.c_str());
        ImGui::EndChild();

        if (ImGui::SmallButton("Reply")) {
            PluginUtils::StrCopy(message_to_buf_, selected.sender.c_str(), IM_ARRAYSIZE(message_to_buf_));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete Message")) {
            DeleteLocalMessage(selected_message_idx_);
        }
    } else {
        ImGui::TextDisabled("Select a message from the list on the left to read it.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Send Direct Message (Offline Delivery)");
    ImGui::TextDisabled("Messages are queued until the addressed character logs in, then removed from the server after delivery.");

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##MsgTo", "Recipient Character Name", message_to_buf_, IM_ARRAYSIZE(message_to_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Use Target")) {
        const auto* target = GW::Agents::GetTarget();
        const auto* living = target ? target->GetAsAgentLiving() : nullptr;
        if (living && living->IsPlayer()) {
            if (const auto* wname = GW::Agents::GetPlayerNameByLoginNumber(living->login_number)) {
                const auto sname = PluginUtils::WStringToString(wname);
                if (!sname.empty()) {
                    PluginUtils::StrCopy(message_to_buf_, sname.c_str(), IM_ARRAYSIZE(message_to_buf_));
                }
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fills recipient with currently targeted player's name");
    }

    ImGui::InputTextMultiline("##MsgContent", message_content_buf_, IM_ARRAYSIZE(message_content_buf_), ImVec2(-1.0f, 90.0f));

    if (ImGui::Button("Send Direct Message")) {
        SendDirectMessage();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        message_to_buf_[0] = '\0';
        message_content_buf_[0] = '\0';
    }

    ImGui::EndChild();
}
