#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>


#include "../core/boss_registry.h"
#include "../core/bosses.h"

#include "../core/player_manager.h"
#include "../core/settings.h"
#include "../core/shared.h"
#include "../core/tab_config.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "../providers/common/provider_registry.h"
#include "../providers/wingman/wingman_client.h"
#include "../providers/wingman/wingman_types.h"


#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui_extensions.h"
#include <Windows.h>


static ImGuiWindowFlags windowFlags = (ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_AlwaysAutoResize);
static ImGuiTableFlags tableFlags = (ImGuiTableFlags_Borders | ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY);

static std::vector<std::string> GetDataSources() {
	return BossRegistry::Instance().GetAvailableProviders();
}

namespace {
	static float bezier_ease(float t) {

		return t * t * (3.0f - 2.0f * t);
	}

	static float lerp(float x0, float x1, float t) {
		return (1.0f - t) * x0 + t * x1;
	}

	static float sawtooth(float t, int periods) {
		return ImFmod(((float) periods) * t, 1.0f);
	}

	static float interval(float t, float t0, float t1) {
		return t < t0 ? 0.0f : t > t1 ? 1.0f
									  : bezier_ease((t - t0) / (t1 - t0));
	}

	constexpr const char* WINGMAN_PROVIDER_NAME = "Wingman";
	constexpr std::chrono::minutes WINGMAN_RERANK_COOLDOWN(10);

	struct WingmanRerankState {
		bool pending = false;
		std::chrono::steady_clock::time_point lastRequestTime {};
		std::string lastNote;
	};

	static Wingman::WingmanClient wingmanClient;
	static std::unordered_map<std::string, WingmanRerankState> wingmanRerankStates;
	static std::unordered_set<std::string> wingmanExpandedPlayers;
	static std::mutex wingmanRerankMutex;
	static const std::array<const char*, 5> wingmanRankCategories = {"Damage", "Mechanics", "Speed", "Support", "Teamplay"};

	static bool ShouldShowWingmanRankColumn(const std::string& providerName) {
		return providerName == WINGMAN_PROVIDER_NAME && Settings::ShowWingmanRankEntity;
	}

	static bool ShouldShowWingmanBreakdown(const std::string& providerName) {
		return ShouldShowWingmanRankColumn(providerName) && Settings::ShowWingmanRankBreakdown;
	}

	static const Wingman::WingmanPlayerData* GetWingmanPlayerData(const PlayerProofData* proofData) {
		if (!proofData || !proofData->rawData.has_value()) {
			return nullptr;
		}

		try {
			return &std::any_cast<const Wingman::WingmanPlayerData&>(proofData->rawData);
		} catch (const std::bad_any_cast&) {
			return nullptr;
		}
	}

	static const Wingman::WingmanRankResponse* GetWingmanRankResponse(const PlayerProofData* proofData) {
		const auto* wingmanData = GetWingmanPlayerData(proofData);
		return wingmanData ? &wingmanData->rank : nullptr;
	}

	static bool HasWingmanRankDisplayData(const PlayerProofData* proofData) {
		const auto* rankData = GetWingmanRankResponse(proofData);
		if (!rankData) {
			return false;
		}

		return !rankData->globalRank.empty() || !rankData->note.empty() || !rankData->bestPerBoss.empty();
	}

	static bool HasWingmanBreakdownForGroup(const Wingman::WingmanRankResponse* rankData, const BossGroup& group) {
		if (!rankData) {
			return false;
		}

		for (const auto& bossId : group.cachedBossIds) {
			if (rankData->bestPerBoss.contains(bossId)) {
				return true;
			}
		}

		return false;
	}

	static std::string BuildWingmanBreakdownCellText(const std::map<std::string, std::string>& bossRanks) {
		std::string text;
		for (const char* category : wingmanRankCategories) {
			auto it = bossRanks.find(category);
			if (it == bossRanks.end()) {
				continue;
			}
			if (!text.empty()) {
				text += "\n";
			}
			text += it->second;
		}
		return text;
	}

