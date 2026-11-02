// lr_hits — Plugify C++ port of the Pisex LR module "[LR] ExStats Hits"
// (the original's lr_hits.cpp; the Metamod module ships as addons/lr_modules/lr_hits with
// lr_hits.vdf).
//
// What it does (identical to the original): counts the damage, armor damage and per-hitgroup hits
// of every player_hurt into the `<lr_table>_hits` table (one row per SteamID), saves it at
// disconnect (or per hit when lr_db_savedata_player_mode is set) and zeroes it on !reset.
//
// This plugin calls s2sdk directly - no pisexcompat, no guard library (docs/PLUGIN_STANDARD.md §7).
// The safety rules of handoff/weaponskins/PITFALLS.md therefore live right here:
//   * B1/B2 map gate: the OnMapStart/OnMapEnd listeners are registered in OnPluginStart and the
//     gate g_mapReady is opened by OnMapStart (or by the first OnPluginUpdate when the plugin was
//     loaded into a running map) and dropped by OnMapEnd. IsAuthenticated() and ClientSteamId2()
//     refuse a closed gate before they touch s2sdk, so no client query can reach a torn-down client
//     list; OnPluginEnd removes the listeners and closes the gate again (A7).
//   * C2 client checks: 0 <= slot < 64, IsClientConnected, IsClientInGame, !IsClientSourceTV,
//     !IsFakeClient, IsClientAuthorized. Every s2sdk binding slot is checked for null as well,
//     because the generated wrappers call theirs unchecked.
//   * C12/C13: "player_hurt" is hooked with s2sdk::HookEvent(..., HookMode::Pre) in OnPluginStart
//     and unhooked with s2sdk::UnhookEvent in OnPluginEnd. The fields are read with s2sdk while the
//     event is alive (nothing is cached across the event) and the callback answers Continue on
//     every path: a Pre hook result >= Handled would free the event for every other plugin. The
//     attacker is s2sdk::GetEventPlayerSlot (the field is a player_controller_and_pawn one), the
//     damage and hitgroup are s2sdk::GetEventInt.
//
// Port notes (the rest is parity):
//   * `engine->IsClientFullyAuthenticated(attacker)` becomes IsAuthenticated(): the same engine
//     call (s2sdk::IsClientAuthorized) plus the "not a fake client" test the old player manager
//     added (bot damage is not counted; the original's -1/world attacks were rejected by the engine
//     call). The map/slot checks pisexcompat's Ready()/ClientExists() did are part of both gates.
//   * engine->GetPlayerNetworkIDString(slot) + ConvertSteamID() becomes ClientSteamId2() +
//     NormalizeSteamId() (hits_core): s2sdk::GetClientAuthId returns the same value pisexcompat's
//     GetSteamID returned, and the LR core normalizes it the same way, so both write one `steam`.
//   * The original hooked IServerGameClients::ClientDisconnect through SourceHook; the port
//     registers s2sdk's OnClientDisconnect (the same point the levels_ranks core uses; s2sdk 3.0
//     fires it before the client data is reset, like the original hook) and takes the steam id
//     from the engine with the id cached at OnPlayerLoaded as the fallback.
//   * The UPDATE of SaveData is built with bound parameters instead of the original's string
//     formatting; its buffer bug (the column list formatted into the uninitialized sColumns
//     buffer) is not reproduced - the columns are the same ones the flags select.
//   * The database is its own plgdb connection with the core's settings
//     (GetDatabaseConfig(), "lr.sqlite3" for the SQLite fallback); a Plugify plugin cannot share
//     another plugin's connection object. Layout and statements are the original's.
//   * Errors go through plg::Log (with the null check every pure-s2sdk plugin uses), never
//     pisex::ErrorLog.

#include <plg/plugin.hpp>
#include <plg/string.hpp>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "hits_core.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-Hits]";

// A failed row load is retried after this delay; a failed CREATE TABLE (the database may simply
// not be up yet at plugin start) after the longer one. Errors of the per-hit saves are logged at
// most once per interval - the point of the delay is a quiet log during a database outage.
constexpr int64_t kLoadRetrySeconds = 5;
constexpr int64_t kDdlRetrySeconds = 30;
constexpr int64_t kErrorLogSeconds = 10;

int64_t NowSeconds() {
	return static_cast<int64_t>(std::time(nullptr));
}

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

