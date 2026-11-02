// Engine test for the *real* "player_hurt" callback of plugins/lr_hits/src/plugin.cpp.
//
// src/plugin.cpp is compiled into this executable together with the generated binding objects
// (s2sdk_bindings and lr_bindings). Their __s2sdk_* / __levels_ranks_* globals are plain variables
// that the plugify module normally fills; here the test fills them with fakes, drives the plugin
// through its own exported entry points (Plugify_PluginStart / _Update / _End) and fires the
// callback the plugin registered for "player_hurt".
//
// What it proves:
//   * "attacker" is read with s2sdk::GetEventPlayerSlot, not with s2sdk::GetEventInt. The fakes
//     answer deliberately different values (GetEventInt("attacker") = 7, the slot = 3), and every
//     client check must be called with 3 and never with 7. This is the bug the accessor change
//     fixes: player_hurt.attacker is a `player_controller_and_pawn` field, so GetEventInt returns
//     the engine user id (IGameEvent::GetInt) while GetEventPlayerSlot returns the 0-based slot.
//   * slot 0 is a real player and reaches the client checks (0 is not "world");
//   * -1 (INVALID_PLAYER_SLOT_INDEX, world damage) and 64 (out of range) never reach a client call;
//   * a missing GetEventPlayerSlot or GetEventInt binding is a no-op - no null call, no client
//     query, and the hook still answers Continue; a build that lacks the accessor at start is
//     reported once and does not hook the event at all;
//   * the map gate (PITFALLS B1/B2) is applied by the callback itself: a plugin loaded into a
//     running map works after the first OnPluginUpdate, an OnMapEnd closes the gate even while
//     the host keeps pumping updates, and the next OnMapStart opens it again;
//   * the LR core's "statistics off" (CheckCountPlayers) short-circuits everything;
//   * the callback returns s2sdk::ResultType::Continue on every path, also when a binding throws,
//     so it never frees the event for the other plugins of the server (PITFALLS C12);
//   * OnPluginEnd removes the event hook, both map listeners and the disconnect listener again
//     (PITFALLS A7).
//
// Nothing here talks to a game server, a database or the levels_ranks plugin: every binding is a
// counting fake in this file. Run by ../test.sh (build/tests/lr_hits_event_tests).

#include <plg/plugin.hpp>
#include <plg/string.hpp>

#include <s2sdk/s2sdk.hpp>

#include <levels_ranks/levels_ranks.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The entry points PLUGIFY_PLUGIN (compiled into this executable from src/plugin.cpp) exports.
extern "C" plg::PluginResult Plugify_PluginStart();
extern "C" plg::PluginResult Plugify_PluginUpdate(std::chrono::milliseconds dt);
extern "C" plg::PluginResult Plugify_PluginEnd();

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool condition, std::string_view what, int line) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s\n", __FILE__, line, static_cast<int>(what.size()), what.data());
	}
}

void CheckEqImpl(long long actual, long long expected, std::string_view what, int line) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s (got %lld, expected %lld)\n", __FILE__, line,
					static_cast<int>(what.size()), what.data(), actual, expected);
	}
}

void CheckStrImpl(const std::string& actual, std::string_view expected, std::string_view what, int line) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s (got \"%s\", expected \"%.*s\")\n", __FILE__, line,
					static_cast<int>(what.size()), what.data(), actual.c_str(),
					static_cast<int>(expected.size()), expected.data());
	}
}

#define CHECK(condition, message) CheckImpl((condition), (message), __LINE__)
#define CHECK_EQ(actual, expected, message) CheckEqImpl((actual), (expected), (message), __LINE__)
#define CHECK_STR(actual, expected, message) CheckStrImpl((actual), (expected), (message), __LINE__)

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// --- what the fakes answer -----------------------------------------------------------------------

