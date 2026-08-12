#!/usr/bin/env python3
"""Dice!Next 插件签名工具（可选安全功能）。

用途：
  1. 生成 RSA 密钥对（公钥填入 系统设置 → 插件签名公钥）：
       python tools/sign-plugin.py genkey --out plugin-sign-key.pem
  2. 为插件文件（JS 单文件 / Lua mod zip）生成签名：
       python tools/sign-plugin.py sign plugin.zip --key plugin-sign-key.pem --out signature.json
  3. 上传插件时，把 signature.json 里的 base64 放进请求体 "signature" 字段。

依赖：pip install cryptography
"""

import argparse
import base64
import json
import sys


def _load_crypto():
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import padding, rsa
        from cryptography.hazmat.primitives.serialization import load_pem_private_key
        return hashes, padding, rsa, serialization, load_pem_private_key
    except ImportError:
        print("需要 cryptography 库：pip install cryptography", file=sys.stderr)
        sys.exit(1)


def cmd_genkey(args):
    hashes, padding, rsa, serialization, _ = _load_crypto()
    key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    with open(args.out, "wb") as f:
        f.write(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        ))
    pub = key.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    with open(args.out + ".pub", "wb") as f:
        f.write(pub)
    print(f"已生成私钥 {args.out} 与公钥 {args.out}.pub")
    print("把公钥内容填入 系统设置 → 插件签名公钥。私钥请妥善保管（勿上传）。")


def cmd_sign(args):
    hashes, padding, rsa, serialization, load_pem_private_key = _load_crypto()
    with open(args.key, "rb") as f:
        key = load_pem_private_key(f.read(), password=None)
    with open(args.file, "rb") as f:
        data = f.read()
    signature = key.sign(data, padding.PKCS1v15(), hashes.SHA256())
    payload = {"algorithm": "rsa-sha256", "signature": base64.b64encode(signature).decode()}
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    print(f"签名已写入 {args.out}")
    print(f"上传时请求体加：\"signature\": \"{payload['signature']}\"")


def main():
    ap = argparse.ArgumentParser(description="Dice!Next 插件签名工具")
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("genkey", help="生成 RSA 密钥对")
    g.add_argument("--out", default="plugin-sign-key.pem")
    g.set_defaults(func=cmd_genkey)
    s = sub.add_parser("sign", help="为插件文件生成 RSA-SHA256 签名")
    s.add_argument("file")
    s.add_argument("--key", default="plugin-sign-key.pem")
    s.add_argument("--out", default="signature.json")
    s.set_defaults(func=cmd_sign)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
