// lr_hits — Plugify C++ port of the Pisex LR module "[LR] ExStats Hits"
// (ref/src/cs2-lvl_ranks_modules/LR_ExStatsHits/lr_hits.cpp, 307 lines; the production plugin
// addons/lr_modules/lr_hits, vdf lr_hits.vdf).
//
// What it does (identical to the original): counts the damage, armor damage and per-hitgroup hits
// of every player_hurt into the `<lr_table>_hits` table (one row per SteamID), saves it at
// disconnect (or per hit when lr_db_savedata_player_mode is set) and zeroes it on !reset.
//
// Port notes (the rest is parity):
//   * The original hooked IServerGameClients::ClientDisconnect through SourceHook; the port
//     registers s2sdk's OnClientDisconnect_Post (the same point the levels_ranks core uses) and
//     takes the steam id from the engine with the id cached at OnPlayerLoaded as the fallback.
//   * `engine->IsClientFullyAuthenticated(attacker)` becomes pisex::g_pPlayers->IsAuthenticated -
//     the same engine call plus the "not a fake client" test s2sdk's player manager adds (bot
//     damage is not counted; the original's -1/world attacks were rejected by the engine call).
//   * The UPDATE of SaveData is built with bound parameters instead of the original's string
//     formatting; its buffer bug (the column list formatted into the uninitialized sColumns
//     buffer) is not reproduced - the columns are the same ones the flags select.
//   * The database is its own plgdb connection with the core's settings
//     (GetDatabaseConfig(), "lr.sqlite3" for the SQLite fallback); a Plugify plugin cannot share
//     another plugin's connection object. Layout and statements are the original's.

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "hits_core.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-Hits]";

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

using namespace lr_hits;

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason reason);

class LrHitsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// lr_hits::OnClientDisconnect of the original (s2sdk OnClientDisconnect_Post context).
	void OnClientDisconnect(int32_t slot);

private:
	void OnCoreIsReady();
	void OnPlayerLoaded(int slot, const char* steamid);
	void OnResetPlayerStats(int slot, const char* steamid);
	// OnPlayerHurt() of the original (pisexcompat's pre hook of "player_hurt").
	void OnPlayerHurt(pisex::IGameEvent* event);
	// SaveData() of the original; queues nothing when no hitgroup flag is set.
	void SaveData(int slot);
	// engine->GetPlayerNetworkIDString(slot) + ConvertSteamID, with the cached id as the fallback.
	std::string SteamIdOf(int slot) const;
	void ZeroSlot(SlotState& state);

	State m_state{};
	std::array<std::string, kMaxSlots> m_steamids{};
	std::unique_ptr<plgdb::Database> m_db;
	std::string m_table; // <lr_table>_hits
	bool m_disconnect_registered = false;
};

LrHitsPlugin g_plugin;

// engine->GetPlayerNetworkIDString() + ConvertSteamID() of the original: CS2's "[U:1:accountid]"
// becomes "STEAM_1:x:y" (the core's own conversion, so both write the same `steam` value); a value
// that already is a STEAM_ id is passed through, anything unusable becomes "".
std::string ConvertSteamId(std::string_view network_id) {
	const std::string text(network_id);
	if (text.empty()) {
		return {};
	}
	if (text.rfind("STEAM_", 0) == 0) {
		return text;
	}
	std::string stripped;
	stripped.reserve(text.size());
	for (const char c : text) {
		if (c != '[' && c != ']') {
			stripped.push_back(c);
		}
	}
	std::string parts[3];
	int part = 0;
	for (const char c : stripped) {
		if (c == ':') {
			if (part < 2) {
				++part;
			}
			continue;
		}
		parts[part].push_back(c);
	}
	if (parts[2].empty()) {
		return stripped; // the original crashed on such input (usteamid_split[2]); the port keeps it
	}
	int64_t account = 0;
	try {
		account = std::stoll(parts[2]); // the original's std::stoi: a 64-bit account id no longer throws
	} catch (const std::exception&) {
		return stripped;
	}
	return "STEAM_1:" + std::string(account % 2 == 0 ? "0:" : "1:") + std::to_string(account / 2);
}

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason /*reason*/) {
	try {
		g_plugin.OnClientDisconnect(slot);
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnClientDisconnect failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnClientDisconnect failed with an unknown exception", kLogTag);
	}
}

// --- database ------------------------------------------------------------------------------------

void LrHitsPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: the connection first, then the table and the hooks.
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		pisex::ErrorLog("%s the database configuration is invalid - hit statistics are off", kLogTag);
		return;
	}
	// The DDL is queued (FIFO before every later statement), like the original's async Query.
	m_db->Query(CreateTableSql(m_table, m_db->GetDriver()), {});

	g_pLRCore->HookOnPlayerLoaded([this](int slot, const char* steamid) { OnPlayerLoaded(slot, steamid); });
	g_pLRCore->HookOnResetPlayerStats(
		[this](int slot, const char* steamid) { OnResetPlayerStats(slot, steamid); });
}

void LrHitsPlugin::ZeroSlot(SlotState& state) {
	state.hits.fill(0);
	state.flags = 0;
}

