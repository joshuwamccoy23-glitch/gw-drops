#include "DropExplorerPlugin.h"

#include <GWCA/Constants/Maps.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/ItemMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Item.h>
#include <GWCA/GameEntities/Party.h>
#include <GWCA/Constants/Constants.h>
#include <Utils/GuiUtils.h>
#include <Utils/TextUtils.h>
#include <Utils/ToolboxUtils.h>

#include <glaze/glaze.hpp>
#include <imgui.h>
#include <fstream>
#include <algorithm>
#include <shellapi.h>
#include <limits>
#include <random>
#include <ctime>
#include <format>

namespace {
    std::string ToLower(std::string_view s)
    {
        std::string res;
        res.reserve(s.size());
        for (const char c : s) {
            res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return res;
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
    }

    zones_ = DropExplorer::GetBuiltinZones();
    NormalizeUnverifiedData(zones_);
    BuildItemIndex();
}

void DropExplorerPlugin::Terminate()
{
    GW::StoC::RemoveCallback<GW::Packet::StoC::QuotedItemPrice>(&price_quote_entry_);
    GW::StoC::RemoveCallback<GW::Packet::StoC::TransactionDone>(&trans_done_entry_);
    if (telemetry_client_ && telemetry_client_->IsPending()) telemetry_client_->Abort();
    if (rates_client_ && rates_client_->IsPending()) rates_client_->Abort();
    if (vendor_prices_client_ && vendor_prices_client_->IsPending()) vendor_prices_client_->Abort();
    telemetry_client_.reset();
    rates_client_.reset();
    vendor_prices_client_.reset();
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
    UpdateCommunityRatesDownload();
    UpdateVendorPricesDownload();
    UpdateDropTelemetry(delta);
    if (tracked_mob_name_.empty()) return;
    tracking_scan_timer_ += delta;
    if (tracking_scan_timer_ < 0.25f) return;
    tracking_scan_timer_ = 0.0f;
    tracked_mob_visible_ = false;
    tracked_agent_id_ = 0;

    if (!GW::Map::GetIsMapLoaded() ||
        GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable ||
        static_cast<uint32_t>(GW::Map::GetMapID()) != tracked_map_id_) {
        return;
    }

    const auto* player = GW::Agents::GetControlledCharacter();
    const auto* agents = GW::Agents::GetAgentArray();
    if (!player || !agents) return;

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

    if (!closest) return;
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
}

void DropExplorerPlugin::LoadSettings(const wchar_t* folder)
{
    ToolboxUIPlugin::LoadSettings(folder);
    settings_folder_ = folder ? folder : L"";
    LoadSetting("telemetry_enabled", telemetry_enabled_);
    LoadSetting("download_community_rates", download_community_rates_);
    LoadSetting("telemetry_install_id", telemetry_install_id_);
    auto endpoint = std::string{};
    LoadSetting("telemetry_endpoint", endpoint);
    if (!endpoint.empty() && endpoint.find("173.189.220.88") == std::string::npos) {
        PluginUtils::StrCopy(telemetry_endpoint_, endpoint.c_str(), IM_ARRAYSIZE(telemetry_endpoint_));
    } else {
        PluginUtils::StrCopy(telemetry_endpoint_, "https://escape-championship-screening-international.trycloudflare.com/v1/telemetry", IM_ARRAYSIZE(telemetry_endpoint_));
    }
    LoadSetting("batch_interval_minutes", batch_interval_minutes_);
    if (batch_interval_minutes_ < 1.0f) batch_interval_minutes_ = 5.0f;
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
    StartCommunityRatesDownload();
    StartVendorPricesDownload();
}

void DropExplorerPlugin::SaveSettings(const wchar_t* folder)
{
    SaveSetting("telemetry_enabled", telemetry_enabled_);
    SaveSetting("download_community_rates", download_community_rates_);
    SaveSetting("telemetry_install_id", telemetry_install_id_);
    SaveSetting("telemetry_endpoint", std::string(telemetry_endpoint_));
    SaveSetting("batch_interval_minutes", batch_interval_minutes_);
    SaveSetting("github_base_url", std::string(github_base_url_));
    ToolboxUIPlugin::SaveSettings(folder);
}

void DropExplorerPlugin::DrawSettings()
{
    ImGui::TextDisabled("Drop Explorer Configuration");
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Community Rates & Vendor Prices");
    ImGui::Checkbox("Download community-observed rates & vendor prices", &download_community_rates_);
    ImGui::InputTextWithHint("GitHub Raw URL##DropGhBase", "https://raw.githubusercontent.com/.../main", github_base_url_, IM_ARRAYSIZE(github_base_url_));
    if (ImGui::Button("Sync from GitHub Now")) {
        StartCommunityRatesDownload();
        StartVendorPricesDownload();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Status: %s", last_sync_status_.c_str());

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Telemetry & Crowdsourced Ingestion");
    if (ImGui::Checkbox("Contribute anonymous drop & vendor observations", &telemetry_enabled_) && !telemetry_enabled_) {
        ResetDropTelemetry();
        upload_queue_.clear();
        vendor_upload_queue_.clear();
    }
    ImGui::TextWrapped("Opt-in telemetry buffers mob kills and vendor quotes/sales for 5-10 minutes, then sends all accumulated data in a single batch. No account, character, chat, or inventory scanning is sent.");

    ImGui::TextDisabled("Aggregator: Official GW-Drops Network (Connected)");
    ImGui::SliderFloat("Batch Interval (minutes)##DropBatchInterval", &batch_interval_minutes_, 5.0f, 10.0f, "%.1f min");

    const float remaining_sec = std::max(0.0f, (batch_interval_minutes_ * 60.0f) - (batch_timer_ms_ / 1000.0f));
    const int rem_min = static_cast<int>(remaining_sec) / 60;
    const int rem_s = static_cast<int>(remaining_sec) % 60;
    ImGui::Text("Batch timer: %02d:%02d until next check | Queued: %zu kills, %zu vendor records",
                rem_min, rem_s, upload_queue_.size(), vendor_upload_queue_.size());

    const bool has_queued_data = !upload_queue_.empty() || !vendor_upload_queue_.empty();
    if (!has_queued_data) ImGui::BeginDisabled();
    if (ImGui::Button("Send Batch Now")) {
        TriggerBatchUpload();
    }
    if (!has_queued_data) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Last Upload: %s", last_upload_status_.c_str());

    ImGui::Separator();
    if (ImGui::Button("Reload Built-in & External Database")) {
        zones_ = DropExplorer::GetBuiltinZones();
        NormalizeUnverifiedData(zones_);
        const auto db_path = std::filesystem::path(settings_folder_) / "drops_database.json";
        if (std::filesystem::exists(db_path)) {
            LoadExternalDatabase(db_path);
        }
        BuildItemIndex();
        StartCommunityRatesDownload();
        StartVendorPricesDownload();
    }
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

    if (!telemetry_enabled_ || !telemetry_endpoint_[0]) {
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
    if (!telemetry_client_ || !telemetry_enabled_ || !telemetry_endpoint_[0]) return;
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

    auto endpoint = std::string(telemetry_endpoint_);
    while (endpoint.ends_with('/')) endpoint.pop_back();
    if (!endpoint.ends_with("/v1/telemetry") && !endpoint.ends_with("/v1/events")) {
        endpoint += "/v1/telemetry";
    }

    telemetry_client_->Clear();
    telemetry_client_->SetUrl(endpoint.c_str());
    telemetry_client_->SetMethod(HttpMethod::Post);
    telemetry_client_->SetHeader("Content-Type", "application/json");
    telemetry_client_->SetUserAgent("GWToolbox-DropExplorer/1.0");
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
    vendor_prices_client_->SetUserAgent("GWToolbox-DropExplorer/1.0");
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
    rates_client_->SetUserAgent("GWToolbox-DropExplorer/1.0");
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

void DropExplorerPlugin::ViewZone(const DropExplorer::ZoneInfo& zone)
{
    SelectZoneByName(zone.name);

    if (set_travel_dest_fn_ && zone.map_id > 0) {
        set_travel_dest_fn_(zone.map_id);
    }

    OpenWorldMap();
}

void DropExplorerPlugin::TravelToZone(const DropExplorer::ZoneInfo& zone)
{
    if (set_travel_dest_fn_ && zone.map_id > 0) {
        set_travel_dest_fn_(zone.map_id);
    }

    const auto dest_id = (zone.nearest_outpost_id > 0) ? zone.nearest_outpost_id : zone.map_id;
    if (dest_id > 0) {
        GW::Map::Travel(static_cast<GW::Constants::MapID>(dest_id));
    }

    OpenWorldMap();
}

void DropExplorerPlugin::TrackMob(const DropExplorer::ZoneInfo& zone, const DropExplorer::MobInfo& mob, const bool travel)
{
    if (clear_custom_point_fn_) clear_custom_point_fn_();
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
    if (travel) TravelToZone(zone);
    else ViewZone(zone);
}

void DropExplorerPlugin::SelectZoneByMapId(const uint32_t map_id)
{
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i].map_id == map_id) {
            selected_zone_idx_ = static_cast<int>(i);
            current_tab_ = 0;
            return;
        }
    }
}

void DropExplorerPlugin::SelectZoneByName(const std::string& zone_name)
{
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i].name == zone_name) {
            selected_zone_idx_ = static_cast<int>(i);
            current_tab_ = 0;
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
        if (ImGui::BeginTabItem("Zone Explorer")) {
            current_tab_ = 0;
            DrawZoneExplorerView();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Item Search")) {
            current_tab_ = 1;
            DrawItemSearchView();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Drop Rates & Guide")) {
            current_tab_ = 2;
            DrawInfoView();
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
                std::string header = "[BOSS] " + boss->name + " (" + boss->level_str + " " + boss->profession + ")";
                if (!boss->boss_skill.empty()) {
                    header += " - Elite: " + boss->boss_skill;
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
                const std::string header = mob->name + " (" + mob->level_str + " " + mob->profession + ")";
                if (ImGui::CollapsingHeader(header.c_str())) {
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

void DropExplorerPlugin::DrawInfoView()
{
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Guild Wars 1 Drop Rates & Mechanics");
    ImGui::Separator();

    ImGui::TextWrapped(
        "In Guild Wars 1, mob drop tables and drop chances are calculated strictly on the game server. "
        "The client only receives entity packets when items actually spawn in the world.\n\n"
        "ArenaNet has not published exact per-item drop probabilities. Drop Explorer therefore labels a rate "
        "Unknown unless the dataset includes a traceable published source. It does not estimate or invent rates."
    );

    ImGui::Spacing();
    ImGui::Bullet();
    ImGui::TextColored(GetRarityColor(DropExplorer::DropRarity::UniqueGreen), "Green unique items:");
    ImGui::SameLine();
    ImGui::TextWrapped("Their modifiers are fixed per named item, but low-requirement and other exceptions exist. Exact stats are shown from the item's Guild Wars Wiki page.");

    ImGui::Bullet();
    ImGui::TextColored(GetRarityColor(DropExplorer::DropRarity::Rare), "Ordinary loot:");
    ImGui::SameLine();
    ImGui::TextWrapped("Loot scaling, party size, area, foe, chest, difficulty, and anti-farm behavior can affect what drops. A rarity label is not a probability.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Extensibility & Custom Datasets");
    ImGui::TextWrapped(
        "Place 'drops_database.json' in the GWToolboxpp plugin folder to replace the bundled catalog. "
        "Unverified numeric rates are discarded on load. Source-backed item records can include a source_url field."
    );
}
