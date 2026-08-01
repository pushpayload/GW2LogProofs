#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
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
#include "../resource.h"


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
	static std::unordered_set<size_t> wingmanExpandedPlayers;
	static std::mutex wingmanRerankMutex;
	static const std::array<const char*, 5> wingmanRankCategories = {"Damage", "Mechanics", "Speed", "Support", "Teamplay"};

	static void TriggerWingmanRerank(const std::string& account);
	static bool ShouldShowWingmanRankColumn(const std::string& providerName);

	static size_t GetWingmanRowId(const BossGroup& group, const std::string& account) {
		size_t seed = std::hash<std::string> {}(group.tableName);
		seed ^= std::hash<std::string> {}(account) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		return seed;
	}

	static float GetWingmanRankColumnWidth() {
		return (std::max)(Settings::ColumnSizeBosses, 64.0f);
	}

	static float GetWingmanTooltipWrapWidth() {
		return 320.0f;
	}

	// Colors sampled from Wingman rev profile rank emblems (/static/rev/ranks/emblem_*.png).
	static ImVec4 GetWingmanRankLetterColor(const std::string& rank) {
		unsigned rgb = 0;
		if (rank == "SSS" || rank == "SS" || rank == "S") {
			rgb = 0x9800F8;
		} else if (rank == "A+" || rank == "A") {
			rgb = 0xF89000;
		} else if (rank == "B+" || rank == "B") {
			rgb = 0x88C040;
		} else if (rank == "C+" || rank == "C") {
			rgb = 0x5878A8;
		} else if (rank == "D" || rank == "E") {
			rgb = 0xA8A0A0;
		} else if (rank == "F") {
			rgb = 0xF84000;
		} else {
			return ImGui::GetStyleColorVec4(ImGuiCol_Text);
		}

		return ImVec4(
			((rgb >> 16) & 0xFF) / 255.0f,
			((rgb >> 8) & 0xFF) / 255.0f,
			(rgb & 0xFF) / 255.0f,
			1.0f
		);
	}

	static void DrawWingmanRankLetter(const std::string& rank) {
		ImGui::TextColored(GetWingmanRankLetterColor(rank), "%s", rank.c_str());
	}

	static void DrawWingmanRankPair(const std::string& globalRank, const std::string& tabRank) {
		ImGui::BeginGroup();
		DrawWingmanRankLetter(globalRank);
		if (!tabRank.empty()) {
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::TextUnformatted("/");
			ImGui::SameLine(0.0f, 0.0f);
			DrawWingmanRankLetter(tabRank);
		}
		ImGui::EndGroup();
	}

	static void DrawWingmanLabeledRank(const char* label, const std::string& rank) {
		ImGui::Text("%s: ", label);
		ImGui::SameLine(0.0f, 0.0f);
		DrawWingmanRankLetter(rank);
	}

	// Same numeric scale Wingman uses for globalRankNumeric (lower is better).
	static std::optional<int> GetWingmanRankNumeric(const std::string& rank) {
		if (rank == "SSS") return 0;
		if (rank == "SS") return 1;
		if (rank == "S") return 2;
		if (rank == "A+") return 3;
		if (rank == "A") return 4;
		if (rank == "B+") return 5;
		if (rank == "B") return 6;
		if (rank == "C+") return 7;
		if (rank == "C") return 8;
		if (rank == "D") return 9;
		if (rank == "E") return 10;
		if (rank == "F") return 11;
		return std::nullopt;
	}

	static std::string GetWingmanRankLetterFromNumeric(double value) {
		static const std::pair<const char*, int> kScale[] = {
			{"SSS", 0}, {"SS", 1}, {"S", 2}, {"A+", 3}, {"A", 4}, {"B+", 5},
			{"B", 6}, {"C+", 7}, {"C", 8}, {"D", 9}, {"E", 10}, {"F", 11}
		};

		const char* best = "F";
		double bestDistance = 1e9;
		for (const auto& [letter, numeric] : kScale) {
			const double distance = std::abs(value - static_cast<double>(numeric));
			if (distance < bestDistance) {
				bestDistance = distance;
				best = letter;
			}
		}
		return best;
	}

	static bool IsWingmanOptionalCategory(const char* category) {
		return std::strcmp(category, "Mechanics") == 0 || std::strcmp(category, "Teamplay") == 0;
	}

	static std::optional<double> CalculateWingmanBossOverallNumericExcluding(
		const std::map<std::string, std::string>& categoryRanks,
		bool excludeOptionalCategories
	) {
		double weightedSum = 0.0;
		double totalWeight = 0.0;
		for (const char* category : wingmanRankCategories) {
			if (excludeOptionalCategories && IsWingmanOptionalCategory(category)) {
				continue;
			}

			auto it = categoryRanks.find(category);
			if (it == categoryRanks.end() || it->second.empty()) {
				continue;
			}

			auto numeric = GetWingmanRankNumeric(it->second);
			if (!numeric) {
				continue;
			}

			weightedSum += static_cast<double>(*numeric);
			totalWeight += 1.0;
		}

		if (totalWeight <= 0.0) {
			return std::nullopt;
		}

		return weightedSum / totalWeight;
	}

	static std::optional<double> CalculateWingmanBossOverallNumeric(const std::map<std::string, std::string>& categoryRanks) {
		const auto fullAverage = CalculateWingmanBossOverallNumericExcluding(categoryRanks, false);
		if (!Settings::WeighWingmanMechanicsTeamplayLess) {
			return fullAverage;
		}

		const auto withoutOptional = CalculateWingmanBossOverallNumericExcluding(categoryRanks, true);
		if (!fullAverage) {
			return withoutOptional;
		}
		if (!withoutOptional) {
			return fullAverage;
		}

		// Lower numeric is better — drop Mechanics/Teamplay only when they worsen the overall.
		return (std::min)(*fullAverage, *withoutOptional);
	}

	static std::string CalculateWingmanBossOverallRank(const std::map<std::string, std::string>& categoryRanks) {
		auto numeric = CalculateWingmanBossOverallNumeric(categoryRanks);
		if (!numeric) {
			return {};
		}
		return GetWingmanRankLetterFromNumeric(*numeric);
	}

	// Tab rank: average of up to 10 best per-boss overall ranks in the current boss group.
	static std::string CalculateWingmanTabRank(const Wingman::WingmanRankResponse& rankData, const BossGroup& group, int* outUsedCount = nullptr, int* outAvailableCount = nullptr) {
		std::vector<double> bossNumerics;
		bossNumerics.reserve(group.cachedBossIds.size());

		for (const auto& bossId : group.cachedBossIds) {
			auto bossIt = rankData.bestPerBoss.find(bossId);
			if (bossIt == rankData.bestPerBoss.end()) {
				continue;
			}

			auto numeric = CalculateWingmanBossOverallNumeric(bossIt->second);
			if (!numeric) {
				continue;
			}
			bossNumerics.push_back(*numeric);
		}

		if (outAvailableCount) {
			*outAvailableCount = static_cast<int>(bossNumerics.size());
		}

		if (bossNumerics.empty()) {
			if (outUsedCount) {
				*outUsedCount = 0;
			}
			return {};
		}

		std::sort(bossNumerics.begin(), bossNumerics.end());
		const size_t usedCount = (std::min)(bossNumerics.size(), size_t {10});
		double sum = 0.0;
		for (size_t i = 0; i < usedCount; ++i) {
			sum += bossNumerics[i];
		}

		if (outUsedCount) {
			*outUsedCount = static_cast<int>(usedCount);
		}

		return GetWingmanRankLetterFromNumeric(sum / static_cast<double>(usedCount));
	}

	// Highest rank = best letter (lowest numeric on Wingman's scale).
	static std::string GetWingmanBossHighestRank(const std::map<std::string, std::string>& categoryRanks) {
		std::optional<int> bestNumeric;
		std::string bestRank;
		for (const char* category : wingmanRankCategories) {
			auto it = categoryRanks.find(category);
			if (it == categoryRanks.end() || it->second.empty()) {
				continue;
			}

			auto numeric = GetWingmanRankNumeric(it->second);
			if (!numeric) {
				continue;
			}

			if (!bestNumeric || *numeric < *bestNumeric) {
				bestNumeric = *numeric;
				bestRank = it->second;
			}
		}
		return bestRank;
	}

	static std::string GetWingmanBossHighestRankForId(const Wingman::WingmanRankResponse* rankData, const std::string& bossId) {
		if (!rankData) {
			return {};
		}
		auto bossIt = rankData->bestPerBoss.find(bossId);
		if (bossIt == rankData->bestPerBoss.end()) {
			return {};
		}
		return GetWingmanBossHighestRank(bossIt->second);
	}

	static void DrawWingmanBossKpAmount(const std::string& amountText, const Wingman::WingmanRankResponse* rankData, const std::string& bossId, bool colorByRank) {
		if (colorByRank) {
			const std::string highestRank = GetWingmanBossHighestRankForId(rankData, bossId);
			if (!highestRank.empty()) {
				ImGui::TextColored(GetWingmanRankLetterColor(highestRank), "%s", amountText.c_str());
				return;
			}
		}
		ImGui::Text("%s", amountText.c_str());
	}

	static float GetAccountColumnWidth(const std::string& providerName) {
		float width = Settings::ColumnSizeAccount;
		if (ShouldShowWingmanRankColumn(providerName)) {
			width += 70.0f;
		}
		return width;
	}

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

	static bool ShouldShowWingmanRankNote(const Wingman::WingmanRankResponse& rankData) {
		// ratePerformance can return a stale "not enough data" note alongside a successful ranked payload.
		if (rankData.note.empty()) {
			return false;
		}
		if (rankData.success && rankData.enoughData && !rankData.globalRank.empty()) {
			return false;
		}
		return true;
	}

	static bool HasWingmanBreakdownForGroup(const Wingman::WingmanRankResponse* rankData, const BossGroup& group) {
		if (!rankData) {
			return false;
		}

		for (const auto& bossId : group.cachedBossIds) {
			if (rankData->bestPerBoss.find(bossId) != rankData->bestPerBoss.end()) {
				return true;
			}
		}

		return false;
	}

	static bool HasWingmanCategoryForGroup(const Wingman::WingmanRankResponse* rankData, const BossGroup& group, const char* category) {
		if (!rankData) {
			return false;
		}

		for (const auto& bossId : group.cachedBossIds) {
			auto bossIt = rankData->bestPerBoss.find(bossId);
			if (bossIt != rankData->bestPerBoss.end()) {
				auto categoryIt = bossIt->second.find(category);
				if (categoryIt != bossIt->second.end() && !categoryIt->second.empty()) {
					return true;
				}
			}
		}

		return false;
	}

	static std::vector<std::string> GetWingmanHiddenBossKeysForGroup(const Wingman::WingmanRankResponse* rankData, const BossGroup& group) {
		std::vector<std::string> hiddenBossKeys;
		if (!rankData) {
			return hiddenBossKeys;
		}

		std::unordered_set<std::string> visibleBossIds(group.cachedBossIds.begin(), group.cachedBossIds.end());
		for (const auto& bossEntry : rankData->bestPerBoss) {
			if (visibleBossIds.find(bossEntry.first) == visibleBossIds.end()) {
				hiddenBossKeys.push_back(bossEntry.first);
			}
		}

		return hiddenBossKeys;
	}

	static void DrawWingmanBossKpTooltip(const Wingman::WingmanRankResponse* rankData, const std::string& bossId) {
		if (!rankData) {
			return;
		}

		auto bossIt = rankData->bestPerBoss.find(bossId);
		if (bossIt == rankData->bestPerBoss.end()) {
			return;
		}

		bool hasCategoryRank = false;
		for (const char* category : wingmanRankCategories) {
			auto categoryIt = bossIt->second.find(category);
			if (categoryIt != bossIt->second.end() && !categoryIt->second.empty()) {
				hasCategoryRank = true;
				break;
			}
		}
		if (!hasCategoryRank || !ImGui::IsItemHovered()) {
			return;
		}

		ImGui::BeginTooltip();
		std::string overallRank = CalculateWingmanBossOverallRank(bossIt->second);
		if (!overallRank.empty()) {
			DrawWingmanLabeledRank("Overall", overallRank);
			ImGui::Separator();
		}
		for (const char* category : wingmanRankCategories) {
			auto categoryIt = bossIt->second.find(category);
			if (categoryIt != bossIt->second.end() && !categoryIt->second.empty()) {
				DrawWingmanLabeledRank(category, categoryIt->second);
			}
		}
		ImGui::EndTooltip();
	}

	static void DrawWingmanRankTooltip(const Wingman::WingmanRankResponse& rankData, const BossGroup& group, const std::string& tabRank = {}, int tabUsedCount = 0, int tabAvailableCount = 0) {
		ImGui::BeginTooltip();
		if (!rankData.era.empty()) {
			ImGui::Text("Era: %s", rankData.era.c_str());
		}
		if (!rankData.globalRank.empty()) {
			DrawWingmanLabeledRank("Global Rank", rankData.globalRank);
		}
		if (!tabRank.empty()) {
			const std::string tabLabel = group.name.empty() ? "Tab Rank" : (group.name + " Rank");
			DrawWingmanLabeledRank(tabLabel.c_str(), tabRank);
			ImGui::TextDisabled("Top %d of %d bosses in this tab", tabUsedCount, tabAvailableCount);
		}
		if (rankData.bossesCompleted > 0) {
			ImGui::Text("Bosses Completed: %d", rankData.bossesCompleted);
		}
		if (rankData.commanderRatio > 0.0) {
			ImGui::Text("Commander Ratio: %.1f%%", rankData.commanderRatio * 100.0);
		}
		for (const char* category : wingmanRankCategories) {
			auto it = rankData.ranksByCategory.find(category);
			if (it != rankData.ranksByCategory.end()) {
				DrawWingmanLabeledRank(category, it->second);
			}
		}
		if (!rankData.mainProfessions.empty()) {
			ImGui::Separator();
			ImGui::Text("Main Professions:");
			for (const auto& profession : rankData.mainProfessions) {
				ImGui::Text("%s: %d", profession.first.c_str(), profession.second);
			}
		}
		if (!rankData.completion.empty()) {
			ImGui::Separator();
			ImGui::Text("Completion:");
			for (const auto& entry : rankData.completion) {
				if (entry.second) {
					ImGui::TextUnformatted(entry.first.c_str());
				}
			}
		}
		if (ShouldShowWingmanRankNote(rankData)) {
			ImGui::Separator();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + GetWingmanTooltipWrapWidth());
			ImGui::TextWrapped("%s", rankData.note.c_str());
			ImGui::PopTextWrapPos();
		}
