#include "wingman_provider.h"
#include <atomic>

void WingmanProvider::LoadPlayerDataAsync(const std::string& account, std::function<void(const PlayerProofData&)> callback) {
	struct PendingWingmanLoad {
		Wingman::WingmanResponse kp;
		Wingman::WingmanRankResponse rank;
		std::atomic<int> completedRequests = 0;
	};

	auto pending = std::make_shared<PendingWingmanLoad>();
	auto finalizeLoad = [pending, callback, account]() {
		if (pending->completedRequests.fetch_add(1) + 1 < 2) {
			return;
		}

		Wingman::WingmanPlayerData response;
		response.kp = pending->kp;
		response.rank = pending->rank;
		callback(ConvertWingmanResponse(response, account));
	};

	client_.GetKpAsync(account, [pending, finalizeLoad](const Wingman::WingmanResponse& response) {
		pending->kp = response;
		finalizeLoad();
	});

	client_.GetPlayerRanksAsync(account, [pending, finalizeLoad](const Wingman::WingmanRankResponse& response) {
		pending->rank = response;
		finalizeLoad();
	});
}

std::vector<std::string> WingmanProvider::GetSupportedProofTypes() const {
	return {"KILL_PROOF"};
}

PlayerProofData WingmanProvider::ConvertWingmanResponse(const Wingman::WingmanPlayerData& response, const std::string& requestedAccount) {
	PlayerProofData data;
	data.accountName = !response.kp.account.empty() ? response.kp.account : (!response.rank.account.empty() ? response.rank.account : requestedAccount);
	data.profileId = data.accountName;
	data.profileUrl = "https://gw2wingman.nevermindcreations.de/kp/" + data.accountName;
	data.hasLinkedAccounts = false;
	data.rawData = response;

	for (const auto& kp : response.kp.kp) {
		ProofData proof;
		proof.identifier = kp.first;
		proof.amount = kp.second;
		proof.type = ProofType::KILL_PROOF;
		proof.displayName = kp.first;
		proof.url = "";
		data.proofs[kp.first] = proof;
	}

	return data;
}