void LrHitsPlugin::OnPlayerLoaded(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[static_cast<std::size_t>(slot)] = id;
	SlotState& state = m_state[static_cast<std::size_t>(slot)];
	if (slot != 0) {
		// The original's `if(iSlot)` guard around the pre-query zeroing (slot 0 only got the values
		// of the callback below).
		ZeroSlot(state);
	}
	Statement load = LoadStatement(m_table, id);
	m_db->Query(std::move(load.sql), std::move(load.params), [this, slot, id](const plgdb::Result& result) {
		SlotState& slot_state = m_state[static_cast<std::size_t>(slot)];
		const bool no_row = !result.Ok() || result.RowCount() == 0;
		ApplyLoaded(slot_state, result);
		if (no_row && m_db != nullptr) {
			// The original inserted the missing row inside the callback and loaded the zeros.
			Statement insert = InsertStatement(m_table, id);
			m_db->Query(std::move(insert.sql), std::move(insert.params));
		}
	});
}

void LrHitsPlugin::OnResetPlayerStats(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[static_cast<std::size_t>(slot)] = id;
	Statement reset = ResetStatement(m_table, id);
	// No local zeroing: the original's callback was empty and the core reloads the player right
	// after the reset (its OnPlayerLoaded runs and re-reads the zeroed row).
	m_db->Query(std::move(reset.sql), std::move(reset.params));
}

// --- game -----------------------------------------------------------------------------------------

std::string LrHitsPlugin::SteamIdOf(int slot) const {
	// engine->GetPlayerNetworkIDString(slot) in the original.
	const std::string network_id = pisex::g_pPlayers->GetSteamID(slot);
	std::string id = ConvertSteamId(network_id);
	if (id.empty() && slot >= 0 && slot < kMaxSlots) {
		id = m_steamids[static_cast<std::size_t>(slot)];
	}
	return id;
}

void LrHitsPlugin::SaveData(int slot) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	if (m_state[static_cast<std::size_t>(slot)].flags == 0) {
		return; // the original's `if(iFlags)` guard
	}
	const std::string id = SteamIdOf(slot);
	if (id.empty()) {
		// The original wrote the row with an empty steam id (matching nothing); the port keeps the
		// counters for the next save instead.
		pisex::ErrorLog("%s no steam id for slot %d - the hit statistics were not saved", kLogTag, slot);
		return;
	}
	Statement statement = SaveStatement(m_table, m_state[static_cast<std::size_t>(slot)], id);
	if (statement.sql.empty()) {
		return;
	}
	m_db->Query(std::move(statement.sql), std::move(statement.params));
}

void LrHitsPlugin::OnPlayerHurt(pisex::IGameEvent* event) {
	// The statistic gate first (both checks are side-effect free; the original ordered them the
	// other way round): while the gate is closed - warmup, fewer than lr_minplayers_count players -
	// every player_hurt costs nothing at all instead of an engine authentication call.
	if (event == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	// OnPlayerHurt of the original.
	const int attacker = event->GetInt("attacker");
	if (attacker < 0 || attacker >= kMaxSlots) {
		// The original indexed g_iHits[iAttacker] after only the engine check; out-of-range
		// attackers (world damage) are dropped here instead of writing out of bounds.
		return;
	}
	if (!pisex::g_pPlayers->IsAuthenticated(attacker)) {
		return;
	}
	ApplyHit(m_state[static_cast<std::size_t>(attacker)], event->GetInt("dmg_health"),
			 event->GetInt("dmg_armor"), event->GetInt("hitgroup"));
	if (g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode)) {
		SaveData(attacker);
	}
}

void LrHitsPlugin::OnClientDisconnect(int32_t slot) {
	//     if(!g_pLRCore->GetSettingsValue(LR_DB_SaveDataPlayer_Mode)) SaveData(slot.Get());
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (!g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode)) {
		SaveData(static_cast<int>(slot));
	}
	m_steamids[static_cast<std::size_t>(slot)].clear(); // the slot is free again
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrHitsPlugin::OnPluginStart() {
	try {
		pisex::Init("levels_ranks");

		// SH_ADD_HOOK_MEMFUNC(IServerGameClients, ClientDisconnect, ...) of the original.
		if (__s2sdk_OnClientDisconnect_Post_Register != nullptr) {
			s2sdk::OnClientDisconnect_Post_Register(&OnClientDisconnectListener);
			m_disconnect_registered = true;
		}

		// AllPluginsLoaded() of the original: the event hook first, then the LR core lookup.
		pisex::g_pUtils->HookEvent("player_hurt",
								   [](const char*, pisex::IGameEvent* event, bool) { g_plugin.OnPlayerHurt(event); });

		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginStart failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrHitsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll();
		if (m_db != nullptr) {
			m_db->Poll();
		}
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{};
}

plg::PluginResult LrHitsPlugin::OnPluginEnd() {
	try {
		if (m_disconnect_registered && __s2sdk_OnClientDisconnect_Post_Unregister != nullptr) {
			s2sdk::OnClientDisconnect_Post_Unregister(&OnClientDisconnectListener);
			m_disconnect_registered = false;
		}
		g_pLRCore->UnhookOnPlayerLoaded();
		g_pLRCore->UnhookOnResetPlayerStats();
		g_pLRCore->UnhookOnCoreIsReady();
		pisex::Shutdown();
		m_db.reset();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
