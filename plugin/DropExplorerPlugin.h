#pragma once

#include <ToolboxUIPlugin.h>
#include <PluginUtils.h>
#include "DropExplorerData.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <d3d9.h>
#include <RestClient.h>
#include <GWCA/Utilities/Hook.h>
#include <GWCA/Packets/StoC.h>

namespace GW {
    struct Agent;
    struct Item;
    namespace UI::UIPacket { struct kMouseAction; }
}

class DropExplorerPlugin : public ToolboxUIPlugin {
public:
    enum class SortMode : uint8_t {
        NameAsc = 0,
        NameDesc,
        ChanceDesc,
        ChanceAsc,
        ZoneAsc,
        RarityDesc
    };

    enum class LinkerRarity : uint8_t {
        White,
        Blue,
        Purple,
        Gold,
        Green,
        Unknown
    };

    struct LinkerDecodeState {
        uint32_t item_id = 0;
        uint32_t model_id = 0;
        uint32_t quantity = 1;
        uint32_t value = 0;
        GW::Constants::ItemType type = GW::Constants::ItemType::Unknown;
        LinkerRarity rarity = LinkerRarity::White;

        std::string name;
        std::string stats;
        bool name_done = false;
        bool stats_done = false;
        bool cancelled = false;

        bool should_send_when_ready = false;
        int pending_link_channel = 2;
        std::string pending_link_note;
        std::string pending_link_whisper;
    };

    DropExplorerPlugin() = default;
    ~DropExplorerPlugin() override = default;

    [[nodiscard]] const char* Name() const override { return "AuctionHouse&DropFinder"; }
    [[nodiscard]] const char* Icon() const override { return "\xef\x80\x82"; }
    [[nodiscard]] bool HasSettings() const override { return true; }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;
    void Terminate() override;
    void LoadSettings(const wchar_t* folder) override;
    void SaveSettings(const wchar_t* folder) override;
    void DrawSettings() override;
    void Draw(IDirect3DDevice9* device) override;
    void Update(float delta) override;
    bool WndProc(UINT message, WPARAM w_param, LPARAM l_param) override;

    void SelectZoneByMapId(uint32_t map_id);
    void SelectZoneByName(const std::string& zone_name);
    void ViewZone(const DropExplorer::ZoneInfo& zone);
    void TravelToZone(const DropExplorer::ZoneInfo& zone);
    void OpenWorldMap();

private:
    struct TelemetryDrop {
        uint32_t item_model_id = 0;
        std::string item_name;
        uint32_t item_type = 0;
        uint32_t rarity = 0;
    };

    struct TelemetryEvent {
        std::string event_id;
        uint64_t observed_at = 0;
        uint32_t map_id = 0;
        bool hard_mode = false;
        uint32_t party_size = 1;
        uint32_t mob_model_id = 0;
        std::string mob_name;
        std::string confidence = "confirmed";
        std::vector<TelemetryDrop> drops;
    };

    struct TelemetryVendorEvent {
        std::string transaction_id;
        uint64_t observed_at = 0;
        uint32_t map_id = 0;
        std::string vendor_type;
        std::string transaction_type;
        uint32_t item_model_id = 0;
        std::string item_name;
        uint32_t unit_price = 0;
        uint32_t quantity = 1;
    };

    struct TelemetryEnvelope {
        uint32_t schema_version = 2;
        std::string install_id;
        std::vector<TelemetryEvent> events;
        std::vector<TelemetryVendorEvent> vendor_events;
    };

    struct ObservedMob {
        uint32_t model_id = 0;
        std::string name;
        float x = 0.0f;
        float y = 0.0f;
        bool alive = false;
    };

    struct PendingKill {
        TelemetryEvent event;
        uint64_t death_ms = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct ObservedItem {
        uint32_t item_id = 0;
        uint32_t model_id = 0;
        uint32_t item_type = 0;
        uint32_t rarity = 0;
        uint32_t owner_id = 0;
        uint64_t first_seen_ms = 0;
        float x = 0.0f;
        float y = 0.0f;
        bool handled = false;
        std::unique_ptr<PluginUtils::EncString> name;
    };

    struct CommunityRate {
        uint32_t map_id = 0;
        bool hard_mode = false;
        uint32_t mob_model_id = 0;
        std::string mob_name;
        uint32_t item_model_id = 0;
        std::string item_name;
        uint32_t eligible_kills = 0;
        uint32_t confirmed_drops = 0;
        uint32_t sample_window = 0;
        std::string confidence;
        double rate_pct = 0.0;
        double confidence_low_pct = 0.0;
        double confidence_high_pct = 0.0;
    };

