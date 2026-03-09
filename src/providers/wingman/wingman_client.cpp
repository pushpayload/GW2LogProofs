#include "wingman_client.h"
#include "../../core/shared.h"
#include "../../nlohmann/json.hpp"
#include "../../utils/httpclient.h"
#include <format>


using json = nlohmann::json;

namespace Wingman {
	namespace {
		template <typename T>
		void RequestWingmanJson(
				const std::string& url,
				const std::string& account,
				const char* requestName,
				std::function<void(const T&)> callback
		) {
			const char* cUrl = url.c_str();
			APIDefs->Log(ELogLevel_DEBUG, ADDON_NAME, std::format("Requesting Wingman {} (async): {}", requestName, cUrl).c_str());
			std::wstring wUrl(cUrl, cUrl + strlen(cUrl));

			HTTPClient::GetRequestAsync(wUrl, [account, callback, requestName](const std::string& response) {
				if (response.empty()) {
					APIDefs->Log(ELogLevel_WARNING, ADDON_NAME, std::format("Empty response from Wingman {} for account: {}", requestName, account).c_str());
					callback(T {});
					return;
				}
				try {
					json j = json::parse(response);
					callback(j.template get<T>());
				} catch (const json::parse_error& e) {
					APIDefs->Log(ELogLevel_WARNING, ADDON_NAME, std::format("Failed to parse Wingman {} response for {}: {}", requestName, account, e.what()).c_str());
					callback(T {});
				}
			});
		}

		void ParseStringMap(const json& source, std::map<std::string, std::string>& target) {
			if (!source.is_object()) {
				return;
			}

			for (const auto& item : source.items()) {
				if (item.value().is_string()) {
					target[item.key()] = item.value().get<std::string>();
				}
			}
		}
	} // namespace

	void from_json(const json& j, WingmanResponse& r) {
		try {
			if (j.contains("account")) {
				if (j.at("account").is_string()) {
					j.at("account").get_to(r.account);
				}
			}

			if (j.contains("kp")) {
				if (j.at("kp").is_object()) {
					for (const auto& item : j.at("kp").items()) {
						if (item.value().is_object()) {
							if (item.value().contains("total")) {
								if (item.value().at("total").is_number_integer()) {
									r.kp[item.key()] = item.value().at("total");
								}
							}
						}
					}
				}
			}
		} catch (const json::parse_error& ex) {
			APIDefs->Log(ELogLevel_WARNING, ADDON_NAME, std::format("Failed to parse Wingman JSON response: {}", ex.what()).c_str());
		}
	}

	void from_json(const json& j, WingmanRankResponse& r) {
		try {
			if (j.contains("account") && j.at("account").is_string()) {
				j.at("account").get_to(r.account);
			}
			if (j.contains("note") && j.at("note").is_string()) {
				j.at("note").get_to(r.note);
			}
			if (j.contains("era") && j.at("era").is_string()) {
				j.at("era").get_to(r.era);
			}
			if (j.contains("globalRank") && j.at("globalRank").is_string()) {
				j.at("globalRank").get_to(r.globalRank);
			}
			if (j.contains("timestamp") && j.at("timestamp").is_string()) {
				j.at("timestamp").get_to(r.timestamp);
			}
			if (j.contains("success") && j.at("success").is_boolean()) {
				j.at("success").get_to(r.success);
			}
			if (j.contains("enoughData") && j.at("enoughData").is_boolean()) {
				j.at("enoughData").get_to(r.enoughData);
			}
			if (j.contains("bossesCompleted") && j.at("bossesCompleted").is_number_integer()) {
				j.at("bossesCompleted").get_to(r.bossesCompleted);
			}
			if (j.contains("globalRankNumeric") && j.at("globalRankNumeric").is_number()) {
				j.at("globalRankNumeric").get_to(r.globalRankNumeric);
			}
			if (j.contains("ranksByCategory")) {
				ParseStringMap(j.at("ranksByCategory"), r.ranksByCategory);
			}
			if (j.contains("bestPerBoss") && j.at("bestPerBoss").is_object()) {
				for (const auto& bossEntry : j.at("bestPerBoss").items()) {
					std::map<std::string, std::string> categoryRanks;
					ParseStringMap(bossEntry.value(), categoryRanks);
					if (!categoryRanks.empty()) {
						r.bestPerBoss[bossEntry.key()] = std::move(categoryRanks);
					}
				}
			}
		} catch (const json::parse_error& ex) {
			APIDefs->Log(ELogLevel_WARNING, ADDON_NAME, std::format("Failed to parse Wingman rank JSON response: {}", ex.what()).c_str());
		}
	}

	void WingmanClient::GetKpAsync(const std::string& account, std::function<void(const WingmanResponse&)> callback) {
		std::string url = std::format("https://gw2wingman.nevermindcreations.de/api/kp?account={}", account);
		RequestWingmanJson<WingmanResponse>(url, account, "kp", callback);
	}

	void WingmanClient::GetPlayerRanksAsync(const std::string& account, std::function<void(const WingmanRankResponse&)> callback) {
		std::string url = std::format("https://gw2wingman.nevermindcreations.de/api/playerRanks/current/{}", account);
		RequestWingmanJson<WingmanRankResponse>(url, account, "playerRanks/current", callback);
	}

	void WingmanClient::RatePerformanceForCurrentPatchAsync(const std::string& account, std::function<void(const WingmanRankResponse&)> callback) {
		std::string url = std::format("https://gw2wingman.nevermindcreations.de/api/ratePerformanceOfPlayerForCurrentPatch/{}", account);
		RequestWingmanJson<WingmanRankResponse>(url, account, "ratePerformanceOfPlayerForCurrentPatch", callback);
	}
} // namespace Wingman