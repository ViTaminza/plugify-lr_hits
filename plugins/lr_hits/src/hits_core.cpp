#include "hits_core.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <utility>

namespace lr_hits {
namespace {

// sHitColumnName of the original, in hitgroup order (index = hitType - kHdHitHead).
constexpr std::string_view kColumnNames[] = {"Head", "Chest", "Belly", "LeftArm",
											 "RightArm", "LeftLeg", "RightLeg", "Neak"};
constexpr int kColumnCount = static_cast<int>(sizeof(kColumnNames) / sizeof(kColumnNames[0]));

bool IsCounterColumn(int hitType) {
	return hitType >= kHdHitHead && hitType - kHdHitHead < kColumnCount;
}

std::string_view TrimSpaces(std::string_view text) {
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const auto end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}

} // namespace

std::string_view ColumnName(int hitType) {
	if (!IsCounterColumn(hitType)) {
		return {};
	}
	return kColumnNames[hitType - kHdHitHead];
}

void ApplyHit(SlotState& state, int dmg_health, int dmg_armor, int hitgroup) {
	state.hits[kHdDmgHealth] += dmg_health;
	state.hits[kHdDmgArmor] += dmg_armor;

	const int hit_type = hitgroup + 1; // iHB of the original
	if (hit_type > 1 && hit_type < kHitData) {
		state.hits[hit_type]++;
		state.hits[kHdHitAll]++;
		state.flags |= 1 << hit_type;
	}
}

std::string TableName(std::string_view base) {
	return std::string(base) + "_hits";
}

std::string NormalizeSteamId(std::string_view network_id) {
	const std::string_view trimmed = TrimSpaces(network_id);
	if (trimmed.empty()) {
		return {};
	}
	if (trimmed.rfind("STEAM_", 0) == 0) {
		return std::string(trimmed);
	}
	// "[U:1:accountid]" and friends: strip the brackets, split on ':' and rebuild STEAM_1.
	std::string stripped;
	stripped.reserve(trimmed.size());
	for (const char c : trimmed) {
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
		// The original indexed usteamid_split[2] here and crashed; the port keeps the value.
		return stripped;
	}
	std::int64_t account = 0;
	const std::string_view account_text = TrimSpaces(parts[2]);
	const auto parsed =
		std::from_chars(account_text.data(), account_text.data() + account_text.size(), account);
	if (parsed.ec != std::errc{}) {
		return stripped;
	}
	return "STEAM_1:" + std::string(account % 2 == 0 ? "0:" : "1:") + std::to_string(account / 2);
}

std::string CreateTableSql(std::string_view table, plgdb::Driver driver) {
	const std::string name(table);
	if (driver == plgdb::Driver::MySQL) {
		// The CREATE TABLE of the original, byte for byte.
		return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
			   "`SteamID` varchar(32) NOT NULL PRIMARY KEY DEFAULT '', "
			   "`DmgHealth` int NOT NULL DEFAULT 0, "
			   "`DmgArmor` int NOT NULL DEFAULT 0, "
			   "`Head` int NOT NULL DEFAULT 0, "
			   "`Chest` int NOT NULL DEFAULT 0, "
			   "`Belly` int NOT NULL DEFAULT 0, "
			   "`LeftArm` int NOT NULL DEFAULT 0, "
			   "`RightArm` int NOT NULL DEFAULT 0, "
			   "`LeftLeg` int NOT NULL DEFAULT 0, "
			   "`RightLeg` int NOT NULL DEFAULT 0, "
			   "`Neak` int NOT NULL DEFAULT 0"
			   ") CHARSET = utf8 COLLATE utf8_general_ci;";
	}
	// SQLite has no CHARSET/COLLATE clauses; the columns and the primary key are the same.
	return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
		   "`SteamID` TEXT NOT NULL PRIMARY KEY DEFAULT '', "
		   "`DmgHealth` INTEGER NOT NULL DEFAULT 0, "
		   "`DmgArmor` INTEGER NOT NULL DEFAULT 0, "
		   "`Head` INTEGER NOT NULL DEFAULT 0, "
		   "`Chest` INTEGER NOT NULL DEFAULT 0, "
		   "`Belly` INTEGER NOT NULL DEFAULT 0, "
		   "`LeftArm` INTEGER NOT NULL DEFAULT 0, "
		   "`RightArm` INTEGER NOT NULL DEFAULT 0, "
		   "`LeftLeg` INTEGER NOT NULL DEFAULT 0, "
		   "`RightLeg` INTEGER NOT NULL DEFAULT 0, "
		   "`Neak` INTEGER NOT NULL DEFAULT 0"
		   ");";
}

Statement LoadStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql = "SELECT `DmgHealth`, `DmgArmor`, `Head`, `Chest`, `Belly`, `LeftArm`, "
					"`RightArm`, `LeftLeg`, `RightLeg`, `Neak` FROM `" + std::string(table) +
					"` WHERE `SteamID` = ?;";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

Statement InsertStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql = "INSERT INTO `" + std::string(table) + "` (`SteamID`) VALUES (?);";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

void ApplyLoaded(SlotState& state, const plgdb::Result& result) {
	// An error is not "no row": the slot keeps its counters and the plugin retries the load.
	// Zeroing here would let the next save overwrite the real row with the hits counted since.
	if (!result.Ok()) {
		return;
	}
	// The callback of the original: every counter is written (a missing row leaves zeros).
	for (int i = 0; i < kHitData; ++i) {
		state.hits[i] = 0;
	}
	state.flags = 0;
	// The original rewrote the slot from the query result row (or the zeros above when the row had
	// just been inserted).
	const std::optional<std::size_t> column = result.ColumnIndex("DmgHealth");
	if (result.RowCount() == 0 || !column.has_value()) {
		return;
	}
	const std::size_t row = 0;
	const std::size_t base = *column;
	for (int i = 0; i < kHitData - 1; ++i) {
		state.hits[i] = static_cast<int>(result.GetInt(row, base + static_cast<std::size_t>(i), 0));
	}
	// The all-hit sum: the original added the hitgroup counters (indexes > 1) up in the same loop.
	int all = 0;
	for (int i = kHdHitHead; i < kHdHitAll; ++i) {
		all += state.hits[i];
	}
	state.hits[kHdHitAll] = all;
}

Statement SaveStatement(std::string_view table, SlotState& state, std::string_view steamid) {
	Statement statement;
	if (state.flags == 0) {
		return statement; // the original's `if(iFlags)` guard: nothing changed since the last save
	}
	// `DmgHealth`/`DmgArmor` are always written (like the original), then one assignment per
	// flagged hitgroup column (in hitgroup order).
	std::string sql = "UPDATE `" + std::string(table) + "` SET `DmgHealth` = ?, `DmgArmor` = ?";
	plgdb::Params params;
	params.emplace_back(static_cast<int64_t>(state.hits[kHdDmgHealth]));
	params.emplace_back(static_cast<int64_t>(state.hits[kHdDmgArmor]));
	for (int type = kHdHitHead; type < kHdHitAll; ++type) {
		if ((state.flags & (1 << type)) == 0) {
			continue;
		}
		sql += ", `";
		sql += ColumnName(type);
		sql += "` = ?";
		params.emplace_back(static_cast<int64_t>(state.hits[type]));
	}
	sql += " WHERE `SteamID` = ?";
	statement.sql = std::move(sql);
	params.emplace_back(std::string(steamid));
	statement.params = std::move(params);
	state.flags = 0; // the original cleared them right after building the statement
	return statement;
}

Statement ResetStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql = "UPDATE `" + std::string(table) +
					"` SET `DmgHealth` = 0, `DmgArmor` = 0, `Head` = 0, `Chest` = 0, `Belly` = 0, "
					"`LeftArm` = 0, `RightArm` = 0, `LeftLeg` = 0, `RightLeg` = 0, `Neak` = 0 "
					"WHERE `SteamID` = ?";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

} // namespace lr_hits