    struct CommunityRatesDocument {
        uint32_t schema_version = 0;
        std::string generated_at;
        std::vector<CommunityRate> records;
    };

    struct VendorPriceRecord {
        uint32_t item_model_id = 0;
        std::string item_name;
        std::string vendor_type;
        std::string transaction_type;
        uint32_t sample_count = 0;
        double avg_price = 0.0;
        uint32_t min_price = 0;
        uint32_t max_price = 0;
        uint64_t last_observed_at = 0;
    };

    struct VendorPricesDocument {
        uint32_t schema_version = 0;
        std::string generated_at;
        uint32_t record_count = 0;
        std::vector<VendorPriceRecord> prices;
    };

    struct AuctionListing {
        std::string listing_id;
        std::string seller_name;
        std::string listing_type;
        std::string item_name;
        uint32_t item_model_id = 0;
        uint32_t quantity = 1;
        uint32_t unit_price = 0;
        std::string currency_type = "gold";
        std::string currency_item;
        std::string notes;
        std::string modifiers;
        uint64_t created_at = 0;
        uint64_t expires_at = 0;
        uint64_t publish_at = 0;
        bool is_pending = false;
        uint32_t seconds_until_public = 0;
        bool is_trade_chat = false;
        std::string price_display;
        double sort_price = 0.0;
    };

    struct TradeChatMessage {
        std::string sender;
        std::string message;
        uint64_t timestamp = 0;
    };

    struct AuctionListingsDocument {
        uint32_t schema_version = 0;
        std::string generated_at;
        std::vector<AuctionListing> listings;
    };

    struct AuctionListingRequest {
        std::string install_id;
        std::string seller_name;
        std::string listing_type;
        std::string item_name;
        uint32_t item_model_id = 0;
        uint32_t quantity = 1;
        uint32_t unit_price = 0;
        std::string currency_type = "gold";
        std::string currency_item;
        std::string notes;
        std::string modifiers;
        uint32_t duration_hours = 24;
    };

    struct PendingAuctionListing {
        AuctionListingRequest request;
        uint64_t send_at = 0;
    };

    struct PendingAuctionStore {
        std::vector<PendingAuctionListing> listings;
    };

    enum class AuctionRequestKind : uint8_t {
        None,
        Refresh,
        RefreshMine,
        Create,
        Cancel
    };

    struct DirectMessage {
        std::string id;
        std::string sender;
        std::string recipient;
        std::string content;
        uint64_t timestamp = 0;
        bool is_read = false;
    };

    struct DirectMessagesFetchResponse {
        std::vector<DirectMessage> messages;
    };

    struct DirectMessageSendRequest {
        std::string sender;
        std::string recipient;
        std::string content;
    };

    struct LocalMessagesStore {
        std::vector<DirectMessage> messages;
        std::vector<std::string> my_characters;
    };

    enum class MessageRequestKind : uint8_t {
        None,
        Fetch,
        Send
    };


    void BuildItemIndex();
    void SortItemIndex();
    void LoadExternalDatabase(const std::filesystem::path& path);
    void DrawAuctionHouseView();
    void DrawMyListingsView();
    void DrawListingFilters();
    void DrawListingTable(const std::vector<AuctionListing>& listings, bool allow_cancel);
    void DrawItemLinkerView();
    void DrawZoneExplorerView();
    void DrawItemSearchView();
    void DrawMessagesView();
    void CheckIncomingMessages();
    void SendDirectMessage();
    void UpdateMessageRequests(float delta);
    void LoadLocalMessages();
    void SaveLocalMessages();
    void DeleteLocalMessage(size_t index);
    void AddMyCharacter(const std::string& name);
    void TrackMob(const DropExplorer::ZoneInfo& zone, const DropExplorer::MobInfo& mob, bool travel);
    bool RefreshTrackedMobMarker();
    void CenterWorldMapOnZone(uint32_t map_id);
    std::string GetResolvedAgentName(uint32_t agent_id, const GW::Agent* agent);
    void UpdateDropTelemetry(float delta);
    void ResetDropTelemetry();
    void StartCommunityRatesDownload();
    void UpdateCommunityRatesDownload();
    void ApplyCommunityRates(const CommunityRatesDocument& document);
    void StartVendorPricesDownload();
    void UpdateVendorPricesDownload();
    void ApplyVendorPrices(const VendorPricesDocument& document);
    void RefreshAuctionListings();
    void RefreshMyListings();
    void StartTradeChatFeed();
    void UpdateTradeChatListings();
    void IngestTradeChatMessage(const TradeChatMessage& message);
    void CreateAuctionListing();
    void SendPendingAuctionListing();
    void LoadPendingAuctionListings();
    void SavePendingAuctionListings();
    void CancelAuctionListing(const std::string& listing_id);
    void UpdateAuctionRequest(float delta);
    std::string GetServiceBaseUrl() const;
    void PrefillAuctionItem(const GW::Item* item);
    void PrefillAuctionFromLinker(const LinkerDecodeState& state);
    void SetLinkerFocusedItem(const GW::Item* item);
    void LinkItem(const GW::Item* item, int channel_index, const std::string& note = "");
    void LinkItemById(uint32_t item_id, int channel_index);
    void DispatchChatLine(char channel, const std::string& message, const std::string& whisper_target);
    std::vector<std::string> BuildLinkerChatLines(const std::string& name, const std::string& stats, uint32_t quantity, const std::string& note);
    void TriggerLinkerPendingSendIfReady(const std::shared_ptr<LinkerDecodeState>& state);
    static bool __cdecl DrawInventoryContextMenuEntry(uint32_t item_id, float width);
    void TriggerBatchUpload();
    void DrawItemTooltip(const std::string& name,
                         const std::string& category,
                         DropExplorer::DropRarity rarity,
                         const std::string& chance_str,
                         const std::string& stats,
                         const std::string& description,
                         const std::string& notes,
                         const std::string& source_url,
                         uint32_t model_id = 0);