using namespace lr_hits;

// --- logging -------------------------------------------------------------------------------------
// plg::Log is filled by the plugify module right before OnPluginStart; the null check keeps the
// plugin silent (not crashing) in an unusual host that never provides it.

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// --- map gate (PITFALLS B1/B2) -------------------------------------------------------------------
//
// The same three flags (and the same state machine) pisexcompat used, driven by the s2sdk listeners
// directly:
//   g_mapReady       true while a map is up: every s2sdk client call is allowed
//   g_mapStartSeen   an OnMapStart arrived since the start of the plugin / the last OnMapEnd
//   g_mapEndSeen     an OnMapEnd arrived since the start of the plugin

bool g_mapReady = false;
bool g_mapStartSeen = false;
bool g_mapEndSeen = false;
bool g_mapListenersRegistered = false;

// s2sdk listeners: plain function pointers, called on the game thread. Nothing here may throw and
// no client call is made - only state changes.
void OnMapStartListener() {
	// The engine calls this once the new map and its client list exist: every client call is legal
	// from here on.
	g_mapReady = true;
	g_mapStartSeen = true;
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart s2sdk's client list is torn down and any
	// client query crashes - so this handler only drops state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without the map-end listener nothing could take readiness away again, so the gate stays
		// closed for good (PITFALLS A1: log and stay inactive, never return Failed).
		LogError(std::string(kLogTag) +
				 " the s2sdk map listeners are unavailable - the hit statistics stay off");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

void UnregisterMapListeners() {
	if (!g_mapListenersRegistered) {
		return;
	}
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

void PollMapGate() {
	if (!g_mapListenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first OnPluginUpdate after
	// OnPluginStart is the proof of a live map (the host pumps updates from the server frame).
	// After an OnMapEnd only a new OnMapStart re-opens the gate - the host keeps pumping updates
	// through a changelevel, so `!g_mapEndSeen` must not be enough once one was seen.
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

// --- client gates (PITFALLS C2) -------------------------------------------------------------------

// pisex::g_pPlayers->IsAuthenticated() of the old port: the slot has an engine-authenticated human
// client. Every check is short-circuited in the order of the PITFALLS rule, and the map gate comes
// first - a closed gate means no s2sdk client call may run at all.
bool IsAuthenticated(int slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_IsFakeClient == nullptr ||
		__s2sdk_IsClientAuthorized == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot) && !s2sdk::IsFakeClient(slot) &&
			   s2sdk::IsClientAuthorized(slot);
	} catch (...) {
		return false;
	}
}

// pisex::g_pPlayers->GetSteamID() of the old port: the engine's authentication string of the slot,
// which NormalizeSteamId() turns into the "STEAM_1:x:y" form the LR tables use. A bot, SourceTV or
// a client that is still connecting has none (the caller falls back to the id cached at load).
std::string ClientSteamId2(int slot) {
	if (!g_mapReady || slot < 0 || slot >= kMaxSlots) {
		return {};
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientSourceTV == nullptr ||
		__s2sdk_IsFakeClient == nullptr || __s2sdk_GetClientAuthId == nullptr) {
		return {};
	}
	try {
		if (!s2sdk::IsClientConnected(slot) || s2sdk::IsClientSourceTV(slot) ||
			s2sdk::IsFakeClient(slot)) {
			return {};
		}
		const plg::string auth = s2sdk::GetClientAuthId(slot);
		return std::string(auth.data(), auth.size());
	} catch (...) {
		return {};
	}
}

class LrHitsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// lr_hits::OnClientDisconnect of the original (s2sdk OnClientDisconnect context: still called
	// before the client data is reset, in the same slot-preserving window as the original hook).
	void OnClientDisconnect(int32_t slot);

	// OnPlayerHurt() of the original (the s2sdk "player_hurt" Pre hook).
	void OnPlayerHurt(void* event);

private:
	void OnCoreIsReady();
	void OnPlayerLoaded(int slot, const char* steamid);
	void OnResetPlayerStats(int slot, const char* steamid);
	// SaveData() of the original; queues nothing when no hitgroup flag is set.
	void SaveData(int slot);
	// engine->GetPlayerNetworkIDString(slot) + ConvertSteamID, with the cached id as the fallback.
	std::string SteamIdOf(int slot) const;
	void ZeroSlot(SlotState& state);
	// The load of |id| into |slot|; a query error leaves the slot unloaded and schedules a retry.
	void StartLoad(int slot, const std::string& id);
	// The retries of the DDL and of the failed/silent loads (OnPluginUpdate).
	void UpdateRetries();
	void IssueCreateTable();
	void LogLoadError(const std::string& error);
	void LogSaveError(const std::string& error);

	State m_state{};
	std::array<std::string, kMaxSlots> m_steamids{};
	// The slot's row is in memory: only then are hits counted and saved. A failed SELECT used to
	// look like "no row" and the next save replaced the real damage totals with the hits counted
	// since the error; an unloaded slot now neither counts nor writes.
	std::array<bool, kMaxSlots> m_loaded{};
	std::array<bool, kMaxSlots> m_load_in_flight{};
	std::array<int64_t, kMaxSlots> m_load_retry_at{};
	std::array<uint64_t, kMaxSlots> m_load_seq{};
	std::unique_ptr<plgdb::Database> m_db;
	std::string m_table; // <lr_table>_hits
	bool m_disconnect_registered = false;
	bool m_event_hooked = false;
	// The table must exist before any load/save is issued. A failed CREATE TABLE is retried; a
	// database that comes up after the core (the reboot race) recovers without a plugin reload.
	bool m_table_ready = false;
	int64_t m_ddl_requested_at = 0;
	int64_t m_last_load_error_log = 0;
	int64_t m_last_save_error_log = 0;
};

LrHitsPlugin g_plugin;

// --- s2sdk callbacks -----------------------------------------------------------------------------
// Plain function pointers called by the engine: nothing may unwind into it, so every body is
// wrapped and the helpers only touch plugin state.

void OnClientDisconnectListener(int32_t slot, s2sdk::NetworkDisconnectionReason /*reason*/) {
	try {
		g_plugin.OnClientDisconnect(slot);
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnClientDisconnect failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnClientDisconnect failed with an unknown exception");
	}
}

// The single "player_hurt" hook. A Pre hook must answer Continue on every path: a result >=
// Handled frees the event for every other plugin on the server (the original's listener returned
// false for the same reason).
s2sdk::ResultType OnPlayerHurtEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		g_plugin.OnPlayerHurt(event);
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " player_hurt failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " player_hurt failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

// --- database ------------------------------------------------------------------------------------

void LrHitsPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: the connection first, then the table and the hooks.
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		LogError(std::string(kLogTag) + " the database configuration is invalid - hit statistics are off");
		return;
	}
	// The DDL is queued (FIFO before every later statement), like the original's async Query. Its
	// result is checked: a database that is not up yet must not leave the module without a table.
	m_table_ready = false;
	IssueCreateTable();

	g_pLRCore->HookOnPlayerLoaded([this](int slot, const char* steamid) { OnPlayerLoaded(slot, steamid); });
	g_pLRCore->HookOnResetPlayerStats(
		[this](int slot, const char* steamid) { OnResetPlayerStats(slot, steamid); });
}

