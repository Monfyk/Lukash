#!/usr/bin/env python3
"""KairoBoost — генератор ключів ліцензій.

Секретний ключ (keys-private/signing_key.pem) нікому не показуй і не публікуй:
з ним будь-хто зможе створювати дійсні ключі.

  python tools/keygen.py --new                       # створити пару ключів (один раз!)
  python tools/keygen.py standard 100 > standard.txt # 100 ключів Standard
  python tools/keygen.py pro 100 > pro.txt           # 100 ключів Pro
  python tools/keygen.py --check КЛЮЧ                # перевірити ключ
"""
import os, secrets, struct, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_DIR = os.path.join(HERE, "..", "keys-private")
PRIV = os.path.join(KEY_DIR, "signing_key.pem")
MSG_PREFIX = b"KairoBoost-License-v1"
ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"  # Crockford base32: без I, L, O, U
TIERS = {"standard": 1, "pro": 2}


def b32(data):
    bits = int.from_bytes(data, "big")
    n = (len(data) * 8 + 4) // 5
    bits <<= n * 5 - len(data) * 8
    return "".join(ALPHABET[(bits >> (5 * (n - 1 - i))) & 31] for i in range(n))


def unb32(text, nbytes):
    text = text.upper().replace("-", "").replace(" ", "").replace("O", "0").replace("I", "1").replace("L", "1")
    bits = 0
    for c in text:
        bits = bits * 32 + ALPHABET.index(c)
    extra = len(text) * 5 - nbytes * 8
    return (bits >> extra).to_bytes(nbytes, "big")


def load_priv():
    with open(PRIV, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def make_key(priv, tier, serial):
    payload = struct.pack("<BBI", 1, tier, serial)
    der = priv.sign(MSG_PREFIX + payload, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    raw = payload + r.to_bytes(32, "big") + s.to_bytes(32, "big")
    t = b32(raw)
    return "-".join(t[i:i + 8] for i in range(0, len(t), 8))


def check(pub, key):
    raw = unb32(key, 70)
    payload, sig = raw[:6], raw[6:]
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    pub.verify(der, MSG_PREFIX + payload, ec.ECDSA(hashes.SHA256()))
    ver, tier, serial = struct.unpack("<BBI", payload)
    return tier, serial


def main():
    a = sys.argv[1:]
    if a and a[0] == "--new":
        if os.path.exists(PRIV):
            sys.exit("Ключ уже існує: " + PRIV + " (старі ключі ліцензій перестали б працювати)")
        os.makedirs(KEY_DIR, exist_ok=True)
        priv = ec.generate_private_key(ec.SECP256R1())
        with open(PRIV, "wb") as f:
            f.write(priv.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                       serialization.NoEncryption()))
        n = priv.public_key().public_numbers()
        blob = struct.pack("<II", 0x31534345, 32) + n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")
        print("Публічний ключ для src/license.cpp (kPublicKey):")
        print(", ".join("0x%02X" % b for b in blob))
        return
    priv = load_priv()
    if a and a[0] == "--check":
        tier, serial = check(priv.public_key(), a[1])
        print("Дійсний: %s, #%d" % ({1: "Standard", 2: "Pro"}.get(tier, "?"), serial))
        return
    if len(a) != 2 or a[0] not in TIERS:
        sys.exit(__doc__)
    for _ in range(int(a[1])):
        print(make_key(priv, TIERS[a[0]], secrets.randbits(32)))


if __name__ == "__main__":
    main()
