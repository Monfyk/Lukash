#include "fortnite.h"

#include <cstdio>
#include <cstdlib>

#include "core.h"

namespace fn {

const char* kExe = "FortniteClient-Win64-Shipping.exe";
static std::string g_manual;

static std::string ReadFile(const std::string& path) {
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// Значення "key": "value" у JSON-об'єкті (з розекрануванням \\ і \")
static std::string JsonValue(const std::string& obj, const std::string& key) {
    size_t p = obj.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    p = obj.find(':', p);
    if (p == std::string::npos) return "";
    p = obj.find('"', p);
    if (p == std::string::npos) return "";
    std::string v;
    for (size_t i = p + 1; i < obj.size(); ++i) {
        char c = obj[i];
        if (c == '\\' && i + 1 < obj.size()) {
            v += obj[++i];
        } else if (c == '"') {
            break;
        } else {
            v += c;
        }
    }
    return v;
}

std::string FindInstallDir() {
    // Epic Games Launcher зберігає список встановлених ігор у цьому файлі
    std::string pd = lb::GetEnv("ProgramData");
    if (pd.empty()) pd = "C:\\ProgramData";
    std::string data = ReadFile(pd + "\\Epic\\UnrealEngineLauncher\\LauncherInstalled.dat");
    size_t pos = 0;
    while ((pos = data.find('{', pos)) != std::string::npos) {
        size_t end = data.find('}', pos);
        if (end == std::string::npos) break;
        std::string obj = data.substr(pos, end - pos + 1);
        if (JsonValue(obj, "AppName") == "Fortnite") {
            std::string dir = JsonValue(obj, "InstallLocation");
            if (!dir.empty()) return dir;
        }
        pos = end + 1;
    }
    std::string pf = lb::GetEnv("ProgramFiles");
    if (pf.empty()) pf = "C:\\Program Files";
    std::string def = pf + "\\Epic Games\\Fortnite";
    if (lb::FileExists(def + "\\FortniteGame\\Binaries\\Win64\\" + kExe)) return def;
    return "";
}

void SetManualDir(const std::string& dir) { g_manual = dir; }
std::string ManualDir() { return g_manual; }

std::string ExePath() {
    std::string dir = g_manual.empty() ? FindInstallDir() : g_manual;
    if (dir.empty()) return "";
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    std::string exe = dir + "\\FortniteGame\\Binaries\\Win64\\" + kExe;
    return lb::FileExists(exe) ? exe : "";
}

bool IsRunning() { return lb::FindProcess(kExe) != 0; }

// ---------------------------------------------------------------------------
// GameUserSettings.ini
// ---------------------------------------------------------------------------

static const char* kSection = "/Script/FortniteGame.FortGameUserSettings";
static const char* kRhiSection = "D3DRHIPreference";
static const char* kSgSection = "ScalabilityGroups";
// Групи якості Unreal Engine, які найбільше впливають на FPS. Дальність огляду і текстури не чіпаємо:
// дальність важлива для гри, а текстури майже не впливають на FPS (лише на відеопам'ять).
static const char* kLowSg[] = {"sg.ShadowQuality",      "sg.GlobalIlluminationQuality", "sg.ReflectionQuality",
                               "sg.PostProcessQuality", "sg.EffectsQuality",            "sg.FoliageQuality",
                               "sg.ShadingQuality",     "sg.AntiAliasingQuality"};

std::string SettingsPath() {
    std::string la = lb::GetEnv("LOCALAPPDATA");
    if (la.empty()) return "";
    return la + "\\FortniteGame\\Saved\\Config\\WindowsClient\\GameUserSettings.ini";
}

static std::string IniGet(const std::string& file, const char* sec, const char* key) {
    char buf[256];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), file.c_str());
    return buf;
}

Settings ReadSettings() {
    Settings s;
    std::string f = SettingsPath();
    if (f.empty() || !lb::FileExists(f)) return s;
    s.found = true;
    s.fpsLimit = (int)atof(IniGet(f, kSection, "FrameRateLimit").c_str());
    s.vsync = lb::ToLower(IniGet(f, kSection, "bUseVSync")) == "true";
    s.rhi = lb::ToLower(IniGet(f, kRhiSection, "PreferredRHI"));
    s.performanceMode = lb::ToLower(IniGet(f, kRhiSection, "PreferredFeatureLevel")) == "es31";
    std::string fm = IniGet(f, kSection, "FullscreenMode");
    s.fullscreen = fm == "0";
    s.lowGraphics = true;
    for (const char* k : kLowSg)
        if (IniGet(f, kSgSection, k) != "0") s.lowGraphics = false;
    return s;
}

bool HasSettingsBackup() { return lb::FileExists(SettingsPath() + ".kairo-backup"); }