void LrHitsPlugin::IssueCreateTable() {
	if (m_db == nullptr) {
		return;
	}
	m_ddl_requested_at = NowSeconds();
	plgdb::Statement ddl{CreateTableSql(m_table, m_db->GetDriver()), {}};
	m_db->Query(std::move(ddl.sql), std::move(ddl.params), [this](const plgdb::Result& result) {
		if (result.Ok()) {
			m_table_ready = true;
			return;
		}
		LogError(std::string(kLogTag) + " creating the " + m_table + " table failed: " + result.Error());
	});
}

void LrHitsPlugin::UpdateRetries() {
	if (m_db == nullptr) {
		return;
	}
	const int64_t now = NowSeconds();
	if (!m_table_ready) {
		if (now - m_ddl_requested_at >= kDdlRetrySeconds) {
			IssueCreateTable();
		}
		return; // no load can succeed before the table exists
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		const std::size_t index = static_cast<std::size_t>(slot);
		if (m_loaded[index] || m_load_in_flight[index] || m_steamids[index].empty()) {
			continue;
		}
		if (now < m_load_retry_at[index]) {
			continue;
		}
		StartLoad(slot, m_steamids[index]);
	}
}

void LrHitsPlugin::LogLoadError(const std::string& error) {
	const int64_t now = NowSeconds();
	if (now - m_last_load_error_log < kErrorLogSeconds) {
		return;
	}
	m_last_load_error_log = now;
	LogError(std::string(kLogTag) + " loading the hit statistics failed: " + error);
}