#ifdef _DEBUG
		std::vector<std::string> hiddenBossKeys = GetWingmanHiddenBossKeysForGroup(&rankData, group);
		if (!hiddenBossKeys.empty()) {
			ImGui::Separator();
			ImGui::Text("Hidden bestPerBoss keys:");
			for (const auto& bossKey : hiddenBossKeys) {
				ImGui::TextUnformatted(bossKey.c_str());
			}
		}
#endif
		ImGui::EndTooltip();
	}

	static void DrawWingmanRerankButton(const std::string& account, const BossGroup& group, const Wingman::WingmanRankResponse* rankData) {
		if (!rankData) {
			return;
		}
#ifndef _DEBUG
		if (rankData->enoughData) {
			return;
		}
#endif

		WingmanRerankState rerankState;
		{
			std::scoped_lock lock(wingmanRerankMutex);
			auto it = wingmanRerankStates.find(account);
			if (it != wingmanRerankStates.end()) {
				rerankState = it->second;
			}
		}

		const auto now = std::chrono::steady_clock::now();
		const bool isCoolingDown = !rerankState.pending && rerankState.lastRequestTime != std::chrono::steady_clock::time_point {} && (now - rerankState.lastRequestTime) < WINGMAN_RERANK_COOLDOWN;

		ImGui::SameLine();
		ImGui::PushID(group.tableName.c_str());
		ImGui::PushID(account.c_str());
		ImGui::PushID("rerank");
		const bool disableRerankButton = rerankState.pending || isCoolingDown;
		if (disableRerankButton) {
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		}
		if (ImGui::SmallButton("Rerank")) {
			TriggerWingmanRerank(account);
		}
		if (disableRerankButton) {
			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::BeginTooltip();
			if (rerankState.pending) {
				ImGui::Text("Rerank in progress.");
			} else if (isCoolingDown) {
				auto remaining = std::chrono::duration_cast<std::chrono::minutes>(WINGMAN_RERANK_COOLDOWN - (now - rerankState.lastRequestTime)).count();
				ImGui::Text("Cooldown active: %lld minute(s) remaining.", remaining + 1);
			} else if (!rerankState.lastNote.empty()) {
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + GetWingmanTooltipWrapWidth());
				ImGui::TextWrapped("%s", rerankState.lastNote.c_str());
				ImGui::PopTextWrapPos();
			} else {
				ImGui::Text("Ask Wingman to calculate ranks for this player.");
			}
			if (ShouldShowWingmanRankNote(*rankData)) {
				ImGui::Separator();
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + GetWingmanTooltipWrapWidth());
				ImGui::TextWrapped("%s", rankData->note.c_str());
				ImGui::PopTextWrapPos();
			}
			ImGui::EndTooltip();
		}
		ImGui::PopID();
		ImGui::PopID();
		ImGui::PopID();
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
				if (ShouldShowWingmanRankNote(response)) {
					state.lastNote = response.note;
				} else if (response.enoughData && !response.globalRank.empty()) {
					state.lastNote = std::format("Rank calculated: {}", response.globalRank);
				} else {
					state.lastNote.clear();
				}
			}

			const bool shouldRefreshPlayerRanks = response.enoughData || !response.globalRank.empty() || !response.bestPerBoss.empty();
			if (shouldRefreshPlayerRanks) {
				PlayerManager::lazyLoadManager.ClearPlayerData(account, WINGMAN_PROVIDER_NAME);
				PlayerManager::lazyLoadManager.RequestPlayerData(account, WINGMAN_PROVIDER_NAME);
			}
		});
	}

	void ClearWingmanUiState() {
		std::scoped_lock lock(wingmanRerankMutex);
		wingmanRerankStates.clear();
		wingmanExpandedPlayers.clear();
	}
} // namespace