    // Data cache
    std::vector<DropExplorer::ZoneInfo> zones_;
    std::vector<DropExplorer::ItemLookupResult> item_index_;

    // Function pointers to Toolbox exports
    using GetItemImageByName_pt = IDirect3DTexture9** (__cdecl*)(const wchar_t*);
    using SetTravelDestinationMapId_pt = bool (__cdecl*)(uint32_t);
    using SetCustomTravelPointGame_pt = bool (__cdecl*)(float, float);
    using ClearCustomTravelPoint_pt = void (__cdecl*)();
    using InventoryContextMenuCallback_pt = bool(__cdecl*)(uint32_t, float);
    using InventoryAddContextMenuCallback_pt = void(__cdecl*)(InventoryContextMenuCallback_pt);
    using InventoryRemoveContextMenuCallback_pt = void(__cdecl*)(InventoryContextMenuCallback_pt);

    GetItemImageByName_pt get_item_image_fn_ = nullptr;
    SetTravelDestinationMapId_pt set_travel_dest_fn_ = nullptr;
    SetCustomTravelPointGame_pt set_custom_point_fn_ = nullptr;
    ClearCustomTravelPoint_pt clear_custom_point_fn_ = nullptr;
    InventoryAddContextMenuCallback_pt inventory_add_context_menu_fn_ = nullptr;
    InventoryRemoveContextMenuCallback_pt inventory_remove_context_menu_fn_ = nullptr;

    std::unordered_map<uint32_t, std::unique_ptr<PluginUtils::EncString>> agent_names_cache_;
    std::string tracked_mob_name_;
    uint32_t tracked_map_id_ = 0;
    uint32_t tracked_agent_id_ = 0;
    float tracking_scan_timer_ = 0.0f;
    bool tracked_mob_visible_ = false;
    bool tracked_marker_set_ = false;
    float tracked_marker_x_ = 0.0f;
    float tracked_marker_y_ = 0.0f;
    uint32_t navigation_target_map_id_ = 0;
    float navigation_refresh_timer_ = 0.0f;
    std::string navigation_status_;
    std::string highlighted_mob_name_;
    bool zone_explorer_focus_requested_ = false;

    std::unordered_map<uint32_t, ObservedMob> observed_mobs_;
    std::unordered_map<uint32_t, ObservedItem> observed_items_;
    std::vector<PendingKill> pending_kills_;
    std::vector<TelemetryEvent> upload_queue_;
    std::unique_ptr<AsyncRestClient> telemetry_client_;
    std::unique_ptr<AsyncRestClient> rates_client_;
    uint32_t telemetry_map_id_ = 0;
    size_t telemetry_inflight_count_ = 0;
    float telemetry_scan_timer_ = 0.0f;
    uint64_t telemetry_next_retry_ms_ = 0;
    bool telemetry_enabled_ = true;
    bool download_community_rates_ = true;
    bool show_auction_house_ = true;
    bool show_my_listings_ = true;
    bool show_messages_ = true;
    bool show_zone_explorer_ = true;
    bool show_item_search_ = true;
    bool crowdsourced_data_disabled_ = false;
    bool rates_request_started_ = false;
    bool vendor_prices_request_started_ = false;
    std::string telemetry_install_id_;
    std::string service_base_url_;
    std::unique_ptr<AsyncRestClient> discovery_client_;
    uint64_t discovery_next_ms_ = 0;
    void UpdateServiceDiscovery();
    char github_base_url_[256] = "https://raw.githubusercontent.com/joshuwamccoy23-glitch/gw-drops/main";

