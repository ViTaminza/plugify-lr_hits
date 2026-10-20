#pragma once
// Engine-free core of the lr_hits port (the Pisex LR module "[LR] ExStats Hits", the original's
// lr_hits.cpp): the per-slot hit counters of OnPlayerHurt, the `<lr_table>_hits` table (SteamID +
// damage/armor + one column per hitgroup) and the SQL of OnPlayerLoadedHook / OnResetLoadedHook /
// SaveData.
//
// The engine-side half (the player_hurt hook, the disconnect listener, the LR core hooks, plgdb)
// lives in src/plugin.cpp; everything here is testable against SQLite and MySQL.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <plgdb/db.hpp>

namespace lr_hits {

inline constexpr int kMaxSlots = 64;

// The original's HitData array size and its indexes (g_iHits[slot][...]).
inline constexpr int kHitData = 11;
inline constexpr int kHdDmgHealth = 0;
inline constexpr int kHdDmgArmor = 1;
inline constexpr int kHdHitHead = 2;
inline constexpr int kHdHitChest = 3;
inline constexpr int kHdHitBelly = 4;
inline constexpr int kHdHitLeftArm = 5;
inline constexpr int kHdHitRightArm = 6;
inline constexpr int kHdHitLeftLeg = 7;
inline constexpr int kHdHitRightLeg = 8;
inline constexpr int kHdHitNeak = 9;
inline constexpr int kHdHitAll = 10;

// The column of each hitgroup index (sHitColumnName of the original, indexed by Type - 2). The
// original also indexed it for Type 10 (`kHdHitAll`) — one past its end; the port never asks for
// that column (bit 10 is the sum, it has no column).
std::string_view ColumnName(int hitType);

// --- per-slot state (the original's g_iHits / g_iHitFlags) --------------------------------------

struct SlotState {
	std::array<int, kHitData> hits{};
	int flags = 0; // `1 << hitType` for every hitgroup counter that changed since the last save
};

using State = std::array<SlotState, kMaxSlots>;

// OnPlayerHurt of the original, minus the guards (the caller checks authentication and the player
// count): dmg_health/dmg_armor into [0]/[1], and for a hitgroup in 2..10 the counter, the all-hit
// sum and the save flag. |hitgroup| is the event's raw value (the original added 1).
void ApplyHit(SlotState& state, int dmg_health, int dmg_armor, int hitgroup);

// --- SQL -----------------------------------------------------------------------------------------
// Statements are built here and executed by the plugin (async, on plgdb's worker) or by the tests
// (QuerySync). Values are always bound parameters, never interpolated.

// `<base>_hits` (the original's g_sTableName + "_hits").
std::string TableName(std::string_view base);

std::string CreateTableSql(std::string_view table, plgdb::Driver driver);

struct Statement {
	std::string sql;
	plgdb::Params params;
};

// OnPlayerLoadedHook(): SELECT of the ten counters; an empty result means "no row yet" (the caller
// then runs InsertStatement and applies zeros).
Statement LoadStatement(std::string_view table, std::string_view steamid);
// The original's `INSERT INTO t (SteamID) VALUES ('...')` for a player without a row (the defaults
// of the table leave every counter at 0).
Statement InsertStatement(std::string_view table, std::string_view steamid);

// Applies a LoadStatement result to the slot state (the callback of the original): every counter
// zeroed, a found row copied in, and the all-hit sum rebuilt from the hitgroup counters.
void ApplyLoaded(SlotState& state, const plgdb::Result& result);

// SaveData() of the original: an UPDATE of DmgHealth/DmgArmor plus the hitgroup columns whose flag
// bit is set; the flags are cleared (the original built the statement and cleared them). An empty
// sql means "nothing to save" (no flags).
Statement SaveStatement(std::string_view table, SlotState& state, std::string_view steamid);

// OnResetLoadedHook(): UPDATE of every counter to 0.
Statement ResetStatement(std::string_view table, std::string_view steamid);

} // namespace lr_hits