void ResetWingmanUiState() {
	ClearWingmanUiState();
}

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

static void DrawPlayerAccountName(const std::string& account, const PlayerProofData* proofData) {
	if (proofData && !proofData->profileUrl.empty()) {
		if (ImGui::TextURL(account.c_str())) {
			ShellExecuteA(0, 0, proofData->profileUrl.c_str(), 0, 0, SW_SHOW);
		}
	} else {
		ImGui::Text(account.c_str());
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

static void DrawKpmeId(const PlayerProofData* proofData) {
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

	columnSpecs.emplace_back("Account", GetAccountColumnWidth(providerName));
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
		columnSpecs.emplace_back("Rank", GetWingmanRankColumnWidth());
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
		Texture* texture = APIDefs->Textures.GetOrCreateFromResource("TEX_WINGMAN", IDB_WINGMAN, hSelf);
		if (texture) {
			float columnWidth = GetWingmanRankColumnWidth();
			float iconSize = Settings::BossIconScale;
			float padding = (columnWidth - iconSize) * 0.5f;
			if (padding > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padding);
			ImGui::Image((void*) texture->Resource, ImVec2(iconSize, iconSize));
		} else {
			ImGui::Text("Rank");
		}
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::Text("Rank");
			ImGui::EndTooltip();
		}
	}
}