struct FakeState {
	// Set by the test before a fire.
	int attacker_slot = -1; // s2sdk::GetEventPlayerSlot("attacker")
	int attacker_int = -1;  // s2sdk::GetEventInt("attacker") - must never be read
	int field_int = 0;      // s2sdk::GetEventInt("dmg_health"/"dmg_armor"/"hitgroup")
	bool throw_on_slot = false;

	// The client layer: every fake returns this (the slots it was asked about are recorded).
	bool connected = true;
	bool in_game = true;
	bool source_tv = false;
	bool fake_client = false;
	bool authorized = false;

	// levels_ranks::CheckCountPlayers() - false simulates warmup / too few players.
	bool count_players = true;

	// Recorded calls.
	std::vector<int> client_slots;
	std::vector<int> authorized_slots;
	std::vector<std::string> slot_keys;
	std::vector<std::string> int_keys;
};

FakeState g_fake;

void ResetCalls() {
	g_fake.client_slots.clear();
	g_fake.authorized_slots.clear();
	g_fake.slot_keys.clear();
	g_fake.int_keys.clear();
}

bool AllClientSlotsAre(int slot) {
	for (const int queried : g_fake.client_slots) {
		if (queried != slot) {
			return false;
		}
	}
	return !g_fake.client_slots.empty();
}

bool Contains(const std::vector<std::string>& values, std::string_view wanted) {
	for (const std::string& value : values) {
		if (value == wanted) {
			return true;
		}
	}
	return false;
}

bool ContainsSlot(const std::vector<int>& values, int wanted) {
	for (const int value : values) {
		if (value == wanted) {
			return true;
		}
	}
	return false;
}

// --- the fake s2sdk / levels_ranks bindings ------------------------------------------------------

// Registered state, as the plugify module would hold it.
s2sdk::EventCallback g_hook_callback = nullptr;
std::string g_hook_name;
s2sdk::HookMode g_hook_mode = s2sdk::HookMode::Pre;
bool g_unhook_called = false;
s2sdk::EventCallback g_unhook_callback = nullptr;
s2sdk::HookMode g_unhook_mode = s2sdk::HookMode::Post;

s2sdk::OnMapStartCallback g_on_map_start = nullptr;
s2sdk::OnMapEndCallback g_on_map_end = nullptr;
bool g_map_start_registered = false;
bool g_map_end_registered = false;
bool g_map_start_unregistered = false;
bool g_map_end_unregistered = false;

s2sdk::OnClientDisconnectCallback g_disconnect = nullptr;
bool g_disconnect_registered = false;
bool g_disconnect_unregistered = false;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	g_hook_name = ToStd(name);
	g_hook_callback = callback;
	g_hook_mode = mode;
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& /*name*/, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	g_unhook_called = true;
	g_unhook_callback = callback;
	g_unhook_mode = mode;
	return s2sdk::EventHookError::Okay;
}

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_on_map_start = callback;
	g_map_start_registered = callback != nullptr;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_on_map_end = callback;
	g_map_end_registered = callback != nullptr;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback /*callback*/) {
	g_map_start_unregistered = true;
	g_on_map_start = nullptr;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback /*callback*/) {
	g_map_end_unregistered = true;
	g_on_map_end = nullptr;
}

void FakeOnClientDisconnectRegister(s2sdk::OnClientDisconnectCallback callback) {
	g_disconnect = callback;
	g_disconnect_registered = callback != nullptr;
}

void FakeOnClientDisconnectUnregister(s2sdk::OnClientDisconnectCallback /*callback*/) {
	g_disconnect_unregistered = true;
	g_disconnect = nullptr;
}

int32_t FakeGetEventInt(void* /*event*/, const plg::string& key) {
	const std::string name = ToStd(key);
	g_fake.int_keys.push_back(name);
	// Deliberately a different value for "attacker": if the plugin used this accessor as the slot,
	// the client checks below would see attacker_int instead of attacker_slot.
	return name == "attacker" ? g_fake.attacker_int : g_fake.field_int;
}

