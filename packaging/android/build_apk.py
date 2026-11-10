#!/usr/bin/env python3
"""Збирає Akordnyk.apk без Android SDK.

Кроки:
  1. javac компілює MainActivity (проти мінімальних заглушок Android API);
  2. dx (dalvik-dx з Maven Central) перетворює класи в classes.dex;
  3. цей скрипт сам пише бінарний AndroidManifest.xml і resources.arsc (іконка);
  4. пакує APK (resources.arsc без стиснення, вирівняний на 4 байти);
  5. apksig підписує схемою v2 і перевіряє підпис.

Потрібно: Java 17+, python3, файли .tools/dx.jar і .tools/apksig.jar
(скачуються з Maven Central автоматично). Результат: packaging/out/Akordnyk.apk
"""
import glob, os, shutil, struct, subprocess, sys, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")
TOOLS = os.path.join(HERE, ".tools")
OUT = os.path.join(ROOT, "out")
WWW = os.path.join(OUT, "www", "index.html")

PACKAGE = "ua.akordnyk.app"
LABEL = "Акордник"
VERSION_CODE, VERSION_NAME = 1, "1.0"
MIN_SDK, TARGET_SDK = 24, 34  # Android 7.0+ (підпис v2)
DENSITIES = [("mdpi", 160), ("hdpi", 240), ("xhdpi", 320), ("xxhdpi", 480), ("xxxhdpi", 640)]
MAVEN = "https://repo.maven.apache.org/maven2/"
DEPS = {
    "dx.jar": ("com/jakewharton/android/repackaged/dalvik-dx/16.0.1/dalvik-dx-16.0.1.jar", "71ac16d1f34143d86440fca923422f9c2e2ac403"),
    "apksig.jar": ("com/android/tools/build/apksig/2.3.0/apksig-2.3.0.jar", "6ef7a58375aa68fb492b58edd97607bee8ee5c5c"),
}

def run(*cmd):
    print("$", " ".join(cmd)); subprocess.run(cmd, check=True)

def fetch_tools():
    import hashlib
    os.makedirs(TOOLS, exist_ok=True)
    for name, (path, sha1) in DEPS.items():
        dst = os.path.join(TOOLS, name)
        if not os.path.exists(dst):
            urllib.request.urlretrieve(MAVEN + path, dst)
        if hashlib.sha1(open(dst, "rb").read()).hexdigest() != sha1:
            os.remove(dst); sys.exit(f"Контрольна сума {name} не збігається")

# ---------------------------------------------------------------- рядки
def string_pool(strings):
    """ResStringPool у UTF-16."""
    offs, data = [], b""
    for s in strings:
        offs.append(len(data))
        u = s.encode("utf-16-le")
        n = len(u) // 2
        data += (struct.pack("<H", n) if n < 0x8000 else struct.pack("<HH", 0x8000 | (n >> 16), n & 0xFFFF)) + u + b"\0\0"
    while len(data) % 4: data += b"\0"
    hdr = 28; start = hdr + 4 * len(strings)
    body = b"".join(struct.pack("<I", o) for o in offs) + data
    return struct.pack("<HHIIIIII", 0x0001, hdr, hdr + len(body), len(strings), 0, 0, start, 0) + body

# ---------------------------------------------------------------- AndroidManifest.xml
ANDROID_NS = "http://schemas.android.com/apk/res/android"
ATTR_IDS = {"label": 0x01010001, "icon": 0x01010002, "name": 0x01010003, "exported": 0x01010010,
            "configChanges": 0x0101001f, "minSdkVersion": 0x0101020c, "versionCode": 0x0101021b,
            "versionName": 0x0101021c, "targetSdkVersion": 0x01010270, "allowBackup": 0x01010280,
            "hardwareAccelerated": 0x010102d3}
T_REF, T_STR, T_DEC, T_HEX, T_BOOL = 0x01, 0x03, 0x10, 0x11, 0x12