static void DrawWingmanRankCell(const std::string& account, const BossGroup& group, const PlayerProofData* proofData, LoadState lazyState) {
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
		int tabUsedCount = 0;
		int tabAvailableCount = 0;
		const std::string tabRank = CalculateWingmanTabRank(*rankData, group, &tabUsedCount, &tabAvailableCount);
		DrawWingmanRankPair(rankData->globalRank, tabRank);
		if (ImGui::IsItemHovered()) {
			DrawWingmanRankTooltip(*rankData, group, tabRank, tabUsedCount, tabAvailableCount);
		}
		return;
	}

	if (!rankData->success || !rankData->enoughData || ShouldShowWingmanRankNote(*rankData)) {
		if (!rankData->success) {
			ImGui::TextDisabled("Unranked");
		} else if (!rankData->enoughData) {
			ImGui::TextDisabled("Need");
		} else {
			ImGui::TextDisabled("None");
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			DrawWingmanRankTooltip(*rankData, group);
		}
		return;
	}

	if (lazyState == LoadState::LOADING) {
		DrawSpinner();
	} else {
		ImGui::Text("-");
	}
}

static void DrawWingmanBreakdownRows(const BossGroup& group, bool showKpmeId, const PlayerProofData* proofData) {
	const auto* rankData = GetWingmanRankResponse(proofData);
	if (!rankData || !HasWingmanBreakdownForGroup(rankData, group)) {
		return;
	}

	auto drawBreakdownRow = [&](const char* label, bool isOverall) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Indent(20.0f);
		ImGui::TextDisabled("%s", label);
		ImGui::Unindent(20.0f);

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
			auto bossIt = rankData->bestPerBoss.find(bossId);
			if (bossIt == rankData->bestPerBoss.end()) {
				ImGui::Text("");
				continue;
			}

			if (isOverall) {
				std::string overallRank = CalculateWingmanBossOverallRank(bossIt->second);
				if (overallRank.empty()) {
					ImGui::Text("");
				} else {
					DrawWingmanRankLetter(overallRank);
				}
				continue;
			}

			auto categoryIt = bossIt->second.find(label);
			if (categoryIt == bossIt->second.end() || categoryIt->second.empty()) {
				ImGui::Text("");
				continue;
			}

			DrawWingmanRankLetter(categoryIt->second);
		}

		if (ShouldShowWingmanRankColumn(WINGMAN_PROVIDER_NAME)) {
			ImGui::TableNextColumn();
			ImGui::Text("");
		}

		HighlightRowOnHover(ImGui::GetCurrentContext()->CurrentTable);
	};

	drawBreakdownRow("Overall", true);

	for (const char* category : wingmanRankCategories) {
		if (!HasWingmanCategoryForGroup(rankData, group, category)) {
			continue;
		}
		drawBreakdownRow(category, false);
	}
}