	static void DrawWingmanRankTooltip(const Wingman::WingmanRankResponse& rankData) {
		ImGui::BeginTooltip();
		if (!rankData.globalRank.empty()) {
			ImGui::Text("Global Rank: %s", rankData.globalRank.c_str());
		}
		if (rankData.bossesCompleted > 0) {
			ImGui::Text("Bosses Completed: %d", rankData.bossesCompleted);
		}
		for (const char* category : wingmanRankCategories) {
			auto it = rankData.ranksByCategory.find(category);
			if (it != rankData.ranksByCategory.end()) {
				ImGui::Text("%s: %s", category, it->second.c_str());
			}
		}
		if (!rankData.note.empty()) {
			ImGui::Separator();
			ImGui::TextWrapped("%s", rankData.note.c_str());
		}
		ImGui::EndTooltip();
	}

	static void TriggerWingmanRerank(const std::string& account) {
		{
			std::scoped_lock lock(wingmanRerankMutex);
			auto& state = wingmanRerankStates[account];
			state.pending = true;
			state.lastRequestTime = std::chrono::steady_clock::now();
			state.lastNote.clear();
		}

		wingmanClient.RatePerformanceForCurrentPatchAsync(account, [account](const Wingman::WingmanRankResponse& response) {
			{
				std::scoped_lock lock(wingmanRerankMutex);
				auto& state = wingmanRerankStates[account];
				state.pending = false;
				state.lastNote = response.note;
			}

			if (response.success || !response.note.empty() || response.bossesCompleted > 0 || !response.globalRank.empty()) {
				PlayerManager::lazyLoadManager.ClearPlayerData(account, WINGMAN_PROVIDER_NAME);
				PlayerManager::lazyLoadManager.RequestPlayerData(account, WINGMAN_PROVIDER_NAME);
			}
		});
	}
} // namespace

static void DrawSpinner() {
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems) return;

	ImGuiContext& g = *ImGui::GetCurrentContext();
	const ImGuiStyle& style = g.Style;

	float radius = 6.0f;
	int thickness = 2;
	ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);

	ImVec2 pos = window->DC.CursorPos;
	ImVec2 size(radius * 2, radius * 2);
	const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
	ImGui::ItemSize(bb);

	const ImVec2 center = ImVec2(pos.x + radius, pos.y + radius);
	const float period = 8.0f;
	const float t = ImFmod((float) g.Time, period) / period;

	const int num_detents = 5;
	const float t_saw = sawtooth(t, num_detents);
	const float head_value = interval(t_saw, 0.0f, 0.5f);
	const float tail_value = interval(t_saw, 0.5f, 1.0f);
	const float rotation_value = sawtooth(t, num_detents);

	const float min_arc = 30.0f / 360.0f * 2.0f * IM_PI;
	const float max_arc = 270.0f / 360.0f * 2.0f * IM_PI;
	const float start_angle = -IM_PI / 2.0f;

	const float a_min = start_angle + tail_value * max_arc + rotation_value * 2.0f * IM_PI;
	const float a_max = a_min + (head_value - tail_value) * max_arc + min_arc;

	window->DrawList->PathClear();
	for (int i = 0; i < 24; i++) {
		const float a = a_min + ((float) i / 24.0f) * (a_max - a_min);
		window->DrawList->PathLineTo(ImVec2(center.x + ImCos(a) * radius, center.y + ImSin(a) * radius));
	}
	window->DrawList->PathStroke(color, false, (float) thickness);
}

static void DrawPlayerAccountName(const Player& player, const auto* proofData) {
	if (proofData && !proofData->profileUrl.empty()) {
		if (ImGui::TextURL(player.account.c_str())) {
			ShellExecuteA(0, 0, proofData->profileUrl.c_str(), 0, 0, SW_SHOW);
		}
	} else {
		ImGui::Text(player.account.c_str());
	}
}

