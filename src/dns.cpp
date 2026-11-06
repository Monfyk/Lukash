#include "dns.h"

#include "core.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstring>

namespace dns {

const std::vector<Provider>& Providers() {
    static const std::vector<Provider> p = {
        {"Cloudflare", "Швидкий і приватний, без фільтрації", {"1.1.1.1", "1.0.0.1"},
         {"2606:4700:4700::1111", "2606:4700:4700::1001"}},
        {"Quad9", "Блокує шкідливі сайти", {"9.9.9.9", "149.112.112.112"}, {"2620:fe::fe", "2620:fe::9"}},
        {"NextDNS", "Без налаштованого профілю — звичайний DNS", {"45.90.28.0", "45.90.30.0"},
         {"2a07:a8c0::", "2a07:a8c1::"}},
        {"Google", "Стабільний, без фільтрації", {"8.8.8.8", "8.8.4.4"}, {"2001:4860:4860::8888", "2001:4860:4860::8844"}},
        {"Comodo Secure", "Блокує шкідливі та фішингові сайти", {"8.26.56.26", "8.20.247.20"}, {nullptr, nullptr}},
        {"AdGuard", "Блокує рекламу і трекери", {"94.140.14.14", "94.140.15.15"},
         {"2a10:50c0::ad1:ff", "2a10:50c0::ad2:ff"}},
        {"OpenDNS", "Від Cisco, блокує фішинг", {"208.67.222.222", "208.67.220.220"},
         {"2620:119:35::35", "2620:119:53::53"}},
        {"Control D", "Без фільтрації", {"76.76.2.0", "76.76.10.0"}, {"2606:1a40::", "2606:1a40:1::"}},
    };
    return p;
}

// ---------------------------------------------------------------------------
// DNS-запит
// ---------------------------------------------------------------------------

double QueryMs(const std::string& server, const char* domain, int timeoutMs, bool strict) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    if (inet_pton(AF_INET, server.c_str(), &addr.sin_addr) != 1) return -1;

    // Заголовок: випадковий id, RD=1, 1 запитання
    unsigned char pkt[512];
    unsigned short id = (unsigned short)(GetTickCount() ^ (uintptr_t)&pkt);
    int n = 0;
    pkt[n++] = id >> 8;
    pkt[n++] = id & 0xff;
    pkt[n++] = 0x01;
    pkt[n++] = 0x00;
    pkt[n++] = 0;
    pkt[n++] = 1;
    for (int i = 0; i < 6; ++i) pkt[n++] = 0;
    // Ім'я: мітки
    const char* p = domain;
    while (*p) {
        const char* dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        pkt[n++] = (unsigned char)len;
        memcpy(pkt + n, p, len);
        n += len;
        p += len + (dot ? 1 : 0);
        if (!dot) break;
    }
    pkt[n++] = 0;
    pkt[n++] = 0;
    pkt[n++] = 1;  // A
    pkt[n++] = 0;
    pkt[n++] = 1;  // IN

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return -1;
    DWORD to = timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    double t0 = lb::NowMs();
    double result = -1;
    if (sendto(s, (const char*)pkt, n, 0, (sockaddr*)&addr, sizeof(addr)) == n) {
        unsigned char resp[1024];
        for (;;) {
            int r = recv(s, (char*)resp, sizeof(resp), 0);
            if (r <= 0) break;
            double dt = lb::NowMs() - t0;
            // Зараховуємо лише справжню відповідь: наш id, QR=1, без помилки (RCODE=0) і з адресою.
            // Популярні домени завжди мають адресу, тож порожня/помилкова відповідь — це збій або підміна.
            bool answer = r >= 12 && resp[0] == pkt[0] && resp[1] == pkt[1] && (resp[2] & 0x80) &&
                          (resp[3] & 0x0F) == 0 && ((resp[6] << 8) | resp[7]) > 0;
            if (answer || (!strict && r >= 12 && resp[0] == pkt[0] && resp[1] == pkt[1])) {
                result = dt;
                break;
            }
            if (dt > timeoutMs) break;
        }
    }
    closesocket(s);
    return result;
}

bool InterceptionDetected() {
    // 203.0.113.1 — документаційна адреса (TEST-NET-3), DNS-сервера там немає
    for (int i = 0; i < 2; ++i)
        if (QueryMs("203.0.113.1", "example.com", 800, false) >= 0) return true;
    return false;
}

Result Measure(const std::string& server, int count, const std::atomic<bool>* cancel,
               const std::function<void(double)>& sample) {
    static const char* domains[] = {"google.com",  "epicgames.com", "fortnite.com", "discord.com",
                                    "youtube.com", "microsoft.com", "twitch.tv",    "steamcommunity.com"};
    Result r;
    std::vector<double> ok;
    for (int i = 0; i < count; ++i) {
        if (cancel && *cancel) break;
        double ms = QueryMs(server, domains[i % 8]);
        ++r.total;
        if (ms >= 0) ok.push_back(ms);
        if (sample) sample(ms);
        Sleep(60);
    }
    r.ok = (int)ok.size();
    if (!ok.empty()) {
        // Відкидаємо найгірший запит (часто перший — "холодний" кеш сервера)
        std::sort(ok.begin(), ok.end());
        size_t use = ok.size() > 3 ? ok.size() - 1 : ok.size();
        double sum = 0;
        for (size_t i = 0; i < use; ++i) sum += ok[i];
        r.avg = sum / use;
        r.min = ok.front();
    }
    return r;
}