void LrHitsPlugin::LogSaveError(const std::string& error) {
	const int64_t now = NowSeconds();
	if (now - m_last_save_error_log < kErrorLogSeconds) {
		return;
	}
	m_last_save_error_log = now;
	LogError(std::string(kLogTag) + " saving the hit statistics failed: " + error);
}

void LrHitsPlugin::StartLoad(int slot, const std::string& id) {
	const std::size_t index = static_cast<std::size_t>(slot);
	const uint64_t seq = ++m_load_seq[index];
	m_load_in_flight[index] = true;
	m_loaded[index] = false;
	m_load_retry_at[index] = 0;
	Statement load = LoadStatement(m_table, id);
	m_db->Query(std::move(load.sql), std::move(load.params),
				[this, slot, id, seq](const plgdb::Result& result) {
					const std::size_t index = static_cast<std::size_t>(slot);
					if (m_load_seq[index] != seq || m_steamids[index] != id) {
						// The slot was taken by another player while the query was in flight (a fast
						// reconnect): this row is not theirs (the core drops stale loads the same way).
						return;
					}
					m_load_in_flight[index] = false;
					if (!result.Ok()) {
						// An error is not "no row": the counters stay as they are and the load is
						// retried; only a successful empty result is a new player (below).
						LogLoadError(result.Error());
						m_load_retry_at[index] = NowSeconds() + kLoadRetrySeconds;
						return;
					}
					ApplyLoaded(m_state[index], result);
					m_loaded[index] = true;
					if (result.RowCount() == 0) {
						// The original inserted the missing row inside the callback and loaded the zeros.
						Statement insert = InsertStatement(m_table, id);
						m_db->Query(std::move(insert.sql), std::move(insert.params),
									[this, slot, id, seq](const plgdb::Result& insert_result) {
										const std::size_t index = static_cast<std::size_t>(slot);
										if (m_load_seq[index] != seq || m_steamids[index] != id) {
											return;
										}
										if (!insert_result.Ok()) {
											// Without the row the next UPDATE would match nothing:
											// retry the load (which re-inserts) like a failed SELECT.
											LogLoadError(insert_result.Error());
											m_loaded[index] = false;
											m_load_retry_at[index] = NowSeconds() + kLoadRetrySeconds;
										}
									});
					}
				});
}

void LrHitsPlugin::ZeroSlot(SlotState& state) {
	state.hits.fill(0);
	state.flags = 0;
}

void LrHitsPlugin::OnPlayerLoaded(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	// The core already writes the normalized id, so this is a no-op for a normal load; it keeps the
	// cached fallback of SteamIdOf() in the same form the table uses.
	const std::string id = steamid != nullptr ? NormalizeSteamId(steamid) : "";
	m_steamids[index] = id;
	// A load of the previous occupant (or of an earlier attempt) must not land in this slot.
	++m_load_seq[index];
	m_load_in_flight[index] = false;
	m_loaded[index] = false;
	m_load_retry_at[index] = 0;
	if (id.empty()) {
		return;
	}
	if (slot != 0) {
		// The original's non-zero-slot guard around the pre-query zeroing (slot 0 only got the
		// values of the callback below).
		ZeroSlot(m_state[index]);
	}
	if (m_table_ready) {
		StartLoad(slot, id);
	}
	// Otherwise UpdateRetries() starts the load once the CREATE TABLE succeeded.
}

void LrHitsPlugin::OnResetPlayerStats(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	const std::string id = steamid != nullptr ? NormalizeSteamId(steamid) : "";
	m_steamids[index] = id;
	// An answer of a load that is still in flight would put the pre-reset row back into the slot.
	++m_load_seq[index];
	m_load_in_flight[index] = false;
	// The local counters have to go too: this port's core does not reload the player after a reset
	// (the original's OnPlayerLoaded callback did), so the old totals would come back on the next
	// hit and be saved again.
	ZeroSlot(m_state[index]);
	if (!m_table_ready) {
		return;
	}
	Statement reset = ResetStatement(m_table, id);
	m_db->Query(std::move(reset.sql), std::move(reset.params));
}

// --- game -----------------------------------------------------------------------------------------