static void DrawPlayerProofValue(const Player& player, const std::string& proofId, const std::string& providerName) {
	if (player.state == LoadState::READY && player.proofData) {
		auto it = player.proofData->proofs.find(proofId);
		ImGui::Text(it != player.proofData->proofs.end() ? std::to_string(it->second.amount).c_str() : "0");
	} else {
		if (player.state == LoadState::LOADING) {
			DrawSpinner();
		} else {
			ImGui::Text("0");
		}
	}
}

static void DrawKpmeId(const Player& aPlayer, const auto* proofData) {
	if (proofData && !proofData->profileId.empty()) {
		if (ImGui::TextURL(proofData->profileId.c_str())) {
			ImGui::SetClipboardText(proofData->profileId.c_str());
		}
	} else {
		ImGui::Text("0");
	}
}

static void HighlightColumnOnHover() {
	if (!Settings::hoverEnabled)
		return;
	if (ImGui::TableGetColumnFlags(ImGui::TableGetColumnIndex()) & ImGuiTableColumnFlags_IsHovered) {
		ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, Settings::hoverColour);
	}
}

static void HighlightRowOnHover(ImGuiTable* table) {
	if (!Settings::hoverEnabled)
		return;
	ImRect rowRect(
			table->WorkRect.Min.x,
			table->RowPosY1,
			table->WorkRect.Max.x,
			table->RowPosY2
	);
	rowRect.ClipWith(table->BgClipRect);

	bool rowHovered = (ImGui::IsMouseHoveringRect(rowRect.Min, rowRect.Max, false) && ImGui::IsWindowHovered(ImGuiHoveredFlags_None) && !ImGui::IsAnyItemHovered());

	if (rowHovered) {
		table->RowBgColor[1] = ImGui::GetColorU32(Settings::hoverColour);
	}
}

static void SetupTableColumns(const BossGroup& group, bool showKpmeId, const std::string& providerName) {
	static std::vector<std::pair<std::string, float>> columnSpecs;
	columnSpecs.clear();

	columnSpecs.emplace_back("Account", Settings::ColumnSizeAccount);
	if (showKpmeId) {
		columnSpecs.emplace_back("Id", Settings::ColumnSizeKpmeId);
	}
	for (const auto& currency : group.currencies) {
		columnSpecs.emplace_back(currency, Settings::ColumnSizeBosses);
	}
	for (const auto& bossEntry : group.bosses) {
		static std::unordered_map<std::string, std::string> bossNameCache;
		std::string key = std::to_string(int(bossEntry.boss)) + "_" + std::to_string(int(bossEntry.type));
		auto it = bossNameCache.find(key);
		if (it == bossNameCache.end()) {
			it = bossNameCache.emplace(key, GetBossName(bossEntry.boss, bossEntry.type)).first;
		}
		columnSpecs.emplace_back(it->second, Settings::ColumnSizeBosses);
	}
	if (ShouldShowWingmanRankColumn(providerName)) {
		columnSpecs.emplace_back("Rank", 90.0f);
	}


	for (size_t i = 0; i < columnSpecs.size(); ++i) {
		ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_WidthFixed;
		if (i == 0) flags |= ImGuiTableColumnFlags_NoHide; // Account column
		else if (i > (showKpmeId ? 1 : 0))
			flags |= ImGuiTableColumnFlags_NoResize; // Boss/currency columns
		ImGui::TableSetupColumn(columnSpecs[i].first.c_str(), flags, columnSpecs[i].second);
	}
}