// ---------------------------------------------------------------------------
// Адаптери
// ---------------------------------------------------------------------------

static std::string Utf8(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

struct AdapterEx : Adapter {
    unsigned index6 = 0;
};

static std::vector<AdapterEx> List() {
    std::vector<AdapterEx> out;
    ULONG size = 32 * 1024;
    std::vector<BYTE> buf(size);
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        AdapterEx ad;
        ad.index = a->IfIndex;
        ad.index6 = a->Ipv6IfIndex;
        ad.ifType = a->IfType;
        ad.guid = a->AdapterName;
        ad.name = Utf8(a->FriendlyName);
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
            char ip[64];
            DWORD len = sizeof(ip);
            if (WSAAddressToStringA(d->Address.lpSockaddr, d->Address.iSockaddrLength, nullptr, ip, &len) != 0)
                continue;
            if (d->Address.lpSockaddr->sa_family == AF_INET)
                ad.dns4.push_back(ip);
            else
                ad.dns6.push_back(ip);
        }
        out.push_back(ad);
    }
    return out;
}

std::vector<Adapter> ActiveAdapters() {
    std::vector<Adapter> v;
    for (auto& a : List()) v.push_back(a);
    return v;
}

std::vector<std::string> CurrentServers() {
    std::vector<std::string> out;
    for (auto& a : List())
        for (auto& d : a.dns4)
            if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    return out;
}

int CurrentProvider() {
    auto cur = CurrentServers();
    if (cur.empty()) return -1;
    const auto& ps = Providers();
    for (int i = 0; i < (int)ps.size(); ++i)
        if (cur[0] == ps[i].v4[0] || cur[0] == ps[i].v4[1]) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Зміна DNS (через netsh — так само, як у налаштуваннях Windows)
// ---------------------------------------------------------------------------

static const char* kIf4 = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\";
static const char* kIf6 = "SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\";

static void Netsh(const std::string& args) {
    std::string cmd = "netsh.exe interface " + args;
    lb::RunHidden(cmd.c_str());
}

static void SetStatic(const char* fam, unsigned idx, const std::vector<std::string>& servers) {
    if (servers.empty()) return;
    std::string i = std::to_string(idx);
    Netsh(std::string(fam) + " set dnsservers name=" + i + " source=static address=" + servers[0] +
          " register=primary validate=no");
    for (size_t k = 1; k < servers.size(); ++k)
        Netsh(std::string(fam) + " add dnsservers name=" + i + " address=" + servers[k] + " index=" +
              std::to_string(k + 1) + " validate=no");
}

static void SetDhcp(const char* fam, unsigned idx) {
    Netsh(std::string(fam) + " set dnsservers name=" + std::to_string(idx) + " source=dhcp");
}

static void SaveOriginal(const AdapterEx& a) {
    std::string ns;
    lb::RegGetString(HKEY_LOCAL_MACHINE, kIf4 + a.guid, "NameServer", ns);
    lb::StateSave("dns4|" + a.guid, ns.empty() ? "dhcp" : ns);
    ns.clear();
    lb::RegGetString(HKEY_LOCAL_MACHINE, kIf6 + a.guid, "NameServer", ns);
    lb::StateSave("dns6|" + a.guid, ns.empty() ? "dhcp" : ns);
}

bool Apply(const Provider& p) {
    auto list = List();
    if (list.empty()) {
        lb::Log("[!] Не знайдено активного підключення до інтернету");
        return false;
    }
    for (auto& a : list) {
        SaveOriginal(a);
        SetStatic("ipv4", a.index, {p.v4[0], p.v4[1]});
        if (a.index6 && p.v6[0]) SetStatic("ipv6", a.index6, {p.v6[0], p.v6[1]});
        lb::Log("[OK] DNS %s встановлено для «%s»", p.name, a.name.c_str());
    }
    lb::FlushDns();
    return true;
}

bool Restore() {
    bool any = false;
    for (auto& a : List()) {
        std::string v4, v6;
        bool has4 = lb::StateLoad("dns4|" + a.guid, v4), has6 = lb::StateLoad("dns6|" + a.guid, v6);
        if (!has4 && !has6) continue;
        any = true;
        if (has4) {
            if (v4 == "dhcp") SetDhcp("ipv4", a.index); else SetStatic("ipv4", a.index, lb::Split(v4));
        }
        if (has6 && a.index6) {
            if (v6 == "dhcp") SetDhcp("ipv6", a.index6); else SetStatic("ipv6", a.index6, lb::Split(v6));
        }
        lb::StateDelete("dns4|" + a.guid);
        lb::StateDelete("dns6|" + a.guid);
        lb::Log("[OK] DNS для «%s» повернуто як було", a.name.c_str());
    }
    if (!any) {
        // KairoBoost нічого не змінював — просто вмикаємо автоматичний DNS від провайдера
        for (auto& a : List()) {
            SetDhcp("ipv4", a.index);
            if (a.index6) SetDhcp("ipv6", a.index6);
            lb::Log("[OK] Автоматичний DNS для «%s»", a.name.c_str());
        }
    }
    lb::FlushDns();
    return true;
}

bool OnWifi() {
    auto list = List();
    if (list.empty()) return false;
    for (auto& a : list)
        if (a.ifType != IF_TYPE_IEEE80211) return false;  // є дротове підключення
    return true;
}

bool IsChanged() {
    std::string v;
    for (auto& a : List())
        if (lb::StateLoad("dns4|" + a.guid, v)) return true;
    return false;
}

}  // namespace dns