std::string LrHitsPlugin::SteamIdOf(int slot) const {
	// engine->GetPlayerNetworkIDString(slot) in the original; pisexcompat's GetSteamID was the same
	// s2sdk::GetClientAuthId call behind the map gate, so ClientSteamId2() replaces it one to one.
	std::string id = NormalizeSteamId(ClientSteamId2(slot));
	if (id.empty() && slot >= 0 && slot < kMaxSlots) {
		// The id the LR core loaded into the slot: still usable while the map gate is closed, which
		// is exactly the window a changelevel's disconnects run in.
		id = m_steamids[static_cast<std::size_t>(slot)];
	}
	return id;
}

void LrHitsPlugin::SaveData(int slot) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr || !m_table_ready) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	if (!m_loaded[index]) {
		return; // the row is not in memory; a save would write the handful of hits counted since
	}
	if (m_state[index].flags == 0) {
		return; // the original's `if(iFlags)` guard
	}
	const std::string id = SteamIdOf(slot);
	if (id.empty()) {
		// The original wrote the row with an empty steam id (matching nothing); the port keeps the
		// counters for the next save instead.
		LogError(std::string(kLogTag) + " no steam id for slot " + std::to_string(slot) +
				 " - the hit statistics were not saved");
		return;
	}
	const int saved_flags = m_state[index].flags; // SaveStatement clears them when it builds the SQL
	Statement statement = SaveStatement(m_table, m_state[index], id);
	if (statement.sql.empty()) {
		return;
	}
	m_db->Query(std::move(statement.sql), std::move(statement.params),
				[this, slot, id, saved_flags](const plgdb::Result& result) {
					if (result.Ok()) {
						return;
					}
					LogSaveError(result.Error());
					const std::size_t index = static_cast<std::size_t>(slot);
					if (m_steamids[index] == id) {
						// The statement did not run: keep the columns flagged so the next save
						// (per hit or at disconnect) writes them again. The counters of the slot
						// have advanced in the meantime; their absolute values are what is bound.
						m_state[index].flags |= saved_flags;
					}
				});
}

void LrHitsPlugin::OnPlayerHurt(void* event) {
	// The statistic gate first (all of these checks are side-effect free; the original ordered them
	// the other way round): with no event, no map, in warmup or with fewer than lr_minplayers_count
	// players every player_hurt costs nothing at all instead of an engine authentication call.
	if (event == nullptr || !g_mapReady || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	// The s2sdk wrappers call their binding slots unchecked, so a missing one would be a null call
	// from inside the game event hook. Both accessors are needed here: the attacker slot and the
	// three numeric fields.
	if (__s2sdk_GetEventPlayerSlot == nullptr || __s2sdk_GetEventInt == nullptr) {
		return;
	}
	// OnPlayerHurt of the original. "attacker" is a `player_controller_and_pawn` field of
	// player_hurt, so it is read with the player accessor: s2sdk::GetEventPlayerSlot resolves the
	// field to the engine's 0-based player slot and answers INVALID_PLAYER_SLOT_INDEX (-1) when no
	// player is behind it (world damage). s2sdk::GetEventInt on the same field is IGameEvent::GetInt,
	// i.e. the engine user id, which is not guaranteed to equal the slot (the same finding the cs2ac
	// port records in hooks.cpp) - the original Pisex module read the user id there and indexed by
	// it; the port reads the slot.
	const int attacker = static_cast<int>(s2sdk::GetEventPlayerSlot(event, "attacker"));
	if (attacker < 0 || attacker >= kMaxSlots) {
		// The original indexed g_iHits[iAttacker] after only the engine check; out-of-range
		// attackers (world damage) are dropped here instead of writing out of bounds.
		return;
	}
	if (!IsAuthenticated(attacker)) {
		return;
	}
	if (!m_loaded[static_cast<std::size_t>(attacker)]) {
		// The row is still on its way (or failed and is being retried): the successful load
		// replaces the slot with the database values, so counting now would only lose the delta.
		return;
	}
	ApplyHit(m_state[static_cast<std::size_t>(attacker)], s2sdk::GetEventInt(event, "dmg_health"),
			 s2sdk::GetEventInt(event, "dmg_armor"), s2sdk::GetEventInt(event, "hitgroup"));
	if (g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode)) {
		SaveData(attacker);
	}
}