static void DrawTableHeaders(const BossGroup& group, bool showKpmeId, const std::string& providerName) {
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	ImGui::TableNextColumn();
	ImGui::Text("Account");
	if (showKpmeId) {
		ImGui::TableNextColumn();
		ImGui::Text("Id");
	}
	for (const auto& currency : group.currencies) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		Texture* texture = GetCurrencyTexture(currency);
		if (texture) {
			float columnWidth = Settings::ColumnSizeBosses;
			float iconSize = Settings::BossIconScale;
			float padding = (columnWidth - iconSize) * 0.5f;
			if (padding > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padding);
			ImGui::Image((void*) texture->Resource, ImVec2(iconSize, iconSize));
		} else {
			ImGui::Text(currency.c_str());
		}
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::Text(currency.c_str());
			ImGui::EndTooltip();
		}
	}
	for (const auto& bossEntry : group.bosses) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		Texture* texture = GetBossTexture(bossEntry.boss);
		static std::unordered_map<std::string, std::string> bossNameCache;
		std::string key = std::to_string(int(bossEntry.boss)) + "_" + std::to_string(int(bossEntry.type));
		auto it = bossNameCache.find(key);
		if (it == bossNameCache.end()) {
			it = bossNameCache.emplace(key, GetBossName(bossEntry.boss, bossEntry.type)).first;
		}
		const std::string& bossName = it->second;
		if (texture) {
			float columnWidth = Settings::ColumnSizeBosses;
			float iconSize = Settings::BossIconScale;
			float padding = (columnWidth - iconSize) * 0.5f;
			if (padding > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padding);
			ImGui::Image((void*) texture->Resource, ImVec2(iconSize, iconSize));
		} else {
			ImGui::Text(bossName.c_str());
		}
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::Text(bossName.c_str());
			ImGui::EndTooltip();
		}
	}
	if (ShouldShowWingmanRankColumn(providerName)) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		ImGui::Text("Rank");
	}
}

static void DrawWingmanRankCell(const Player& player, const BossGroup& group, const PlayerProofData* proofData, LoadState lazyState) {
	const auto* rankData = GetWingmanRankResponse(proofData);
	if (!rankData) {
		if (lazyState == LoadState::LOADING) {
			DrawSpinner();
		} else {
			ImGui::Text("-");
		}
		return;
	}

	if (rankData->success && rankData->enoughData && !rankData->globalRank.empty()) {
		ImGui::Text("%s", rankData->globalRank.c_str());
		if (ImGui::IsItemHovered()) {
			DrawWingmanRankTooltip(*rankData);
		}
		return;
	}

	if (!rankData->note.empty()) {
		ImGui::Text("N/A");
		if (ImGui::IsItemHovered()) {
			DrawWingmanRankTooltip(*rankData);
		}

		const bool canRerank = !rankData->enoughData;
		if (canRerank) {
			WingmanRerankState rerankState;
			{
				std::scoped_lock lock(wingmanRerankMutex);
				auto it = wingmanRerankStates.find(player.account);
				if (it != wingmanRerankStates.end()) {
					rerankState = it->second;
				}
			}

			const auto now = std::chrono::steady_clock::now();
			const bool isCoolingDown = !rerankState.pending && rerankState.lastRequestTime != std::chrono::steady_clock::time_point {} && (now - rerankState.lastRequestTime) < WINGMAN_RERANK_COOLDOWN;

			ImGui::PushID((group.tableName + player.account + "rerank").c_str());
			const bool disableRerankButton = rerankState.pending || isCoolingDown;
			if (disableRerankButton) {
				ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
			}
			if (ImGui::SmallButton("Rerank")) {
				TriggerWingmanRerank(player.account);
			}
			if (disableRerankButton) {
				ImGui::PopStyleVar();
				ImGui::PopItemFlag();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				if (rerankState.pending) {
					ImGui::Text("Rerank in progress.");
				} else if (isCoolingDown) {
					auto remaining = std::chrono::duration_cast<std::chrono::minutes>(WINGMAN_RERANK_COOLDOWN - (now - rerankState.lastRequestTime)).count();
					ImGui::Text("Cooldown active: %lld minute(s) remaining.", remaining + 1);
				} else if (!rerankState.lastNote.empty()) {
					ImGui::TextWrapped("%s", rerankState.lastNote.c_str());
				} else {
					ImGui::Text("Ask Wingman to calculate ranks for this player.");
				}
				ImGui::EndTooltip();
			}
			ImGui::PopID();
		}
		return;
	}

	if (lazyState == LoadState::LOADING) {
		DrawSpinner();
	} else {
		ImGui::Text("-");
	}
}