int32_t FakeGetEventPlayerSlot(void* /*event*/, const plg::string& key) {
	g_fake.slot_keys.push_back(ToStd(key));
	if (g_fake.throw_on_slot) {
		throw std::runtime_error("fake GetEventPlayerSlot failure");
	}
	return g_fake.attacker_slot;
}

bool FakeIsClientConnected(int32_t slot) {
	g_fake.client_slots.push_back(static_cast<int>(slot));
	return g_fake.connected;
}

bool FakeIsClientInGame(int32_t slot) {
	g_fake.client_slots.push_back(static_cast<int>(slot));
	return g_fake.in_game;
}

bool FakeIsClientSourceTV(int32_t slot) {
	g_fake.client_slots.push_back(static_cast<int>(slot));
	return g_fake.source_tv;
}

bool FakeIsFakeClient(int32_t slot) {
	g_fake.client_slots.push_back(static_cast<int>(slot));
	return g_fake.fake_client;
}

bool FakeIsClientAuthorized(int32_t slot) {
	g_fake.client_slots.push_back(static_cast<int>(slot));
	g_fake.authorized_slots.push_back(static_cast<int>(slot));
	return g_fake.authorized;
}

plg::string FakeGetClientAuthId(int32_t slot) {
	g_fake.client_slots.push_back(static_cast<int>(slot));
	return plg::string("STEAM_1:0:12345");
}

bool FakeIsCoreLoaded() {
	// The plugin must not reach OnCoreIsReady() in this test: that path opens the plgdb connection
	// and needs the LR table name/database settings. The binding being non-null is what makes
	// levels_ranks::LrApi::Available() true, which the other fakes rely on.
	return false;
}

bool FakeCheckCountPlayers() {
	return g_fake.count_players;
}

int32_t FakeGetSettingsValue(int32_t /*setting*/) {
	// LR_DB_SaveDataPlayer_Mode = 0: SaveData() is only called from the disconnect path.
	return 0;
}

// The plugify logger the plugin uses (plg::Log is a plain function pointer the host fills).
std::string g_last_error;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	if (severity == plg::Severity::Error) {
		g_last_error.assign(message);
	}
}

// Forgets everything the fakes recorded, so each start/teardown phase is asserted on its own.
void ResetRegistrations() {
	g_hook_callback = nullptr;
	g_hook_name.clear();
	g_hook_mode = s2sdk::HookMode::Pre;
	g_unhook_called = false;
	g_unhook_callback = nullptr;
	g_unhook_mode = s2sdk::HookMode::Post;
	g_on_map_start = nullptr;
	g_on_map_end = nullptr;
	g_map_start_registered = false;
	g_map_end_registered = false;
	g_map_start_unregistered = false;
	g_map_end_unregistered = false;
	g_disconnect = nullptr;
	g_disconnect_registered = false;
	g_disconnect_unregistered = false;
	g_last_error.clear();
}

void InstallFakes() {
	ResetRegistrations();
	plg::Log = &FakeLog;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_OnClientDisconnect_Register = &FakeOnClientDisconnectRegister;
	__s2sdk_OnClientDisconnect_Unregister = &FakeOnClientDisconnectUnregister;
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_GetClientAuthId = &FakeGetClientAuthId;

	// The plugin's LrApi wrapper reports "levels_ranks is installed" for every non-null slot.
	__levels_ranks_IsCoreLoaded = &FakeIsCoreLoaded;
	__levels_ranks_CheckCountPlayers = &FakeCheckCountPlayers;
	__levels_ranks_GetSettingsValue = &FakeGetSettingsValue;
}

// --- driving the plugin --------------------------------------------------------------------------

// A non-null pointer the plugin only ever compares and passes back to the fakes above.
int g_event_storage = 0;

