#ifndef KYTY_LIBS_CROSSCREASE_ONLINE_H_
#define KYTY_LIBS_CROSSCREASE_ONLINE_H_

// CrossCrease online support.
//
// Self-contained so it can be carried into another KytyPS5-based emulator (for example a Faceoff
// fork) by copying crosscreaseOnline.{h,cpp} and the small, marked hooks in network.cpp and
// libNet.cpp. Everything is off unless CROSSCREASE_ONLINE_CONFIG names a JSON file; see
// docs/online/README.md for the format.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Libs::CrossCreaseOnline {

enum class HostResult {
	NotHandled, // not configured: resolve normally
	Mapped,     // replaced by the configured IPv4 address
	Blocked,    // not listed and unlisted hosts are blocked: report "no such host"
};

bool Enabled();

// `addr` receives the IPv4 address in network byte order.
// Faceoff: also Blocked when CROSSCREASE_ONLINE_CONFIG is set but can't be used, and for EA/PSN
// names that no mapping covers (they never reach the system resolver).
HostResult ResolveHost(const char* hostname, uint32_t* addr);

// The per-instance IPv4 address (network byte order) that this player binds and advertises, or 0.
uint32_t LocalAddress();
// TCP port to connect to instead of `port` (host byte order) for an address (network byte order)
// that "hosts" maps to: "port_map": {"443": 10443} lets a CrossCrease server that shares its
// machine with a web server take the game's TLS port 443 on another port. Other addresses and
// unlisted ports are returned unchanged.
uint16_t RemapPort(uint32_t addr, uint16_t port);
std::string LocalAddressString();

// Identity presented to the game in place of a PlayStation Network account.
std::string OnlineId(const std::string& fallback);
uint64_t    AccountId(uint64_t fallback);

// Fills a sceNpAuth authorization code (up to `size` bytes, NUL-terminated). Returns false when no
// sign-in token is configured.
bool AuthorizationCode(const char* scope, char* out, size_t size, int* issuer_id);

// sceHttp requests. The PS5 system HTTP library performs TLS itself, so the emulator cannot pass
// the game's HTTPS through to a server as-is. Requests whose host is mapped by "hosts" are sent
// instead to the CrossCrease server's HTTP gateway ("http_gateway": "address:port"), with the
// original scheme, host and port in X-CrossCrease-* headers.
enum class HttpRoute {
	NotHandled, // no gateway configured or host not mapped: keep the emulator's default behaviour
	Gateway,
	Blocked, // host is not mapped and unlisted hosts are blocked
};

struct HttpExchange {
	// Request
	std::string                                      method;
	std::string                                      url; // absolute: scheme://host[:port]/path
	std::vector<std::pair<std::string, std::string>> headers;
	std::string                                      body;
	uint32_t                                         connect_timeout_ms = 10000;
	uint32_t                                         recv_timeout_ms    = 30000;
	// Response
	enum class Error { None, Connect, Io, BadResponse };
	Error       error  = Error::None;
	int         status = 0;
	std::string response_headers; // status line and header lines, CRLF-terminated, final CRLF
	std::string response_body;
};

HttpRoute HttpRouteFor(const std::string& url);
// True when an HTTP gateway is configured (PSN WebAPI requests always go there).
bool HttpGatewayConfigured();
// Blocking; call from a worker thread.
void HttpPerform(HttpExchange* exchange);

// Research trace (crosscrease-online.log): DNS names, connections and auth requests.
void Trace(const char* format, ...);
void TraceSockaddr(const char* what, int guest_socket, const void* host_sockaddr);
// Records the first calls of each HLE function (library::module::function) in the trace.
void TraceCall(const char* library, const char* module, const char* function);

} // namespace Libs::CrossCreaseOnline

// Adds the call trace to PRINT_NAME in the files that implement network and NP functions.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CROSSCREASE_PRINT_NAME()                                                                   \
	do {                                                                                           \
		::Libs::CrossCreaseOnline::TraceCall(g_library, g_module, __func__);                       \
		if (PRINT_NAME_ENABLED) {                                                                  \
			::Libs::PrintName(g_library, g_module, __func__);                                      \
		}                                                                                          \
	} while (false)

#endif /* KYTY_LIBS_CROSSCREASE_ONLINE_H_ */