static void DrawWingmanBreakdownRow(const BossGroup& group, bool showKpmeId, const PlayerProofData* proofData) {
	const auto* rankData = GetWingmanRankResponse(proofData);
	if (!rankData || !HasWingmanBreakdownForGroup(rankData, group)) {
		return;
	}

	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextDisabled("Best");

	if (showKpmeId) {
		ImGui::TableNextColumn();
		ImGui::Text("");
	}

	for (size_t i = 0; i < group.cachedCurrencyIds.size(); ++i) {
		ImGui::TableNextColumn();
		ImGui::Text("");
	}

	for (const auto& bossId : group.cachedBossIds) {
		ImGui::TableNextColumn();
		auto it = rankData->bestPerBoss.find(bossId);
		if (it == rankData->bestPerBoss.end()) {
			ImGui::Text("");
			continue;
		}

		std::string cellText = BuildWingmanBreakdownCellText(it->second);
		ImGui::TextUnformatted(cellText.c_str());
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			for (const char* category : wingmanRankCategories) {
				auto categoryIt = it->second.find(category);
				if (categoryIt != it->second.end()) {
					ImGui::Text("%s: %s", category, categoryIt->second.c_str());
				}
			}
			ImGui::EndTooltip();
		}
	}

	if (ShouldShowWingmanRankColumn(WINGMAN_PROVIDER_NAME)) {
		ImGui::TableNextColumn();
		ImGui::Text("");
	}

	HighlightRowOnHover(ImGui::GetCurrentContext()->CurrentTable);
}