s2sdk::ResultType FirePlayerHurt() {
	ResetCalls();
	if (g_hook_callback == nullptr) {
		CHECK(false, "the plugin registered a player_hurt callback");
		return s2sdk::ResultType::Stop;
	}
	return g_hook_callback(plg::string("player_hurt"), &g_event_storage, false);
}

void FireMapStart() {
	if (g_on_map_start != nullptr) {
		g_on_map_start();
	}
}

void FireMapEnd() {
	if (g_on_map_end != nullptr) {
		g_on_map_end();
	}
}

void PumpUpdate() {
	Plugify_PluginUpdate(std::chrono::milliseconds(0));
}

// A fresh fake state: no calls recorded, GetEventPlayerSlot answers -1 (world) until a case sets it,
// and every client check passes except IsClientAuthorized - so a case that does not ask for the
// authenticated path stops right after the five client checks.
void ResetFake() {
	g_fake = FakeState{};
}

// --- tests ---------------------------------------------------------------------------------------

// A build whose s2sdk lacks the player-slot accessor must not hook the event at all: one clear log
// line at start instead of a handler that silently never counts anything.
void TestStartWithoutAccessor() {
	InstallFakes();
	__s2sdk_GetEventPlayerSlot = nullptr;
	const plg::PluginResult result = Plugify_PluginStart();
	CHECK(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart still answers Ok without the accessor");
	CHECK(g_hook_callback == nullptr, "no player_hurt hook is registered without the accessor");
	CHECK(g_last_error.find("accessors") != std::string::npos,
		  "the missing accessor is reported once at start");
	Plugify_PluginEnd();
}

void TestStart() {
	InstallFakes();
	const plg::PluginResult result = Plugify_PluginStart();
	CHECK(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart answers Ok (never Failed, PITFALLS A1)");
	CHECK(g_map_start_registered && g_map_end_registered, "the plugin registers both map listeners");
	CHECK_STR(g_hook_name, "player_hurt", "the plugin hooks player_hurt");
	CHECK(g_hook_mode == s2sdk::HookMode::Pre, "the hook is a Pre hook (PITFALLS C12)");
	CHECK(g_hook_callback != nullptr, "the registered callback is the plugin's own");
	CHECK(g_disconnect_registered, "the plugin registers the disconnect listener");
}

// PITFALLS B1: a plugin loaded into a running map never sees OnMapStart; the first OnPluginUpdate
// is its proof of a live map, and only then may the callback touch a client.
void TestMapGateOpensOnFirstUpdate() {
	ResetFake();
	g_fake.attacker_slot = 5;
	g_fake.attacker_int = 9;
	const s2sdk::ResultType before = FirePlayerHurt();
	CHECK(before == s2sdk::ResultType::Continue, "a closed gate still answers Continue");
	CHECK(g_fake.client_slots.empty(), "no client call before the first update");
	CHECK(g_fake.slot_keys.empty(), "and no event field is read either");

	PumpUpdate();
	g_fake.attacker_slot = 5;
	FirePlayerHurt();
	CHECK_EQ(static_cast<int>(g_fake.client_slots.size()), 5,
			"the first update opens the gate: connected/in-game/SourceTV/bot/authorized are asked");
	CHECK(AllClientSlotsAre(5), "all client checks use the attacker's slot");
}

// PITFALLS B2: from OnMapEnd until the next OnMapStart no client call may run - not even while the
// host keeps pumping OnPluginUpdate through the changelevel.
void TestMapEndClosesTheGate() {
	FireMapEnd();
	ResetFake();
	g_fake.attacker_slot = 5;
	g_fake.authorized = true;
	FirePlayerHurt();
	CHECK(g_fake.client_slots.empty(), "no client call in the map-end window");

	ResetFake();
	g_fake.attacker_slot = 5;
	PumpUpdate(); // a host update during the changelevel must not re-open the gate
	FirePlayerHurt();
	CHECK(g_fake.client_slots.empty(), "an OnPluginUpdate does not re-open the gate after OnMapEnd");
	CHECK(g_fake.slot_keys.empty(), "and the event is not even read");

	FireMapStart();
	ResetFake();
	g_fake.attacker_slot = 5;
	FirePlayerHurt();
	CHECK(AllClientSlotsAre(5), "the next OnMapStart opens the gate again");
}

// The accessor fix: the slot comes from GetEventPlayerSlot, never from GetEventInt.
void TestAttackerIsThePlayerSlot() {
	ResetFake();
	g_fake.attacker_slot = 3;
	g_fake.attacker_int = 7; // what the raw GetInt of the field would answer
	g_fake.authorized = false;
	const s2sdk::ResultType result = FirePlayerHurt();
	CHECK(result == s2sdk::ResultType::Continue, "the callback answers Continue");

	CHECK_EQ(static_cast<int>(g_fake.slot_keys.size()), 1, "exactly one player field is resolved");
	if (!g_fake.slot_keys.empty()) {
		CHECK_STR(g_fake.slot_keys.front(), "attacker", "it is the attacker field");
	}
	CHECK(!Contains(g_fake.int_keys, "attacker"),
		  "GetEventInt is never asked for the attacker field");
	CHECK_EQ(static_cast<int>(g_fake.client_slots.size()), 5, "the client checks run once");
	CHECK(AllClientSlotsAre(3), "every client check uses the GetEventPlayerSlot value (3)");
	CHECK(!ContainsSlot(g_fake.client_slots, 7), "no client check uses the GetEventInt value (7)");
	CHECK_EQ(static_cast<int>(g_fake.authorized_slots.size()), 1, "the authentication check ran once");
	CHECK(!ContainsSlot(g_fake.authorized_slots, 7), "it was asked about the slot, not the user id");
}

// Slot 0 is a normal player: it must reach the client checks (only -1 is "nobody").
void TestSlotZeroIsAPlayer() {
	ResetFake();
	g_fake.attacker_slot = 0;
	g_fake.attacker_int = 7;
	FirePlayerHurt();
	CHECK_EQ(static_cast<int>(g_fake.client_slots.size()), 5, "slot 0 runs the client checks");
	CHECK(AllClientSlotsAre(0), "all of them use slot 0");
	CHECK_EQ(static_cast<int>(g_fake.authorized_slots.size()), 1, "slot 0 is authenticated");
}

// World damage: the field has no player, s2sdk answers INVALID_PLAYER_SLOT_INDEX (-1).
void TestWorldAttackerIsDropped() {
	ResetFake();
	g_fake.attacker_slot = -1; // INVALID_PLAYER_SLOT_INDEX
	g_fake.attacker_int = 7;
	g_fake.authorized = true;
	const s2sdk::ResultType result = FirePlayerHurt();
	CHECK(result == s2sdk::ResultType::Continue, "world damage still answers Continue");
	CHECK(g_fake.client_slots.empty(), "world damage never reaches a client call");
	CHECK(g_fake.int_keys.empty(), "world damage never reads the damage fields");

	// An out-of-range slot is dropped the same way (the original wrote out of bounds here).
	ResetFake();
	g_fake.attacker_slot = 64;
	FirePlayerHurt();
	CHECK(g_fake.client_slots.empty(), "slot 64 never reaches a client call");
}

// PITFALLS C2: the s2sdk wrappers call their binding slots unchecked, so a missing one must be a
// no-op inside the callback instead of a null call.
void TestMissingBindings() {
	// GetEventPlayerSlot missing: the attacker cannot be resolved, so nothing may happen.
	g_fake = FakeState{};
	g_fake.attacker_slot = 3;
	g_fake.attacker_int = 7;
	__s2sdk_GetEventPlayerSlot = nullptr;
	const s2sdk::ResultType no_slot = FirePlayerHurt();
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	CHECK(no_slot == s2sdk::ResultType::Continue, "a missing GetEventPlayerSlot still answers Continue");
	CHECK(g_fake.client_slots.empty(), "no client call without the player binding");
	CHECK(g_fake.int_keys.empty(), "and no field is read through GetEventInt either");

	// GetEventInt missing: the damage fields cannot be read, so the whole handler is skipped.
	g_fake = FakeState{};
	g_fake.attacker_slot = 3;
	__s2sdk_GetEventInt = nullptr;
	const s2sdk::ResultType no_int = FirePlayerHurt();
	__s2sdk_GetEventInt = &FakeGetEventInt;
	CHECK(no_int == s2sdk::ResultType::Continue, "a missing GetEventInt still answers Continue");
	CHECK(g_fake.client_slots.empty(), "no client call without the int binding");
	CHECK(g_fake.slot_keys.empty(), "the event is not read without the int binding");
}

// A binding that throws must not unwind into the engine (the callback catches everything).
void TestThrowingBinding() {
	g_fake = FakeState{};
	g_fake.attacker_slot = 3;
	g_fake.throw_on_slot = true;
	const s2sdk::ResultType result = FirePlayerHurt();
	g_fake.throw_on_slot = false;
	CHECK(result == s2sdk::ResultType::Continue, "a throwing binding still answers Continue");
	CHECK(g_fake.client_slots.empty(), "a throwing binding makes no client call");
}

// The LR core gate: warmup or too few players short-circuits the whole callback.
void TestStatisticsOff() {
	g_fake = FakeState{};
	g_fake.attacker_slot = 3;
	g_fake.count_players = false;
	FirePlayerHurt();
	CHECK(g_fake.client_slots.empty(), "no client call while the statistics are off");
	CHECK(g_fake.slot_keys.empty(), "the event is not read while the statistics are off");
}

// An authenticated client whose row is not in memory yet: the hit must not be counted (and no
// damage field is read), but the path still ends in Continue.
void TestAuthenticatedButNotLoaded() {
	g_fake = FakeState{};
	g_fake.attacker_slot = 3;
	g_fake.attacker_int = 7;
	g_fake.authorized = true;
	const s2sdk::ResultType result = FirePlayerHurt();
	CHECK(result == s2sdk::ResultType::Continue, "the not-loaded path answers Continue");
	CHECK(AllClientSlotsAre(3), "it still resolved the slot through the player accessor");
	CHECK(g_fake.int_keys.empty(), "no damage field is read while the row is not loaded");
}

// PITFALLS A7: everything the plugin registered is removed again.
void TestEnd() {
	const plg::PluginResult result = Plugify_PluginEnd();
	CHECK(result.GetCode() == plg::PluginCode::Ok, "OnPluginEnd answers Ok");
	CHECK(g_unhook_called, "the player_hurt hook is removed again");
	CHECK(g_unhook_callback == g_hook_callback, "with the callback the plugin registered");
	CHECK(g_unhook_mode == s2sdk::HookMode::Pre, "and the same Pre mode");
	CHECK(g_map_start_unregistered && g_map_end_unregistered, "both map listeners are removed");
	CHECK(g_disconnect_unregistered, "the disconnect listener is removed");
	CHECK(g_on_map_start == nullptr && g_on_map_end == nullptr && g_disconnect == nullptr,
		  "no callback of the unloaded library stays registered");
}

} // namespace

int main() {
	std::printf("lr_hits_event_tests\n");

	TestStartWithoutAccessor();
	TestStart();
	TestMapGateOpensOnFirstUpdate();
	TestMapEndClosesTheGate();
	TestAttackerIsThePlayerSlot();
	TestSlotZeroIsAPlayer();
	TestWorldAttackerIsDropped();
	TestMissingBindings();
	TestThrowingBinding();
	TestStatisticsOff();
	TestAuthenticatedButNotLoaded();
	TestEnd();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