bool WriteSettings(int fpsLimit, bool vsync, bool performanceMode, bool fullscreen, bool lowGraphics) {
    std::string f = SettingsPath();
    if (f.empty() || !lb::FileExists(f)) {
        lb::Log("[!] Не знайдено GameUserSettings.ini — запусти Fortnite хоча б раз");
        return false;
    }
    if (IsRunning()) {
        lb::Log("[!] Закрий Fortnite: під час виходу гра перезапише налаштування");
        return false;
    }
    std::string bak = f + ".kairo-backup";
    if (!lb::FileExists(bak)) CopyFileA(f.c_str(), bak.c_str(), TRUE);

    // Файл може бути "лише для читання" — знімаємо атрибут на час запису
    DWORD attr = GetFileAttributesA(f.c_str());
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(f.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);

    char fps[32];
    snprintf(fps, sizeof(fps), "%d.000000", fpsLimit);
    bool ok = WritePrivateProfileStringA(kSection, "FrameRateLimit", fps, f.c_str()) &&
              WritePrivateProfileStringA(kSection, "bUseVSync", vsync ? "True" : "False", f.c_str());
    if (performanceMode) {
        ok = ok && WritePrivateProfileStringA(kRhiSection, "PreferredRHI", "dx11", f.c_str()) &&
             WritePrivateProfileStringA(kRhiSection, "PreferredFeatureLevel", "es31", f.c_str());
    } else if (lb::ToLower(IniGet(f, kRhiSection, "PreferredFeatureLevel")) == "es31") {
        // Вимикаємо режим продуктивності -> звичайний DirectX 11
        ok = ok && WritePrivateProfileStringA(kRhiSection, "PreferredFeatureLevel", "sm5", f.c_str());
    }
    if (fullscreen) {
        // 0 = повноекранний, 1 = вікно без рамки, 2 = вікно
        for (const char* k : {"FullscreenMode", "LastConfirmedFullscreenMode", "PreferredFullscreenMode"})
            ok = ok && WritePrivateProfileStringA(kSection, k, "0", f.c_str());
    }
    if (lowGraphics)
        for (const char* k : kLowSg) ok = ok && WritePrivateProfileStringA(kSgSection, k, "0", f.c_str());
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(f.c_str(), attr);
    lb::Log("[%s] Налаштування Fortnite: FPS %s, VSync %s, режим продуктивності %s%s%s", ok ? "OK" : "ПОМИЛКА",
            fpsLimit ? std::to_string(fpsLimit).c_str() : "без обмеження", vsync ? "увімк" : "вимк",
            performanceMode ? "увімк" : "вимк", fullscreen ? ", повноекранний режим" : "",
            lowGraphics ? ", мінімальна графіка" : "");
    return ok;
}

bool RestoreSettingsBackup() {
    std::string f = SettingsPath(), bak = f + ".kairo-backup";
    if (!lb::FileExists(bak)) return false;
    SetFileAttributesA(f.c_str(), FILE_ATTRIBUTE_NORMAL);
    bool ok = CopyFileA(bak.c_str(), f.c_str(), FALSE) && DeleteFileA(bak.c_str());
    lb::Log("[%s] Налаштування Fortnite відновлено з копії", ok ? "OK" : "ПОМИЛКА");
    return ok;
}

// ---------------------------------------------------------------------------

static int DeleteFilesIn(const std::string& dir) {
    int n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string p = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            n += DeleteFilesIn(p);
            RemoveDirectoryA(p.c_str());
        } else if (DeleteFileA(p.c_str())) {
            ++n;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

int CleanLogs() {
    std::string la = lb::GetEnv("LOCALAPPDATA");
    if (la.empty()) return 0;
    if (IsRunning()) {
        lb::Log("[!] Закрий Fortnite перед очищенням");
        return 0;
    }
    std::string base = la + "\\FortniteGame\\Saved\\";
    int n = DeleteFilesIn(base + "Logs") + DeleteFilesIn(base + "Crashes");
    lb::Log("[OK] Видалено файлів логів і звітів про збої Fortnite: %d", n);
    return n;
}

const std::vector<Region>& Regions() {
    // Офіційні адреси для вимірювання пінгу до дата-центрів Fortnite (зі статті підтримки Epic
    // "Fortnite latency and ping troubleshooting")
    static const std::vector<Region> r = {
        {"EU", "Європа", "ping-eu.ds.on.epicgames.com"},
        {"EU-DE", "Європа: Німеччина", "ping-de.ds.on.epicgames.com"},
        {"EU-FR", "Європа: Франція", "ping-fr.ds.on.epicgames.com"},
        {"EU-GB", "Європа: Велика Британія", "ping-gb.ds.on.epicgames.com"},
        {"NAE", "Пн. Америка (схід)", "ping-nae.ds.on.epicgames.com"},
        {"NAC", "Пн. Америка (центр)", "ping-nac.ds.on.epicgames.com"},
        {"NAW", "Пн. Америка (захід)", "ping-naw.ds.on.epicgames.com"},
        {"BR", "Бразилія", "ping-br.ds.on.epicgames.com"},
        {"ME", "Близький Схід", "ping-me.ds.on.epicgames.com"},
        {"ASIA", "Азія", "ping-asia.ds.on.epicgames.com"},
        {"OCE", "Океанія", "ping-oce.ds.on.epicgames.com"},
    };
    return r;
}

}  // namespace fn