def manifest_tree(icon_id):
    A = lambda n, t, v: ("android", n, t, v)
    return ("manifest", [(None, "package", T_STR, PACKAGE), A("versionCode", T_DEC, VERSION_CODE), A("versionName", T_STR, VERSION_NAME)], [
        ("uses-sdk", [A("minSdkVersion", T_DEC, MIN_SDK), A("targetSdkVersion", T_DEC, TARGET_SDK)], []),
        ("application", [A("label", T_STR, LABEL), A("icon", T_REF, icon_id), A("allowBackup", T_BOOL, True), A("hardwareAccelerated", T_BOOL, True)], [
            ("activity", [A("name", T_STR, PACKAGE + ".MainActivity"), A("exported", T_BOOL, True), A("configChanges", T_HEX, 0x0FB0)], [
                ("intent-filter", [], [
                    ("action", [A("name", T_STR, "android.intent.action.MAIN")], []),
                    ("category", [A("name", T_STR, "android.intent.category.LAUNCHER")], []),
                ]),
            ]),
        ]),
    ])

def encode_axml(tree):
    # пул рядків: спершу імена атрибутів з resource id (порядок = resource map)
    attr_names, others = [], []
    def collect(node):
        tag, attrs, kids = node
        for ns, n, t, v in attrs:
            if ns == "android" and n not in attr_names: attr_names.append(n)
        for k in kids: collect(k)
    collect(tree)
    attr_names.sort(key=lambda n: ATTR_IDS[n])
    pool = list(attr_names)
    def sid(s):
        if s not in pool: pool.append(s)
        return pool.index(s)
    for s in ["android", ANDROID_NS]: sid(s)
    def collect2(node):
        tag, attrs, kids = node
        sid(tag)
        for ns, n, t, v in attrs:
            sid(n)
            if t == T_STR: sid(v)
        for k in kids: collect2(k)
    collect2(tree)

    NONE = 0xFFFFFFFF
    chunks = []
    chunks.append(struct.pack("<HHIIIII", 0x0100, 16, 24, 1, NONE, sid("android"), sid(ANDROID_NS)))
    def emit(node, line=[1]):
        tag, attrs, kids = node
        line[0] += 1
        enc = []
        for ns, n, t, v in sorted(attrs, key=lambda a: ATTR_IDS.get(a[1], 0x7fffffff) if a[0] == "android" else -1):
            nsi = sid(ANDROID_NS) if ns == "android" else NONE
            if t == T_STR: raw, data = sid(v), sid(v)
            elif t == T_BOOL: raw, data = NONE, (0xFFFFFFFF if v else 0)
            else: raw, data = NONE, v
            enc.append(struct.pack("<IIIHBBI", nsi, sid(n), raw, 8, 0, t, data))
        body = struct.pack("<IIHHHHHH", NONE, sid(tag), 20, 20, len(enc), 0, 0, 0) + b"".join(enc)
        chunks.append(struct.pack("<HHIII", 0x0102, 16, 16 + len(body), line[0], NONE) + body)
        for k in kids: emit(k, line)
        chunks.append(struct.pack("<HHIIIII", 0x0103, 16, 24, line[0], NONE, NONE, sid(tag)))
    emit(tree)
    chunks.append(struct.pack("<HHIIIII", 0x0101, 16, 24, 1, NONE, sid("android"), sid(ANDROID_NS)))
    sp = string_pool(pool)
    rmap = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(attr_names)) + b"".join(struct.pack("<I", ATTR_IDS[n]) for n in attr_names)
    body = sp + rmap + b"".join(chunks)
    return struct.pack("<HHI", 0x0003, 8, 8 + len(body)) + body

# ---------------------------------------------------------------- resources.arsc
def encode_arsc():
    paths = [f"res/mipmap-{d}-v4/ic_launcher.png" for d, _ in DENSITIES]
    gpool = string_pool(paths)
    types = string_pool(["mipmap"])
    keys = string_pool(["ic_launcher"])
    spec = struct.pack("<HHIBBHI", 0x0202, 16, 16 + 4, 1, 0, 0, 1) + struct.pack("<I", 0x0100)  # змінюється від density
    type_chunks = b""
    for i, (_, dens) in enumerate(DENSITIES):
        cfg = struct.pack("<IHH2s2sBBHBBBBHHHHBBHHH4s8sBBH", 64, 0, 0, b"\0\0", b"\0\0", 0, 0, dens,
                          0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, b"\0" * 4, b"\0" * 8, 0, 0, 0)
        cfg += b"\0" * (64 - len(cfg))
        hdr = 20 + len(cfg)
        entries = struct.pack("<HHI", 8, 0, 0) + struct.pack("<HBBI", 8, 0, T_STR, i)
        offsets = struct.pack("<I", 0)
        start = hdr + len(offsets)
        type_chunks += struct.pack("<HHIBBHII", 0x0201, hdr, start + len(entries), 1, 0, 0, 1, start) + cfg + offsets + entries
    pkg_hdr = 288
    name = PACKAGE.encode("utf-16-le").ljust(256, b"\0")
    pkg_body = types + keys + spec + type_chunks
    pkg = struct.pack("<HHII", 0x0200, pkg_hdr, pkg_hdr + len(pkg_body), 0x7f) + name + \
          struct.pack("<IIIII", pkg_hdr, 1, pkg_hdr + len(types), 1, 0) + pkg_body
    body = gpool + pkg
    return struct.pack("<HHII", 0x0002, 12, 12 + len(body), 1) + body, paths

