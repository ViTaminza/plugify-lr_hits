// Tests for plugins/lr_hits/src/hits_core.hpp (engine-free core of the ported
// "[LR] ExStats Hits" module).
//
//   * the OnPlayerHurt counters (damage/armor, the hitgroup mapping, the all-hit sum, the flags),
//   * the column mapping of the original,
//   * the SQL layer: CREATE / SELECT / INSERT / the flag-driven UPDATE / reset against SQLite
//     ":memory:" always and against MySQL when MYSQL_HOST is set (test.sh provides
//     MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE). Rows created in MySQL are
//     deleted at the end.

#include <plg/plugin.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include "hits_core.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                            \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),         \
						g_section.data(), __FILE__, __LINE__, #expr);                          \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const long long a_ = static_cast<long long>(actual);                                   \
		const long long e_ = static_cast<long long>(expected);                                 \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_, e_);                                                     \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const std::string a_{std::string_view(actual)};                                        \
		const std::string e_{std::string_view(expected)};                                      \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

using namespace lr_hits;

// --- the counters ---------------------------------------------------------------------------------

void TestApplyHit() {
	Section("ApplyHit (OnPlayerHurt)");
	SlotState state;

	// A headshot (hitgroup 1 -> iHB 2) for 30 damage / 5 armor.
	ApplyHit(state, 30, 5, 1);
	CHECK_EQ_INT(state.hits[kHdDmgHealth], 30);
	CHECK_EQ_INT(state.hits[kHdDmgArmor], 5);
	CHECK_EQ_INT(state.hits[kHdHitHead], 1);
	CHECK_EQ_INT(state.hits[kHdHitAll], 1);
	CHECK_EQ_INT(state.flags, 1 << kHdHitHead);

	// A chest hit (hitgroup 2 -> iHB 3) for 10 damage.
	ApplyHit(state, 10, 0, 2);
	CHECK_EQ_INT(state.hits[kHdHitChest], 1);
	CHECK_EQ_INT(state.hits[kHdHitAll], 2);
	CHECK_EQ_INT(state.flags, (1 << kHdHitHead) | (1 << kHdHitChest));

	// hitgroup 0 (generic) -> iHB 1: damage only, no hitgroup counter (the original's `1 < iHB`).
	ApplyHit(state, 7, 3, 0);
	CHECK_EQ_INT(state.hits[kHdDmgHealth], 47);
	CHECK_EQ_INT(state.hits[kHdDmgArmor], 8);
	CHECK_EQ_INT(state.hits[kHdHitAll], 2);
	CHECK_EQ_INT(state.flags, (1 << kHdHitHead) | (1 << kHdHitChest));

	// hitgroup 7 (right leg) -> iHB 8.
	ApplyHit(state, 4, 0, 7);
	CHECK_EQ_INT(state.hits[kHdHitRightLeg], 1);
	CHECK_EQ_INT(state.hits[kHdHitAll], 3);

	// hitgroup 9 -> iHB 10: inside the original's `iHB < HitData` guard, and index 10 *is* the
	// all-hit counter, so the original's two increments (the hitgroup one and the all-hit one)
	// land on the same counter: +2. The flag bit 10 has no column (the original read one past
	// sHitColumnName there; the port's SaveStatement ignores it).
	ApplyHit(state, 1, 0, 9);
	CHECK_EQ_INT(state.hits[kHdHitAll], 5);
	CHECK_EQ_INT(state.flags & (1 << 10), 1 << 10);

	// hitgroup 10 -> iHB 11: outside the guard, dropped entirely.
	ApplyHit(state, 1, 0, 10);
	CHECK_EQ_INT(state.hits[kHdHitAll], 5);
	CHECK_EQ_INT(state.flags & (1 << 11), 0);
}

void TestColumnMapping() {
	Section("column names");
	CHECK_EQ_STR(ColumnName(kHdHitHead), "Head");
	CHECK_EQ_STR(ColumnName(kHdHitChest), "Chest");
	CHECK_EQ_STR(ColumnName(kHdHitBelly), "Belly");
	CHECK_EQ_STR(ColumnName(kHdHitLeftArm), "LeftArm");
	CHECK_EQ_STR(ColumnName(kHdHitRightArm), "RightArm");
	CHECK_EQ_STR(ColumnName(kHdHitLeftLeg), "LeftLeg");
	CHECK_EQ_STR(ColumnName(kHdHitRightLeg), "RightLeg");
	CHECK_EQ_STR(ColumnName(kHdHitNeak), "Neak"); // the shipped spelling
	// The all-hit index and the non-counter indexes have no column.
	CHECK_EQ_STR(ColumnName(kHdHitAll), "");
	CHECK_EQ_STR(ColumnName(kHdDmgHealth), "");
	CHECK_EQ_STR(ColumnName(99), "");
	CHECK_EQ_STR(TableName("lvl_base"), "lvl_base_hits");
}