static void DrawPlayerRow(const Player& p, const BossGroup& group, IBossProvider* provider, bool showKpmeId, const std::string& providerName) {

	group.InitializeCache(provider);


	auto lazyState = PlayerManager::lazyLoadManager.GetPlayerState(p.account, providerName);
	auto lazyData = PlayerManager::lazyLoadManager.GetPlayerData(p.account, providerName);
	const auto* rawProofData = (lazyState == LoadState::READY && lazyData) ? lazyData.get() : nullptr;


	std::unique_ptr<PlayerProofData> computedData;
	if (rawProofData && rawProofData->rawData.has_value() && providerName == "KPME") {
		auto dataProvider = ProviderRegistry::Instance().CreateProvider(providerName);
		if (dataProvider && dataProvider->SupportsLinkedAccounts()) {
			bool includeLinked = (Settings::LinkedAccountsMode == COMBINE_LINKED);
			computedData = std::make_unique<PlayerProofData>(dataProvider->ComputeProofsFromRawData(*rawProofData, includeLinked));
		}
	}
	const auto* proofData = computedData ? computedData.get() : rawProofData;
	const auto* wingmanRankData = GetWingmanRankResponse(proofData);
	const bool canExpandWingmanBreakdown = ShouldShowWingmanBreakdown(providerName) && HasWingmanBreakdownForGroup(wingmanRankData, group);
	const std::string expansionKey = group.tableName + "|" + p.account;
	bool isWingmanExpanded = false;
	const bool hasAnyDisplayData = proofData && (!proofData->proofs.empty() || (ShouldShowWingmanRankColumn(providerName) && HasWingmanRankDisplayData(proofData)));

	if (canExpandWingmanBreakdown) {
		isWingmanExpanded = wingmanExpandedPlayers.contains(expansionKey);
	}

	if (!hasAnyDisplayData) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
	}

	ImGui::TableNextColumn();
	if (canExpandWingmanBreakdown) {
		ImGui::PushID(expansionKey.c_str());
		if (ImGui::SmallButton(isWingmanExpanded ? "-" : "+")) {
			if (isWingmanExpanded) {
				wingmanExpandedPlayers.erase(expansionKey);
				isWingmanExpanded = false;
			} else {
				wingmanExpandedPlayers.insert(expansionKey);
				isWingmanExpanded = true;
			}
		}
		ImGui::PopID();
		ImGui::SameLine();
	}
	DrawPlayerAccountName(p, proofData);
	if (showKpmeId) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		DrawKpmeId(p, proofData);
	}

	bool isDisabled = !hasAnyDisplayData;

	for (size_t i = 0; i < group.cachedCurrencyIds.size(); ++i) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		if (proofData) {
			auto it = proofData->proofs.find(group.cachedCurrencyIds[i]);
			ImGui::Text(it != proofData->proofs.end() ? std::to_string(it->second.amount).c_str() : (isDisabled ? "" : "0"));
		} else {
			if (lazyState == LoadState::LOADING) {
				DrawSpinner();
			} else {
				ImGui::Text(isDisabled ? "" : "0");
			}
		}
	}
	for (size_t i = 0; i < group.cachedBossIds.size(); ++i) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		if (proofData) {
			auto it = proofData->proofs.find(group.cachedBossIds[i]);
			ImGui::Text(it != proofData->proofs.end() ? std::to_string(it->second.amount).c_str() : (isDisabled ? "" : "0"));
		} else {
			if (lazyState == LoadState::LOADING) {
				DrawSpinner();
			} else {
				ImGui::Text(isDisabled ? "" : "0");
			}
		}
	}
	if (ShouldShowWingmanRankColumn(providerName)) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		DrawWingmanRankCell(p, group, proofData, lazyState);
	}

	if (!hasAnyDisplayData) {
		ImGui::PopStyleColor();
	}

	HighlightRowOnHover(ImGui::GetCurrentContext()->CurrentTable);

	if (isWingmanExpanded) {
		DrawWingmanBreakdownRow(group, showKpmeId, proofData);
	}


	if (Settings::LinkedAccountsMode == SPLIT_LINKED && proofData && !proofData->linkedAccounts.empty()) {
		for (const auto& linkedAccount : proofData->linkedAccounts) {
			ImGui::TableNextColumn();
			ImGui::Indent(20.0f);
			ImGui::Text(linkedAccount.accountName.c_str());
			ImGui::Unindent(20.0f);

			if (showKpmeId) {
				ImGui::TableNextColumn();
				HighlightColumnOnHover();
				ImGui::Text("-");
			}

			for (size_t i = 0; i < group.cachedCurrencyIds.size(); ++i) {
				ImGui::TableNextColumn();
				HighlightColumnOnHover();
				auto it = linkedAccount.proofs.find(group.cachedCurrencyIds[i]);
				ImGui::Text(it != linkedAccount.proofs.end() ? std::to_string(it->second.amount).c_str() : "0");
			}

			for (size_t i = 0; i < group.cachedBossIds.size(); ++i) {
				ImGui::TableNextColumn();
				HighlightColumnOnHover();
				auto it = linkedAccount.proofs.find(group.cachedBossIds[i]);
				ImGui::Text(it != linkedAccount.proofs.end() ? std::to_string(it->second.amount).c_str() : "0");
			}
			if (ShouldShowWingmanRankColumn(providerName)) {
				ImGui::TableNextColumn();
				ImGui::Text("");
			}

			HighlightRowOnHover(ImGui::GetCurrentContext()->CurrentTable);
		}
	}
}