    float batch_interval_minutes_ = 0.5f;
    float batch_timer_ms_ = 0.0f;

    std::vector<TelemetryVendorEvent> vendor_upload_queue_;
    size_t vendor_inflight_count_ = 0;
    std::unique_ptr<AsyncRestClient> vendor_prices_client_;
    std::unique_ptr<AsyncRestClient> auction_client_;
    std::unordered_map<uint32_t, std::vector<VendorPriceRecord>> vendor_prices_by_model_id_;
    std::unordered_map<std::string, std::vector<VendorPriceRecord>> vendor_prices_by_name_;
    TelemetryVendorEvent last_quoted_event_;
    uint64_t last_quoted_ms_ = 0;

    GW::HookEntry price_quote_entry_;
    GW::HookEntry trans_done_entry_;
    GW::HookEntry item_click_entry_;
    void OnPriceQuote(const GW::Packet::StoC::QuotedItemPrice* packet);
    void OnTransactionDone(const GW::Packet::StoC::TransactionDone* packet);

    std::string last_upload_status_ = "None";
    std::string last_sync_status_ = "None";
    std::vector<AuctionListing> auction_listings_;
    std::vector<AuctionListing> my_auction_listings_;
    std::vector<AuctionListing> trade_chat_listings_;
    std::vector<TradeChatMessage> trade_chat_inbox_;
    std::mutex trade_chat_mutex_;
    std::jthread trade_chat_thread_;
    std::atomic_bool trade_chat_enabled_ = false;
    std::atomic_bool trade_chat_connected_ = false;
    std::vector<PendingAuctionListing> pending_auction_listings_;
    size_t pending_auction_inflight_index_ = static_cast<size_t>(-1);
    AuctionRequestKind auction_request_kind_ = AuctionRequestKind::None;
    float auction_refresh_timer_ = 60.0f;
    std::string auction_status_ = "Not synced";
    char auction_search_buf_[128] = "";
    int auction_filter_type_idx_ = 0;
    int auction_filter_age_idx_ = 0;
    int auction_filter_currency_idx_ = 0;
    int auction_filter_value_idx_ = 0;
    char auction_item_buf_[160] = "";
    char auction_modifiers_buf_[1001] = "";
    char auction_other_currency_buf_[161] = "";
    int auction_type_idx_ = 0;
    int auction_currency_idx_ = 0;
    int auction_quantity_ = 1;
    int auction_unit_price_ = 0;
    int auction_duration_hours_ = 24;
    bool auction_publish_name_confirmed_ = false;
    bool auction_new_listing_open_ = false;
    bool auction_focus_requested_ = false;
    bool auction_show_matches_ = false;
    bool auction_item_from_inventory_ = false;
    uint32_t auction_item_model_id_ = 0;
    std::unique_ptr<PluginUtils::EncString> auction_item_name_decoder_;
    std::unique_ptr<PluginUtils::EncString> auction_item_details_decoder_;

    // Item Linker tab state
    std::shared_ptr<LinkerDecodeState> linker_decode_state_;
    char linker_custom_note_buf_[128] = "";
    char linker_whisper_target_buf_[64] = "";
    int linker_selected_channel_idx_ = 2;
    bool linker_focus_requested_ = false;
    bool linker_waiting_for_item_ = false;
    bool auction_waiting_for_item_ = false;
    uint32_t pending_right_click_item_id_ = 0;
    uint64_t dismiss_inventory_context_menu_until_ = 0;

    // UI state
    int current_tab_ = 0;
    int selected_campaign_idx_ = 0;
    int selected_zone_idx_ = 0;
    SortMode current_sort_mode_ = SortMode::NameAsc;

    char zone_search_buf_[128] = "";
    char item_search_buf_[128] = "";
    int item_category_filter_idx_ = 0;

    // Direct Messages tab state
    std::vector<DirectMessage> local_messages_;
    std::vector<std::string> my_characters_;
    std::unique_ptr<AsyncRestClient> messages_client_;
    MessageRequestKind message_request_kind_ = MessageRequestKind::None;
    float message_check_timer_ = 0.0f;
    std::string message_status_ = "Ready";
    char message_to_buf_[64] = "";
    char message_content_buf_[512] = "";
    int selected_message_idx_ = -1;
    bool message_tab_focus_requested_ = false;

    std::wstring settings_folder_;
};
