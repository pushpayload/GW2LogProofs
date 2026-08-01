#include "httpclient.h"
#include "../core/shared.h"

#pragma comment(lib, "winhttp.lib")
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <unordered_set>
#include <vector>
#include <winhttp.h>

namespace {
	HINTERNET g_session = nullptr;
	std::mutex g_sessionMutex;
	std::condition_variable g_pendingCv;
	std::unordered_set<void*> g_pendingContexts;
	std::atomic<bool> g_shuttingDown {false};

	struct AsyncRequestContext {
		std::function<void(const std::string&)> callback;
		std::string response;
		HINTERNET hConnect = nullptr;
		HINTERNET hRequest = nullptr;
		std::unique_ptr<char[]> buffer;
		std::atomic<bool> callbackInvoked {false};
		std::atomic<int> openHandles {0};
	};

	void UnregisterContext(AsyncRequestContext* context) {
		std::lock_guard<std::mutex> lock(g_sessionMutex);
		g_pendingContexts.erase(context);
		if (g_pendingContexts.empty()) {
			g_pendingCv.notify_all();
		}
	}

	void RegisterContext(AsyncRequestContext* context) {
		std::lock_guard<std::mutex> lock(g_sessionMutex);
		g_pendingContexts.insert(context);
	}

	void InvokeCallbackOnce(AsyncRequestContext* context, const std::string& response) {
		if (context->callbackInvoked.exchange(true)) {
			return;
		}

		if (!g_shuttingDown.load(std::memory_order_acquire) && context->callback) {
			context->callback(response);
		}
		context->callback = nullptr;
	}

	void CloseRequestHandles(AsyncRequestContext* context) {
		if (context->hRequest) {
			HINTERNET request = context->hRequest;
			context->hRequest = nullptr;
			WinHttpCloseHandle(request);
		}
		if (context->hConnect) {
			HINTERNET connect = context->hConnect;
			context->hConnect = nullptr;
			WinHttpCloseHandle(connect);
		}
	}

	void ReleaseContextOnHandleClosing(AsyncRequestContext* context) {
		if (context->openHandles.fetch_sub(1) > 1) {
			return;
		}

		UnregisterContext(context);
		delete context;
	}

	void CALLBACK AsyncCallback(
		HINTERNET /*hInternet*/,
		DWORD_PTR dwContext,
		DWORD dwInternetStatus,
		LPVOID lpvStatusInformation,
		DWORD dwStatusInformationLength
	) {
		auto* context = reinterpret_cast<AsyncRequestContext*>(dwContext);
		if (!context) {
			return;
		}

		switch (dwInternetStatus) {
			case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
				WinHttpReceiveResponse(context->hRequest, nullptr);
				break;

			case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
				WinHttpQueryDataAvailable(context->hRequest, nullptr);
				break;

			case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
				DWORD bytesAvailable = *reinterpret_cast<DWORD*>(lpvStatusInformation);
				if (bytesAvailable > 0) {
					context->buffer = std::make_unique<char[]>(bytesAvailable);
					WinHttpReadData(context->hRequest, context->buffer.get(), bytesAvailable, nullptr);
				} else {
					InvokeCallbackOnce(context, context->response);
					CloseRequestHandles(context);
				}
				break;
			}

			case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
				DWORD bytesRead = dwStatusInformationLength;
				if (bytesRead > 0) {
					context->response.append(context->buffer.get(), bytesRead);
					WinHttpQueryDataAvailable(context->hRequest, nullptr);
				} else {
					InvokeCallbackOnce(context, context->response);
					CloseRequestHandles(context);
				}
				break;
			}

			case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
				InvokeCallbackOnce(context, "");
				CloseRequestHandles(context);
				break;

			case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
				ReleaseContextOnHandleClosing(context);
				break;

			default:
				break;
		}
	}
} // namespace

namespace HTTPClient {
	void Initialize() {
		std::lock_guard<std::mutex> lock(g_sessionMutex);
		g_shuttingDown.store(false, std::memory_order_release);
		if (!g_session) {
			g_session = WinHttpOpen(
				L"GW2LogProofs/1.0",
				WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS,
				WINHTTP_FLAG_ASYNC
			);
			if (g_session) {
				WinHttpSetStatusCallback(
					g_session,
					AsyncCallback,
					WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
					0
				);
			}
		}
	}

	std::future<std::string> GetRequestAsync(const std::wstring& wUrl) {
		auto promise = std::make_shared<std::promise<std::string>>();
		auto future = promise->get_future();
		GetRequestAsync(wUrl, [promise](const std::string& result) {
			promise->set_value(result);
		});
		return future;
	}

	void GetRequestAsync(const std::wstring& wUrl, std::function<void(const std::string&)> callback) {
		if (g_shuttingDown.load(std::memory_order_acquire)) {
			callback("");
			return;
		}

		Initialize();
		if (!g_session) {
			callback("");
			return;
		}

		URL_COMPONENTS urlComp = {};
		urlComp.dwStructSize = sizeof(urlComp);
		urlComp.dwSchemeLength = static_cast<DWORD>(-1);
		urlComp.dwHostNameLength = static_cast<DWORD>(-1);
		urlComp.dwUrlPathLength = static_cast<DWORD>(-1);

		if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &urlComp)) {
			callback("");
			return;
		}

		std::wstring hostName(urlComp.lpszHostName, urlComp.dwHostNameLength);
		std::wstring urlPath(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);

		auto* context = new AsyncRequestContext();
		context->callback = std::move(callback);
		RegisterContext(context);

		context->hConnect = WinHttpConnect(g_session, hostName.c_str(), urlComp.nPort, 0);
		if (!context->hConnect) {
			InvokeCallbackOnce(context, "");
			UnregisterContext(context);
			delete context;
			return;
		}
		context->openHandles.fetch_add(1);

		DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
		context->hRequest = WinHttpOpenRequest(
			context->hConnect,
			L"GET",
			urlPath.c_str(),
			nullptr,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			flags
		);
		if (!context->hRequest) {
			InvokeCallbackOnce(context, "");
			CloseRequestHandles(context);
			return;
		}
		context->openHandles.fetch_add(1);

		if (!WinHttpSendRequest(
				context->hRequest,
				WINHTTP_NO_ADDITIONAL_HEADERS,
				0,
				WINHTTP_NO_REQUEST_DATA,
				0,
				0,
				reinterpret_cast<DWORD_PTR>(context)
			)) {
			InvokeCallbackOnce(context, "");
			CloseRequestHandles(context);
		}
	}

	void Shutdown() {
		g_shuttingDown.store(true, std::memory_order_release);

		std::vector<AsyncRequestContext*> pending;
		{
			std::lock_guard<std::mutex> lock(g_sessionMutex);
			pending.reserve(g_pendingContexts.size());
			for (void* ptr : g_pendingContexts) {
				pending.push_back(static_cast<AsyncRequestContext*>(ptr));
			}
		}

		for (AsyncRequestContext* context : pending) {
			InvokeCallbackOnce(context, "");
			CloseRequestHandles(context);
		}

		std::unique_lock<std::mutex> lock(g_sessionMutex);
		g_pendingCv.wait_for(lock, std::chrono::seconds(5), [] {
			return g_pendingContexts.empty();
		});

		if (g_session) {
			WinHttpSetStatusCallback(g_session, nullptr, 0, 0);
			WinHttpCloseHandle(g_session);
			g_session = nullptr;
		}

		// Prefer leaking over freeing contexts WinHTTP may still touch.
		g_pendingContexts.clear();
	}
} // namespace HTTPClient
