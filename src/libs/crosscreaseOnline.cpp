#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "libs/crosscreaseOnline.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::CrossCreaseOnline {

namespace {

struct Settings {
	bool                                             enabled = false;
	// Faceoff: CROSSCREASE_ONLINE_CONFIG is set but can't be read: every lookup is blocked.
	bool config_failed = false;
	std::vector<std::pair<std::string, uint32_t>>    exact_hosts;
	std::vector<std::pair<std::string, uint32_t>>    suffix_hosts; // "*.ea.com" stored as ".ea.com"
	bool                                             block_unlisted = false;
	uint32_t                                         local_address  = 0;
	std::string                                      local_address_text;
	std::string                                      online_id;
	uint64_t                                         account_id = 0;
	std::string                                      auth_code;
	int                                              issuer_id = 1;
	std::string                                      trace_file;
	uint32_t                                         http_gateway_addr = 0; // network byte order
	uint16_t                                         http_gateway_port = 0; // host byte order
	std::string                                      http_gateway_text;
	std::vector<std::pair<uint16_t, uint16_t>>       port_map; // host byte order: from -> to
	uint32_t                                         trace_call_limit = 16;
	std::vector<std::string>                         trace_call_exclude;
};

std::once_flag g_once;
Settings       g_settings;
std::mutex     g_trace_mutex;
FILE*          g_trace = nullptr;

std::string Lower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

bool ParseIpv4(const std::string& text, uint32_t* out) {
	in_addr addr {};
	if (inet_pton(AF_INET, text.c_str(), &addr) != 1) {
		return false;
	}
	std::memcpy(out, &addr, sizeof(*out));
	return true;
}

void LoadConfig() {
	const char* path = std::getenv("CROSSCREASE_ONLINE_CONFIG");
	if (path == nullptr || path[0] == '\0') {
		return;
	}
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		std::fprintf(stderr, "CrossCrease online: cannot open %s\n", path);
		g_settings.config_failed = true; // Faceoff: fail closed
		return;
	}
	nlohmann::json doc = nlohmann::json::parse(file, nullptr, false, true);
	if (doc.is_discarded() || !doc.is_object()) {
		std::fprintf(stderr, "CrossCrease online: %s is not valid JSON\n", path);
		g_settings.config_failed = true; // Faceoff: fail closed
		return;
	}