static void DrawPlayerRow(const std::string& account, const BossGroup& group, IBossProvider* provider, bool showKpmeId, const std::string& providerName) {

	group.InitializeCache(provider);


	auto lazyState = PlayerManager::lazyLoadManager.GetPlayerState(account, providerName);
	auto lazyData = PlayerManager::lazyLoadManager.GetPlayerData(account, providerName);
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
	const size_t expansionKey = GetWingmanRowId(group, account);
	bool isWingmanExpanded = false;
	const bool hasAnyDisplayData = proofData && (!proofData->proofs.empty() || (ShouldShowWingmanRankColumn(providerName) && HasWingmanRankDisplayData(proofData)));

	if (canExpandWingmanBreakdown) {
		isWingmanExpanded = wingmanExpandedPlayers.find(expansionKey) != wingmanExpandedPlayers.end();
	}

	if (!hasAnyDisplayData) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
	}

	ImGui::TableNextColumn();
	if (canExpandWingmanBreakdown) {
		ImGui::PushID(group.tableName.c_str());
		ImGui::PushID(account.c_str());
		ImGui::PushID("expand");
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
		ImGui::PopID();
		ImGui::PopID();
		ImGui::SameLine();
	}
	DrawPlayerAccountName(account, proofData);
	if (ShouldShowWingmanRankColumn(providerName)) {
		DrawWingmanRerankButton(account, group, wingmanRankData);
	}
	if (showKpmeId) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		DrawKpmeId(proofData);
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
		const bool colorKpByRank = ShouldShowWingmanRankColumn(providerName) && Settings::ColorWingmanKpByHighestRank;
		if (proofData) {
			auto it = proofData->proofs.find(group.cachedBossIds[i]);
			const std::string amountText = it != proofData->proofs.end() ? std::to_string(it->second.amount) : (isDisabled ? "" : "0");
			if (!amountText.empty()) {
				DrawWingmanBossKpAmount(amountText, wingmanRankData, group.cachedBossIds[i], colorKpByRank);
			} else {
				ImGui::Text("");
			}
			if (ShouldShowWingmanRankColumn(providerName)) {
				DrawWingmanBossKpTooltip(wingmanRankData, group.cachedBossIds[i]);
			}
		} else {
			if (lazyState == LoadState::LOADING) {
				DrawSpinner();
			} else {
				DrawWingmanBossKpAmount(isDisabled ? "" : "0", wingmanRankData, group.cachedBossIds[i], colorKpByRank);
			}
		}
	}
	if (ShouldShowWingmanRankColumn(providerName)) {
		ImGui::TableNextColumn();
		HighlightColumnOnHover();
		DrawWingmanRankCell(account, group, proofData, lazyState);
	}

	if (!hasAnyDisplayData) {
		ImGui::PopStyleColor();
	}

	HighlightRowOnHover(ImGui::GetCurrentContext()->CurrentTable);

	if (isWingmanExpanded) {
		DrawWingmanBreakdownRows(group, showKpmeId, proofData);
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

		static std::vector<std::string> visibleAccounts;
		visibleAccounts.clear();

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
				visibleAccounts.push_back(p.account);
			}
		}


		for (const auto& account : visibleAccounts) {
			DrawPlayerRow(account, group, provider, showKpmeId, providerName);
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