static void DrawGenericTab(const BossGroup& group, IBossProvider* provider, const std::string& providerName, bool showKpmeId = false) {
	if (!ImGui::BeginTabItem(group.name.c_str())) return;
	int columnCount = static_cast<int>(group.currencies.size() + group.bosses.size()) + (showKpmeId ? 2 : 1) + (ShouldShowWingmanRankColumn(providerName) ? 1 : 0);
	if (ImGui::BeginTable(group.tableName.c_str(), columnCount, tableFlags)) {
		SetupTableColumns(group, showKpmeId, providerName);
		DrawTableHeaders(group, showKpmeId, providerName);

		static std::vector<const Player*> visiblePlayers;
		visiblePlayers.clear();

		{
			std::scoped_lock lck(PlayerManager::playerMutex);
			if (PlayerManager::players.empty()) {
				ImGui::TableNextColumn();
				ImGui::Text("No players found... ");
				ImGui::EndTable();
				ImGui::EndTabItem();
				return;
			}


			for (const auto& p : PlayerManager::players) {
				if (!Settings::IncludeMissingAccounts) {
					auto lazyState = PlayerManager::lazyLoadManager.GetPlayerState(p.account, providerName);
					if (lazyState == LoadState::READY) {
						auto lazyData = PlayerManager::lazyLoadManager.GetPlayerData(p.account, providerName);
						const bool hasVisibleWingmanRankData = ShouldShowWingmanRankColumn(providerName) && HasWingmanRankDisplayData(lazyData.get());
						if (!lazyData || (lazyData->proofs.empty() && !hasVisibleWingmanRankData)) {
							continue;
						}
					}
				}
				visiblePlayers.push_back(&p);
			}
		}


		for (const auto* player : visiblePlayers) {
			DrawPlayerRow(*player, group, provider, showKpmeId, providerName);
		}
		ImGui::EndTable();
	}
	ImGui::EndTabItem();
}


static void SaveWindowState() {
	Settings::Settings[WINDOW_LOG_PROOFS_KEY][SHOW_WINDOW_LOG_PROOFS] = Settings::ShowWindowLogProofs;
	Settings::Save(SettingsPath);
}