# ---------------------------------------------------------------- zip
def write_apk(dst, files):
    """files: [(ім'я, байти, stored?)] — stored-файли вирівнюються на 4 байти."""
    with zipfile.ZipFile(dst, "w") as z:
        for name, data, stored in files:
            zi = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            zi.create_system = 0
            if stored:
                zi.compress_type = zipfile.ZIP_STORED
                off = z.fp.tell() + 30 + len(name.encode())
                pad = (-off) % 4
                if pad: zi.extra = b"\0" * pad  # як zipalign: нулі в extra-полі
            else:
                zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, data)

def main():
    if not os.path.exists(WWW):
        sys.exit("Спершу запусти packaging/offline.py")
    fetch_tools()
    shutil.rmtree(BUILD, ignore_errors=True)
    os.makedirs(BUILD); os.makedirs(OUT, exist_ok=True)
    stubs_cls, cls = os.path.join(BUILD, "stubs"), os.path.join(BUILD, "classes")
    run("javac", "-nowarn", "-d", stubs_cls, *glob.glob(os.path.join(HERE, "stubs", "**", "*.java"), recursive=True))
    run("javac", "--release", "8", "-nowarn", "-Xlint:-options", "-cp", stubs_cls, "-d", cls,
        *glob.glob(os.path.join(HERE, "src", "**", "*.java"), recursive=True))
    dex = os.path.join(BUILD, "classes.dex")
    run("java", "-cp", os.path.join(TOOLS, "dx.jar"), "com.android.dx.command.Main", "--dex",
        f"--min-sdk-version={MIN_SDK}", f"--output={dex}", cls)

    arsc, paths = encode_arsc()
    manifest = encode_axml(manifest_tree(0x7f010000))
    files = [("AndroidManifest.xml", manifest, False), ("classes.dex", open(dex, "rb").read(), False),
             ("resources.arsc", arsc, True)]
    for (d, _), p in zip(DENSITIES, paths):
        files.append((p, open(os.path.join(HERE, "res", f"mipmap-{d}", "ic_launcher.png"), "rb").read(), True))
    files.append(("assets/www/index.html", open(WWW, "rb").read(), False))
    unsigned = os.path.join(BUILD, "unsigned.apk")
    write_apk(unsigned, files)

    ks_dir = os.path.join(HERE, ".keystore"); ks = os.path.join(ks_dir, "akordnyk.p12")
    pw = os.environ.get("AKORDNYK_KEYSTORE_PASS", "akordnyk")
    if not os.path.exists(ks):
        os.makedirs(ks_dir, exist_ok=True)
        run("keytool", "-genkeypair", "-keystore", ks, "-storetype", "PKCS12", "-storepass", pw, "-keypass", pw,
            "-alias", "akordnyk", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname", "CN=Akordnyk")
    signer_cls = os.path.join(BUILD, "signer")
    run("javac", "-nowarn", "-cp", os.path.join(TOOLS, "apksig.jar"), "-d", signer_cls, os.path.join(HERE, "signer", "Sign.java"))
    out = os.path.join(OUT, "Akordnyk.apk")
    opens = [f"--add-exports=java.base/sun.security.{m}=ALL-UNNAMED" for m in ("x509", "pkcs", "util")]
    run("java", *opens, "-cp", os.pathsep.join([signer_cls, os.path.join(TOOLS, "apksig.jar")]), "Sign", unsigned, out, ks, pw, "akordnyk")
    print("OK:", out, os.path.getsize(out) // 1024, "КБ")

if __name__ == "__main__":
    main()
