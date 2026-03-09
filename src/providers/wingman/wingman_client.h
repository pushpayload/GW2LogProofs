#ifndef WINGMAN_CLIENT_H
#define WINGMAN_CLIENT_H

#include "wingman_types.h"
#include <functional>

namespace Wingman {
	class WingmanClient {
	public:

		void GetKpAsync(const std::string& account, std::function<void(const WingmanResponse&)> callback);
		void GetPlayerRanksAsync(const std::string& account, std::function<void(const WingmanRankResponse&)> callback);
		void RatePerformanceForCurrentPatchAsync(const std::string& account, std::function<void(const WingmanRankResponse&)> callback);
	};
} // namespace Wingman

#endif