static void DrawProviderCombo(const std::string& currentProvider) {
	if (ImGui::BeginCombo("##DataSource", currentProvider.c_str())) {
		for (const auto& provider : GetDataSources()) {
			bool is_selected = (provider == currentProvider);
			if (ImGui::Selectable(provider.c_str(), is_selected)) {
				Settings::SelectedDataSource = (provider == "Wingman") ? WINGMAN : KPME;
				Settings::Settings[WINDOW_LOG_PROOFS_KEY][SELECTED_DATA_SOURCE] = Settings::SelectedDataSource;
				Settings::Save(SettingsPath);

				std::scoped_lock lck(PlayerManager::playerMutex);
				for (const auto& player : PlayerManager::players) {
					PlayerManager::lazyLoadManager.RequestPlayerData(player.account, provider);
				}
			}
			if (is_selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
}

static void DrawControls(const std::string& currentProvider) {
	DrawProviderCombo(currentProvider);
	if (currentProvider == "KPME") {
		ImGui::SameLine();
		const char* modeNames[] = {"Hide linked accounts", "Combine linked accounts", "Split linked accounts"};
		if (ImGui::BeginCombo("##LinkedMode", modeNames[Settings::LinkedAccountsMode])) {
			for (int i = 0; i < 3; i++) {
				bool is_selected = (Settings::LinkedAccountsMode == i);
				if (ImGui::Selectable(modeNames[i], is_selected)) {
					Settings::LinkedAccountsMode = static_cast<LinkedAccountMode>(i);
					Settings::Settings[WINDOW_LOG_PROOFS_KEY][LINKED_ACCOUNTS_MODE] = Settings::LinkedAccountsMode;
					Settings::Save(SettingsPath);
				}
				if (is_selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}
	if (currentProvider == WINGMAN_PROVIDER_NAME) {
		ImGui::SameLine();
		bool showRank = Settings::ShowWingmanRankEntity;
		if (ImGui::Checkbox("Rank", &showRank)) {
			Settings::ShowWingmanRankEntity = showRank;
			if (!showRank) {
				Settings::ShowWingmanRankBreakdown = false;
			}
			Settings::Settings[WINDOW_LOG_PROOFS_KEY][SHOW_WINGMAN_RANK_ENTITY] = Settings::ShowWingmanRankEntity;
			Settings::Settings[WINDOW_LOG_PROOFS_KEY][SHOW_WINGMAN_RANK_BREAKDOWN] = Settings::ShowWingmanRankBreakdown;
			Settings::Save(SettingsPath);
		}

		ImGui::SameLine();
		bool showBreakdown = Settings::ShowWingmanRankBreakdown;
		const bool disableBreakdownToggle = !Settings::ShowWingmanRankEntity;
		if (disableBreakdownToggle) {
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		}
		if (ImGui::Checkbox("Breakdown", &showBreakdown)) {
			Settings::ShowWingmanRankBreakdown = Settings::ShowWingmanRankEntity && showBreakdown;
			Settings::Settings[WINDOW_LOG_PROOFS_KEY][SHOW_WINGMAN_RANK_BREAKDOWN] = Settings::ShowWingmanRankBreakdown;
			Settings::Save(SettingsPath);
		}
		if (disableBreakdownToggle) {
			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		}
	}
}

void RenderWindowLogProofs() {
	static bool wasWindowOpen = false;
	bool isWindowOpen = Settings::ShowWindowLogProofs;


	if (isWindowOpen != wasWindowOpen) {
		PlayerManager::OnWindowStateChanged(isWindowOpen);
		if (!isWindowOpen) SaveWindowState();
		wasWindowOpen = isWindowOpen;
	}


	if (!Settings::ShowWindowLogProofs) {
		return;
	}
	ImGuiWindowFlags flags = windowFlags;
	if (Settings::WindowAutoResize) {
		flags |= ImGuiWindowFlags_AlwaysAutoResize;
	} else {
		flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
	}
	if (Settings::WindowRestrictSize) {
		ImGui::SetNextWindowSizeConstraints(ImVec2(Settings::MinWindowWidth, Settings::MinWindowHeight), ImVec2(Settings::MaxWindowWidth, Settings::MaxWindowHeight));
	}
	if (!ImGui::Begin("Log Proofs", &Settings::ShowWindowLogProofs, flags)) {
		ImGui::End();
		return;
	}
	std::string currentProvider = (Settings::SelectedDataSource == WINGMAN) ? "Wingman" : "KPME";
	DrawControls(currentProvider);
	IBossProvider* bossProvider = BossRegistry::Instance().GetProvider(currentProvider);
	if (bossProvider && ImGui::BeginTabBar(("##" + currentProvider).c_str(), ImGuiTabBarFlags_None)) {
		bool isKpme = (currentProvider == "KPME");


		Settings::EnsureProviderConfigExists(currentProvider);
		auto config = TabConfigManager::Instance().GetProviderConfig(currentProvider);

		if (config.useCustomTabs) {

			for (const auto& customTab : config.tabs) {
				if (customTab.visible && !customTab.displayName.empty()) {
					try {
						auto bossGroup = bossProvider->CreateCustomBossGroup(customTab);

						if (!bossGroup.bosses.empty() || !bossGroup.currencies.empty()) {
							DrawGenericTab(bossGroup, bossProvider, currentProvider, isKpme);
						}
					} catch (...) {
						// Skip invalid tabs silently
						continue;
					}
				}
			}
		} else {
			// Render default tabs
			for (const auto& group : bossProvider->GetBossGroups()) {
				DrawGenericTab(group, bossProvider, currentProvider, isKpme);
			}
		}

		ImGui::EndTabBar();
	}
	ImGui::End();
	if (!Settings::ShowWindowLogProofs) SaveWindowState();
}