	Settings s;
	s.enabled = true;
	if (auto hosts = doc.find("hosts"); hosts != doc.end() && hosts->is_object()) {
		for (const auto& [name, value]: hosts->items()) {
			uint32_t addr = 0;
			if (name.empty() || name[0] == '_' || !value.is_string() ||
			    !ParseIpv4(value.get<std::string>(), &addr)) {
				continue;
			}
			const auto key = Lower(name);
			if (key.rfind("*.", 0) == 0) {
				s.suffix_hosts.emplace_back(key.substr(1), addr);
			} else {
				s.exact_hosts.emplace_back(key, addr);
			}
		}
	}
	// Longest suffix wins.
	std::sort(s.suffix_hosts.begin(), s.suffix_hosts.end(),
	          [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
	s.block_unlisted = doc.value("block_unlisted_hosts", false);
	if (auto local = doc.value("local_address", std::string {}); !local.empty()) {
		if (ParseIpv4(local, &s.local_address)) {
			s.local_address_text = local;
		}
	}
	s.online_id = doc.value("online_id", std::string {});
	if (s.online_id.size() > 16) {
		s.online_id.resize(16);
	}
	if (auto id = doc.find("account_id"); id != doc.end()) {
		if (id->is_number_unsigned()) {
			s.account_id = id->get<uint64_t>();
		} else if (id->is_string()) {
			s.account_id = std::strtoull(id->get<std::string>().c_str(), nullptr, 0);
		}
	}
	s.auth_code  = doc.value("auth_code", std::string {});
	s.issuer_id  = doc.value("issuer_id", 1);
	s.trace_file = doc.value("trace_file", std::string {"crosscrease-online.log"});
	s.trace_call_limit = doc.value("trace_call_limit", 16u);
	if (auto exclude = doc.find("trace_call_exclude"); exclude != doc.end() && exclude->is_array()) {
		for (const auto& name: *exclude) {
			if (name.is_string()) {
				s.trace_call_exclude.push_back(name.get<std::string>());
			}
		}
	}
	if (auto ports = doc.find("port_map"); ports != doc.end() && ports->is_object()) {
		for (const auto& [from, to]: ports->items()) {
			const auto source = std::strtoul(from.c_str(), nullptr, 10);
			const auto target = to.is_number_unsigned() ? to.get<uint64_t>() : 0;
			if (source > 0 && source < 65536 && target > 0 && target < 65536) {
				s.port_map.emplace_back(static_cast<uint16_t>(source), static_cast<uint16_t>(target));
			}
		}
	}
	if (auto gateway = doc.value("http_gateway", std::string {}); !gateway.empty()) {
		const auto colon = gateway.rfind(':');
		const auto port  = colon == std::string::npos ? 0UL : std::strtoul(gateway.c_str() + colon + 1, nullptr, 10);
		if (port > 0 && port < 65536 && ParseIpv4(gateway.substr(0, colon), &s.http_gateway_addr)) {
			s.http_gateway_port = static_cast<uint16_t>(port);
			s.http_gateway_text = gateway;
		} else {
			std::fprintf(stderr, "CrossCrease online: http_gateway must be ipv4:port, not %s\n", gateway.c_str());
		}
	}
	g_settings   = std::move(s);

	if (!g_settings.trace_file.empty()) {
		g_trace = std::fopen(g_settings.trace_file.c_str(), "a");
	}
	if (g_trace != nullptr) // Faceoff: else Trace() calls Get() and re-enters call_once (a hang)
		Trace("config %s: %zu exact hosts, %zu suffix hosts, block_unlisted=%d local=%s online_id=%s "
	      "http_gateway=%s",
	      path, g_settings.exact_hosts.size(), g_settings.suffix_hosts.size(),
	      g_settings.block_unlisted ? 1 : 0,
	      g_settings.local_address_text.empty() ? "-" : g_settings.local_address_text.c_str(),
	      g_settings.online_id.empty() ? "-" : g_settings.online_id.c_str(),
	      g_settings.http_gateway_text.empty() ? "-" : g_settings.http_gateway_text.c_str());
}

// Faceoff: a config value of the wrong type (nlohmann throws) fails closed instead of ending the process.
void Load() {
	try {
		LoadConfig();
	} catch (const std::exception& e) {
		std::fprintf(stderr, "CrossCrease online: config error: %s\n", e.what());
		g_settings               = Settings {};
		g_settings.config_failed = true;
	}
}

const Settings& Get() {
	std::call_once(g_once, Load);
	return g_settings;
}

} // namespace

bool Enabled() {
	return Get().enabled;
}

namespace {

HostResult MatchHost(const std::string& hostname, uint32_t* addr, std::string* how) {
	const auto& s    = Get();
	const auto  name = Lower(hostname);
	for (const auto& [host, ip]: s.exact_hosts) {
		if (host == name) {
			*addr = ip;
			*how  = "mapped";
			return HostResult::Mapped;
		}
	}
	for (const auto& [suffix, ip]: s.suffix_hosts) {
		if (name.size() > suffix.size() &&
		    name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
			*addr = ip;
			*how  = "mapped (wildcard *" + suffix + ")";
			return HostResult::Mapped;
		}
	}
	if (s.block_unlisted && name != "localhost") {
		*how = "blocked";
		return HostResult::Blocked;
	}
	*how = "system resolver";
	return HostResult::NotHandled;
}

} // namespace

namespace {

// Faceoff: EA and PlayStation Network names never reach the system resolver, whatever the config
// says. Mapped hosts are answered before this check, so play through the CrossCrease server is
// unaffected; an unmapped EA name gets "no such host" instead of a real DNS lookup.
bool GuardedName(std::string name) {
	name = Lower(std::move(name));
	while (!name.empty() && name.back() == '.') {
		name.pop_back();
	}
	static const char* const guarded[] = {"ea.com", "easports.com", "tnt-ea.com", "playstation.net",
	                                      "playstation.com"};
	for (const char* suffix: guarded) {
		const std::string tail(suffix);
		if (name == tail || (name.size() > tail.size() &&
		                     name.compare(name.size() - tail.size(), tail.size(), tail) == 0 &&
		                     name[name.size() - tail.size() - 1] == '.')) {
			return true;
		}
	}
	return false;
}

} // namespace

HostResult ResolveHost(const char* hostname, uint32_t* addr) {
	const auto& s = Get();
	if (hostname == nullptr) {
		return HostResult::NotHandled;
	}
	// Faceoff: fail closed. A config that was named but can't be used blocks every name.
	if (s.config_failed) {
		return HostResult::Blocked;
	}
	if (!s.enabled) {
		return GuardedName(hostname) ? HostResult::Blocked : HostResult::NotHandled;
	}
	std::string how;
	auto        result = MatchHost(hostname, addr, &how);
	if (result == HostResult::NotHandled && GuardedName(hostname)) {
		result = HostResult::Blocked;
		how    = "blocked (EA/PSN name)";
	}
	Trace("dns %s -> %s", hostname, how.c_str());
	return result;
}

uint32_t LocalAddress() {
	return Get().local_address;
}

uint16_t RemapPort(uint32_t addr, uint16_t port) {
	const auto& s = Get();
	if (!s.enabled || s.port_map.empty()) {
		return port;
	}
	const auto mapped = [&](const auto& hosts) {
		return std::any_of(hosts.begin(), hosts.end(), [&](const auto& h) { return h.second == addr; });
	};
	if (!mapped(s.exact_hosts) && !mapped(s.suffix_hosts)) {
		return port;
	}
	for (const auto& [from, to]: s.port_map) {
		if (from == port) {
			Trace("connect port %u -> %u (port_map)", static_cast<unsigned>(port), static_cast<unsigned>(to));
			return to;
		}
	}
	return port;
}

std::string LocalAddressString() {
	return Get().local_address_text;
}

std::string OnlineId(const std::string& fallback) {
	const auto& s = Get();
	return s.online_id.empty() ? fallback : s.online_id;
}

uint64_t AccountId(uint64_t fallback) {
	const auto& s = Get();
	return s.account_id == 0 ? fallback : s.account_id;
}

bool AuthorizationCode(const char* scope, char* out, size_t size, int* issuer_id) {
	const auto& s = Get();
	Trace("npauth authorization code requested scope=%s -> %s", scope != nullptr ? scope : "-",
	      s.auth_code.empty() ? "no token configured" : "token issued");
	if (s.auth_code.empty() || out == nullptr || size == 0) {
		return false;
	}
	const size_t n = std::min(s.auth_code.size(), size - 1);
	std::memcpy(out, s.auth_code.data(), n);
	out[n] = '\0';
	if (issuer_id != nullptr) {
		*issuer_id = s.issuer_id;
	}
	return true;
}

namespace {

struct ParsedUrl {
	std::string scheme;
	std::string host;
	std::string path;
	uint16_t    port = 0;
};

bool ParseUrl(const std::string& url, ParsedUrl* out) {
	const auto sep = url.find("://");
	if (sep == std::string::npos) {
		return false;
	}
	out->scheme      = Lower(url.substr(0, sep));
	const auto rest  = url.substr(sep + 3);
	const auto slash = rest.find_first_of("/?#");
	auto authority   = rest.substr(0, slash);
	out->path        = slash == std::string::npos ? "/" : rest.substr(slash);
	if (out->path[0] != '/') {
		out->path.insert(0, "/");
	}
	if (auto at = authority.rfind('@'); at != std::string::npos) {
		authority = authority.substr(at + 1);
	}
	out->port = 0;
	if (auto colon = authority.rfind(':');
	    colon != std::string::npos && authority.find(']') == std::string::npos) {
		out->port = static_cast<uint16_t>(std::strtoul(authority.c_str() + colon + 1, nullptr, 10));
		authority = authority.substr(0, colon);
	}
	if (out->port == 0) {
		out->port = out->scheme == "https" ? 443 : 80;
	}
	out->host = authority;
	return !out->host.empty();
}

#if defined(_WIN32)
using Socket                     = SOCKET;
using SockLen                    = int;
constexpr Socket INVALID_SOCKET_ = INVALID_SOCKET;
void CloseSocket(Socket s) {
	closesocket(s);
}
#else
using Socket                     = int;
using SockLen                    = socklen_t;
constexpr Socket INVALID_SOCKET_ = -1;
void CloseSocket(Socket s) {
	close(s);
}
#endif

bool SocketsReady() {
#if defined(_WIN32)
	static std::once_flag once;
	static bool           ready = false;
	std::call_once(once, [] {
		WSADATA data {};
		ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	});
	return ready;
#else
	return true;
#endif
}

void SetBlocking(Socket s, bool blocking) {
#if defined(_WIN32)
	u_long mode = blocking ? 0 : 1;
	ioctlsocket(s, FIONBIO, &mode);
#else
	const int flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

void SetTimeouts(Socket s, uint32_t ms) {
#if defined(_WIN32)
	DWORD value = ms;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
	timeval value {static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
}

Socket ConnectTo(uint32_t addr, uint16_t port, uint32_t timeout_ms) {
	if (!SocketsReady()) {
		return INVALID_SOCKET_;
	}
	Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET_) {
		return s;
	}
	sockaddr_in to {};
	to.sin_family = AF_INET;
	to.sin_port   = htons(port);
	std::memcpy(&to.sin_addr, &addr, sizeof(addr));
	if (const uint32_t local = Get().local_address; local != 0) {
		// Same-PC players use distinct loopback addresses; keep the request on this player's one.
		sockaddr_in from {};
		from.sin_family = AF_INET;
		std::memcpy(&from.sin_addr, &local, sizeof(local));
		bind(s, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
	}
	SetBlocking(s, false);
	connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
	fd_set writable;
	fd_set failed;
	FD_ZERO(&writable);
	FD_ZERO(&failed);
	FD_SET(s, &writable);
	FD_SET(s, &failed);
	timeval wait {};
	wait.tv_sec   = static_cast<decltype(wait.tv_sec)>(timeout_ms / 1000);
	wait.tv_usec  = static_cast<decltype(wait.tv_usec)>((timeout_ms % 1000) * 1000);
	int     error = 0;
	SockLen error_len = sizeof(error);
	if (select(static_cast<int>(s) + 1, nullptr, &writable, &failed, &wait) != 1 ||
	    !FD_ISSET(s, &writable) ||
	    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_len) != 0 ||
	    error != 0) {
		CloseSocket(s);
		return INVALID_SOCKET_;
	}
	SetBlocking(s, true);
	return s;
}

bool SendAll(Socket s, const std::string& data) {
	size_t sent = 0;
	while (sent < data.size()) {
		const int n =
		    send(s, data.data() + sent, static_cast<int>(std::min<size_t>(data.size() - sent, 1u << 20u)), 0);
		if (n <= 0) {
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool IEquals(const std::string& a, const char* b) {
	return Lower(a) == b;
}

// `headers` starts with the status line; every line ends with CRLF.
std::string HeaderValue(const std::string& headers, const char* name) {
	const std::string key   = std::string("\r\n") + name + ":";
	const auto        lower = Lower(headers);
	const auto        pos   = lower.find(key);
	if (pos == std::string::npos) {
		return {};
	}
	auto       begin = pos + key.size();
	const auto end   = headers.find("\r\n", begin);
	while (begin < end && (headers[begin] == ' ' || headers[begin] == '\t')) {
		begin++;
	}
	return headers.substr(begin, end - begin);
}

bool Dechunk(const std::string& raw, std::string* body) {
	size_t pos = 0;
	for (;;) {
		const auto line_end = raw.find("\r\n", pos);
		if (line_end == std::string::npos) {
			return false;
		}
		const auto size = static_cast<size_t>(std::strtoull(raw.c_str() + pos, nullptr, 16));
		pos             = line_end + 2;
		if (size == 0) {
			return true;
		}
		if (pos + size > raw.size()) {
			return false;
		}
		body->append(raw, pos, size);
		pos += size + 2;
	}
}

} // namespace

bool HttpGatewayConfigured() {
	return Get().enabled && Get().http_gateway_port != 0;
}

HttpRoute HttpRouteFor(const std::string& url) {
	const auto& s = Get();
	ParsedUrl   parsed;
	if (s.config_failed) {
		return HttpRoute::Blocked; // Faceoff: fail closed
	}
	if (!s.enabled || !ParseUrl(url, &parsed)) {
		return HttpRoute::NotHandled;
	}
	uint32_t    addr = 0;
	std::string how;
	switch (MatchHost(parsed.host, &addr, &how)) {
		case HostResult::Mapped:
			if (s.http_gateway_port != 0) {
				return HttpRoute::Gateway;
			}
			Trace("http %s -> host mapped but no http_gateway configured", url.c_str());
			return HttpRoute::NotHandled;
		case HostResult::Blocked: Trace("http %s -> blocked", url.c_str()); return HttpRoute::Blocked;
		default: Trace("http %s -> not handled", url.c_str()); return HttpRoute::NotHandled;
	}
}

void HttpPerform(HttpExchange* x) {
	const auto& s       = Get();
	const auto  started = std::chrono::steady_clock::now();
	ParsedUrl   url;
	if (!ParseUrl(x->url, &url)) {
		x->error = HttpExchange::Error::Connect;
		return;
	}
	const bool default_port =
	    (url.scheme == "https" && url.port == 443) || (url.scheme == "http" && url.port == 80);

	std::string request = x->method + " " + url.path + " HTTP/1.1\r\n";
	request += "Host: " + url.host + (default_port ? std::string {} : ":" + std::to_string(url.port)) +
	           "\r\n";
	request += "X-CrossCrease-Scheme: " + url.scheme + "\r\n";
	request += "X-CrossCrease-Host: " + url.host + "\r\n";
	request += "X-CrossCrease-Port: " + std::to_string(url.port) + "\r\n";
	std::string traced_headers;
	for (const auto& [name, value]: x->headers) {
		traced_headers += " " + name;
		if (IEquals(name, "host") || IEquals(name, "content-length") || IEquals(name, "connection") ||
		    IEquals(name, "transfer-encoding") || IEquals(name, "accept-encoding")) {
			continue;
		}
		request += name + ": " + value + "\r\n";
	}
	if (!x->body.empty() || x->method == "POST" || x->method == "PUT" || x->method == "PATCH") {
		request += "Content-Length: " + std::to_string(x->body.size()) + "\r\n";
	}
	request += "Connection: close\r\n\r\n";
	request += x->body;
	Trace("http %s %s -> gateway %s (headers:%s; body %zu bytes)", x->method.c_str(), x->url.c_str(),
	      s.http_gateway_text.c_str(), traced_headers.c_str(), x->body.size());

	const Socket sock = ConnectTo(s.http_gateway_addr, s.http_gateway_port, x->connect_timeout_ms);
	if (sock == INVALID_SOCKET_) {
		x->error = HttpExchange::Error::Connect;
		Trace("http %s %s <- gateway connect failed", x->method.c_str(), x->url.c_str());
		return;
	}
	SetTimeouts(sock, x->recv_timeout_ms);
	std::string raw;
	if (SendAll(sock, request)) {
		char buffer[16384];
		for (;;) {
			const int n = recv(sock, buffer, sizeof(buffer), 0);
			if (n <= 0 || raw.size() > (64u << 20u)) {
				break;
			}
			raw.append(buffer, static_cast<size_t>(n));
		}
	}
	CloseSocket(sock);

	const auto header_end = raw.find("\r\n\r\n");
	if (raw.compare(0, 5, "HTTP/") != 0 || header_end == std::string::npos) {
		x->error = raw.empty() ? HttpExchange::Error::Io : HttpExchange::Error::BadResponse;
		Trace("http %s %s <- no valid response (%zu bytes)", x->method.c_str(), x->url.c_str(), raw.size());
		return;
	}
	const std::string head  = raw.substr(0, header_end + 2);
	const auto        space = head.find(' ');
	x->status               = space == std::string::npos ? 0 : std::atoi(head.c_str() + space + 1);
	std::string rest        = raw.substr(header_end + 4);
	if (Lower(HeaderValue(head, "transfer-encoding")).find("chunked") != std::string::npos) {
		if (!Dechunk(rest, &x->response_body)) {
			x->error = HttpExchange::Error::BadResponse;
		}
	} else if (auto length = HeaderValue(head, "content-length"); !length.empty()) {
		x->response_body = rest.substr(0, static_cast<size_t>(std::strtoull(length.c_str(), nullptr, 10)));
	} else {
		x->response_body = std::move(rest);
	}
	// Report the body as delivered: no chunked framing, exact length.
	std::string headers;
	size_t      pos = 0;
	while (pos < head.size()) {
		const auto end  = head.find("\r\n", pos);
		auto       line = head.substr(pos, end - pos);
		pos             = end + 2;
		const auto lower = Lower(line);
		if (lower.rfind("transfer-encoding:", 0) == 0 || lower.rfind("content-length:", 0) == 0) {
			continue;
		}
		headers += line + "\r\n";
	}
	headers += "Content-Length: " + std::to_string(x->response_body.size()) + "\r\n\r\n";
	x->response_headers = std::move(headers);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                    std::chrono::steady_clock::now() - started)
	                    .count();
	Trace("http %s %s <- %d, %zu bytes, %lld ms", x->method.c_str(), x->url.c_str(), x->status,
	      x->response_body.size(), static_cast<long long>(ms));
}

void Trace(const char* format, ...) {
	if (g_trace == nullptr && !Get().enabled) {
		return;
	}
	std::lock_guard lock(g_trace_mutex);
	if (g_trace == nullptr) {
		return;
	}
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::system_clock::now().time_since_epoch())
	                     .count();
	std::fprintf(g_trace, "%lld.%03lld ", static_cast<long long>(now / 1000),
	             static_cast<long long>(now % 1000));
	va_list args;
	va_start(args, format);
	std::vfprintf(g_trace, format, args);
	va_end(args);
	std::fputc('\n', g_trace);
	std::fflush(g_trace);
}

void TraceCall(const char* library, const char* module, const char* function) {
	if (!Enabled()) {
		return;
	}
	static std::mutex                                mutex;
	static std::unordered_map<const char*, uint32_t> counts; // keyed by the literal __func__
	const uint32_t                                   limit = Get().trace_call_limit;
	uint32_t                                         count = 0;
	{
		std::lock_guard lock(mutex);
		auto [entry, inserted] = counts.try_emplace(function, 0);
		if (inserted) {
			for (const auto& name: Get().trace_call_exclude) {
				if (name == function) {
					entry->second = limit; // never traced
				}
			}
		}
		count = ++entry->second;
	}
	if (count <= limit) {
		Trace("call %s::%s::%s%s", library, module, function, count == limit ? " (further calls not traced)" : "");
	}
}

void TraceSockaddr(const char* what, int guest_socket, const void* host_sockaddr) {
	if (!Enabled() || host_sockaddr == nullptr) {
		return;
	}
	const auto* in = static_cast<const sockaddr_in*>(host_sockaddr);
	if (in->sin_family != AF_INET) {
		return;
	}
	char text[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
	Trace("%s s=%d %s:%u", what, guest_socket, text, static_cast<unsigned>(ntohs(in->sin_port)));
}

} // namespace Libs::CrossCreaseOnline
