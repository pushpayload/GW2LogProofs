#ifndef WINGMAN_TYPES_H
#define WINGMAN_TYPES_H

#include <map>
#include <string>
#include <vector>

namespace Wingman {
	struct WingmanResponse {
		std::string account;
		std::map<std::string, int> kp;
	};

	struct WingmanPerformanceEntry {
		std::string comments;
		std::string log;
		std::string rank;
	};

	struct WingmanRankResponse {
		bool success = false;
		bool enoughData = false;
		int bossesCompleted = 0;
		double commanderRatio = 0.0;
		double globalRankNumeric = 0.0;
		std::string account;
		std::string note;
		std::string era;
		std::string globalRank;
		std::string timestamp;
		std::map<std::string, bool> completion;
		std::map<std::string, int> mainProfessions;
		std::map<std::string, std::string> ranksByCategory;
		std::map<std::string, std::vector<std::string>> top10BossKeysPerCategory;
		std::map<std::string, std::vector<WingmanPerformanceEntry>> byPerformanceType;
		std::map<std::string, std::map<std::string, std::string>> bestPerBoss;
	};

	struct WingmanPlayerData {
		WingmanResponse kp;
		WingmanRankResponse rank;
	};
} // namespace Wingman

#endif