// --- SQL -----------------------------------------------------------------------------------------

plgdb::Config SqliteConfig() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	return config;
}

bool MySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (host == nullptr || *host == '\0') {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") != nullptr ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") != nullptr ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") != nullptr ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port != nullptr && *port != '\0') {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

void TestDatabase(const plgdb::Config& config, std::string_view label, const std::string& table,
				  bool cleanup) {
	Section(std::string("database ") + std::string(label));
	std::unique_ptr<plgdb::Database> db = plgdb::Database::Create(config);
	CHECK(db != nullptr);
	if (db == nullptr) {
		return;
	}
	CHECK(db->QuerySync(CreateTableSql(table, db->GetDriver()), {}).Ok());

	const std::string steam = "STEAM_1:1:90000001";
	State state{};
	SlotState& slot = state[3];

	// A fresh player: the SELECT finds no row, the original inserts one and the counters are zero.
	{
		Statement load = LoadStatement(table, steam);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 0);
		ApplyLoaded(slot, result);
		CHECK_EQ_INT(slot.hits[kHdDmgHealth], 0);
		CHECK_EQ_INT(slot.hits[kHdHitAll], 0);
		Statement insert = InsertStatement(table, steam);
		CHECK(db->QuerySync(insert.sql, insert.params).Ok());
	}

	// A save without flags is a no-op (the original guarded it with a flags check).
	CHECK(SaveStatement(table, slot, steam).sql.empty());

	// Two headshots and one chest hit: the UPDATE carries DmgHealth/DmgArmor and only the flagged
	// hitgroup columns.
	ApplyHit(slot, 30, 5, 1);
	ApplyHit(slot, 25, 1, 1);
	ApplyHit(slot, 10, 0, 2);
	{
		Statement save = SaveStatement(table, slot, steam);
		CHECK_EQ_STR(save.sql,
					 "UPDATE `" + table +
						 "` SET `DmgHealth` = ?, `DmgArmor` = ?, `Head` = ?, `Chest` = ? "
						 "WHERE `SteamID` = ?");
		CHECK_EQ_INT(save.params.size(), 5);
		CHECK_EQ_INT(slot.flags, 0); // the flags were cleared like the original did
		CHECK(db->QuerySync(save.sql, save.params).Ok());
	}
	// A second save right after: nothing changed, no statement.
	CHECK(SaveStatement(table, slot, steam).sql.empty());

	// Reload: the row is found and the counters (including the all-hit sum) are restored.
	{
		Statement load = LoadStatement(table, steam);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK_EQ_INT(result.RowCount(), 1);
		SlotState reloaded{};
		reloaded.hits[kHdDmgHealth] = 999; // must be overwritten by ApplyLoaded
		ApplyLoaded(reloaded, result);
		CHECK_EQ_INT(reloaded.hits[kHdDmgHealth], 65);
		CHECK_EQ_INT(reloaded.hits[kHdDmgArmor], 6);
		CHECK_EQ_INT(reloaded.hits[kHdHitHead], 2);
		CHECK_EQ_INT(reloaded.hits[kHdHitChest], 1);
		CHECK_EQ_INT(reloaded.hits[kHdHitBelly], 0);
		CHECK_EQ_INT(reloaded.hits[kHdHitAll], 3); // head + chest only
	}

	// OnResetLoadedHook(): every counter zeroed in the database, the local state untouched (the
	// original's empty callback; the core reloads the player right after).
	{
		Statement reset = ResetStatement(table, steam);
		CHECK(db->QuerySync(reset.sql, reset.params).Ok());
		Statement load = LoadStatement(table, steam);
		const plgdb::Result result = db->QuerySync(load.sql, load.params);
		CHECK_EQ_INT(result.RowCount(), 1);
		for (std::size_t column = 0; column < 10; ++column) {
			CHECK_EQ_INT(result.GetInt(0, column, -1), 0);
		}
	}

	if (cleanup) {
		db->QuerySync("DROP TABLE `" + table + "`;", {});
	}
}

} // namespace

int main() {
	std::printf("lr_hits_tests\n");

	std::random_device device;
	std::mt19937_64 rng(device());
	const std::string tag = std::to_string(rng() % 1000000ULL);

	TestApplyHit();
	TestColumnMapping();

	TestDatabase(SqliteConfig(), "sqlite (:memory:)", "lvl_base_hits_sqlite", /*cleanup=*/false);

	plgdb::Config mysql;
	std::string error;
	if (MySqlConfig(mysql, error)) {
		std::printf("== mysql: %s:%u/%s\n", mysql.host.c_str(), mysql.port, mysql.database.c_str());
		TestDatabase(mysql, "mysql", "lvl_hits_test_" + tag, /*cleanup=*/true);
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