void LrHitsPlugin::OnClientDisconnect(int32_t slot) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (!g_pLRCore->GetSettingsValue(levels_ranks::LR_DB_SaveDataPlayer_Mode)) {
		SaveData(static_cast<int>(slot));
	}
	// SaveData above ran while the client data was still valid; drop the cached id now - s2sdk
	// resets the client once the disconnect callbacks return, and this is still the same slot.
	const std::size_t index = static_cast<std::size_t>(slot);
	++m_load_seq[index]; // an in-flight load of this occupant must not land in the reused slot
	m_load_in_flight[index] = false;
	m_loaded[index] = false;
	m_steamids[index].clear();
	ZeroSlot(m_state[index]);
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrHitsPlugin::OnPluginStart() {
	try {
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = false;

		// PITFALLS B1: registering the map listeners touches no client, so it is safe here. They
		// are the only source of the readiness every s2sdk client call of this plugin is gated on.
		RegisterMapListeners();

		// The ClientDisconnect hook of the original: it saves the row while the client data is
		// still valid, so it is not optional (the statistics would only be saved per hit).
		if (__s2sdk_OnClientDisconnect_Register != nullptr) {
			s2sdk::OnClientDisconnect_Register(&OnClientDisconnectListener);
			m_disconnect_registered = true;
		} else {
			LogError(std::string(kLogTag) +
					 " the s2sdk OnClientDisconnect listener is unavailable - the statistics are only saved per hit");
		}

		// PITFALLS C12: one Pre hook on "player_hurt", the event pisexcompat's HookEvent wrapped.
		// The handler needs both event accessors (the attacker player slot and the numeric fields),
		// so a build whose s2sdk lacks one reports it here instead of failing silently on every hit;
		// OnPlayerHurt() guards them again, because the generated wrappers call their slots unchecked.
		if (__s2sdk_HookEvent == nullptr) {
			LogError(std::string(kLogTag) + " the s2sdk event bindings are unavailable - 'player_hurt' is not hooked");
		} else if (__s2sdk_GetEventPlayerSlot == nullptr || __s2sdk_GetEventInt == nullptr) {
			LogError(std::string(kLogTag) +
					 " the s2sdk event accessors are unavailable - 'player_hurt' is not hooked");
		} else if (s2sdk::HookEvent("player_hurt", &OnPlayerHurtEvent, s2sdk::HookMode::Pre) ==
				   s2sdk::EventHookError::Okay) {
			m_event_hooked = true;
		} else {
			LogError(std::string(kLogTag) + " hooking 'player_hurt' was rejected - the hit statistics stay off");
		}

		// AllPluginsLoaded() of the original: the event hook first, then the LR core lookup.
		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		LogInfo(std::string(kLogTag) + " ready");
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrHitsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// PITFALLS B1/B2: the host pumps this from the server frame, so the first update proves a
		// live map for a plugin loaded into a running map.
		PollMapGate();
		if (m_db != nullptr) {
			m_db->Poll();
		}
		UpdateRetries();
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginUpdate failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginUpdate failed with an unknown exception");
	}
	return plg::PluginResult{};
}

plg::PluginResult LrHitsPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7/C12: every registration of this library is removed again, so `plg reload`
		// leaves nothing that points into the unloaded .so.
		if (m_event_hooked) {
			m_event_hooked = false;
			if (__s2sdk_UnhookEvent != nullptr) {
				s2sdk::UnhookEvent("player_hurt", &OnPlayerHurtEvent, s2sdk::HookMode::Pre);
			}
		}
		if (m_disconnect_registered) {
			m_disconnect_registered = false;
			if (__s2sdk_OnClientDisconnect_Unregister != nullptr) {
				s2sdk::OnClientDisconnect_Unregister(&OnClientDisconnectListener);
			}
		}
		// The Unhook* are no-ops when levels_ranks is not loaded (lr_api.hpp checks the binding
		// slot); the Available() guard only keeps that intent visible here.
		if (g_pLRCore->Available()) {
			g_pLRCore->UnhookOnPlayerLoaded();
			g_pLRCore->UnhookOnResetPlayerStats();
			g_pLRCore->UnhookOnCoreIsReady();
		}
		// The gate closes last: a listener the engine may still hold for a moment must not reach a
		// client, and nothing may call into this library after it is unloaded.
		UnregisterMapListeners();
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = false;
		m_db.reset();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
