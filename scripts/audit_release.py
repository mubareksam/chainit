#!/usr/bin/env python3
"""Audit the binaries in releases/ - no SDK, no jars, no network.

Why this exists
---------------
ChainIt ships three artifacts and every claim made about them in README.md used to be
re-checked with tools that live outside the repository: `apksigner.jar` for the APK
signature, `dx`/`kotlinc` output for the invokedynamic guarantee, the compiled `.class`
tree for `verify_dex_safety.py`, a Windows SDK for the version resource. Those tools are
provisioned into /tmp, and /tmp is wiped regularly - which silently turns "verified" into
"not verified" rather than into a failure.

This script parses the artifacts themselves, so the audit is reproducible from a clean
checkout with nothing but python3:

  * releases/version.txt agrees with the bytes on disk (size + SHA-256 of all three).
  * the exe and the packed installer carry the right numeric VERSIONINFO, and the
    installer's *appended payload* is the very exe that is published next to it.
  * no browser can be launched by the Windows binary (no `msedge`, no `--app=`).
  * the APK carries APK Signature Scheme v2 **and** v3 blocks, and both are verified
    properly rather than merely spotted: the signer's certificate is the pinned release key
    (the guarantee that an upgrade installs in place instead of failing with
    INSTALL_FAILED_UPDATE_INCOMPATIBLE), the chunked content digest recomputed from the file
    still matches what the signer committed to, and the RSASSA-PKCS1-v1_5 signature over
    `signedData` verifies against the public key embedded in the block. So "signed by us"
    and "unchanged since signing" are both proved, in pure python, without apksigner.
  * the binary AndroidManifest.xml really says the published versionName/versionCode
    (read out of the AXML chunk, not out of the source file).
  * no first-party class in classes*.dex contains `invoke-custom` / `invoke-polymorphic`.
    The scan is per code item, walking real instruction boundaries, because an operand word
    is not an opcode and a byte grep reports phantom crashes. Sites inside prebuilt
    kotlin-stdlib classes are counted and reported but not failed: `dx` lowers those to a
    runtime LambdaMetafactory call that ART resolves from API 26, which is our minSdk - that
    is exactly the distinction `android/tools/verify_dex_safety.py` draws, and it is why
    first-party code is compiled with -Xlambdas=class -Xsam-conversions=class.
  * the classes the phone's native PC browser is made of are present in the dex.

Nothing here trusts the repository's own claims: every value is parsed back out of the
published bytes. `--releases-dir` points the audit at a copy, which is how the checks were
proved to fail when they should - a flipped byte in a ZIP entry, in the central directory,
inside `signedData`, or inside the certificate is each caught by the corresponding check.

Usage:  python3 scripts/audit_release.py [--version 1.7.0] [--code 8] [--releases-dir DIR]
        (defaults are read from releases/version.txt)

Exits 0 only when every check passes.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import struct
import sys
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
RELEASES = REPO / "releases"  # the default; --releases-dir audits a copy instead

# The certificate ChainIt has signed every APK with since the first release. Changing it
# would break in-place upgrades for every installed phone, so it is pinned here and the
# keystore (android/keystore/chainit-release.p12) must never be regenerated.
EXPECTED_CERT_SHA256 = "9a57db1b6d0538d7ccfb93fb317a528411e19e12ead41a28826f440caabd46d8"

# Classes the phone's native PC browser is made of. If one of these is missing from the
# dex the app was built from the wrong sources - the same regression §9.6 guards against.
REQUIRED_CLASSES = (
    "Lcom/chainit/android/ui/PcBrowserActivity;",
    "Lcom/chainit/android/net/PcVfsClient;",
    "Lcom/chainit/android/net/ChainItFileProvider;",
    "Lcom/chainit/android/net/BatteryTelemetry;",
    "Lcom/chainit/android/service/SiblingDaemonService;",
    "Lcom/chainit/android/net/PhoneHttpAndWebDavServer;",
)

FIRST_PARTY_PREFIX = "Lcom/chainit/android/"

# DEX opcodes that `dx` cannot encode and that kill the app the moment ART touches them.
BAD_OPCODES = {0xFA: "invoke-polymorphic", 0xFB: "invoke-custom", 0xFC: "invoke-custom/range"}

_VS_FIXEDFILEINFO_SIG = b"\xbd\x04\xef\xfe"
_APK_SIG_BLOCK_MAGIC = b"APK Sig Block 42"
_SIG_V2_ID = 0x7109871A
_SIG_V3_ID = 0xF05368C0

failures: list[str] = []
notes: list[str] = []


def check(ok: bool, label: str, detail: str = "") -> bool:
    """Record one pass/fail line. Never raises, so one bad artifact does not hide the rest."""
    if ok:
        print(f"  OK   {label}" + (f"  ({detail})" if detail else ""))
    else:
        print(f"  FAIL {label}" + (f"  ({detail})" if detail else ""))
        failures.append(label)
    return ok


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_manifest(releases: pathlib.Path) -> dict[str, str]:
    manifest = releases / "version.txt"
    if not manifest.exists():
        print(f"ERROR: {manifest} does not exist - run scripts/publish_release_manifest.py", file=sys.stderr)
        raise SystemExit(1)
    out: dict[str, str] = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        out[key.strip()] = value.strip()
    return out


# --------------------------------------------------------------------------- PE (Windows)


def pe_versions(data: bytes) -> list[tuple[str, str]]:
    """Every (fileVersion, productVersion) pair compiled into a PE's VERSIONINFO."""
    found: list[tuple[str, str]] = []
    idx = data.find(_VS_FIXEDFILEINFO_SIG)
    while idx != -1 and idx + 24 <= len(data):
        file_ms, file_ls, prod_ms, prod_ls = struct.unpack_from("<IIII", data, idx + 8)
        fmt = lambda ms, ls: f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"  # noqa: E731
        found.append((fmt(file_ms, file_ls), fmt(prod_ms, prod_ls)))
        idx = data.find(_VS_FIXEDFILEINFO_SIG, idx + 1)
    return found


def audit_exe(path: pathlib.Path, version: str) -> bytes:
    print(f"\n{path.name}")
    data = path.read_bytes()
    want = f"{version}.0"
    versions = pe_versions(data)
    if not check(bool(versions), "VERSIONINFO resource present", f"{len(versions)} found"):
        return data
    bad = [v for v in versions if v != (want, want)]
    check(not bad, f"numeric VERSIONINFO is {want}", ", ".join(f"{f}/{p}" for f, p in versions))
    check(
        version.encode("utf-16-le") in data,
        f'"{version}" version string present',
        "Explorer's Details tab reads this one",
    )
    # The whole point of the native window: ChainIt draws its own UI and never hands the
    # user to a browser. These strings are how that regression would show up in a binary.
    for needle in (b"msedge", b"--app=", b"iexplore", b"chrome.exe"):
        check(data.count(needle) == 0, f"no browser launch string {needle.decode()!r}")
    return data


def audit_installer(path: pathlib.Path, version: str, exe: bytes) -> None:
    print(f"\n{path.name}")
    data = path.read_bytes()
    versions = pe_versions(data)
    # Two VERSIONINFO blocks are expected: the setup stub's own resource, plus the
    # appended ChainIt-Windows.exe payload's.
    check(len(versions) >= 2, "carries stub + payload VERSIONINFO", f"{len(versions)} found")
    bad = [v for v in versions if v != (f"{version}.0", f"{version}.0")]
    check(not bad, f"every VERSIONINFO is {version}.0", ", ".join(f"{f}/{p}" for f, p in versions))

    # The 52-byte trailer: "CHAINIT1" | payload size (u64) | SHA-256 (32 B) | trailer size (u32).
    trailer_size = struct.unpack_from("<I", data, len(data) - 4)[0]
    if not check(trailer_size == 52, "trailer size is 52 bytes", str(trailer_size)):
        return
    trailer = data[len(data) - trailer_size : len(data) - 4]
    magic, payload_size, digest = struct.unpack_from("<8sQ32s", trailer, 0)
    check(magic == b"CHAINIT1", 'trailer magic is "CHAINIT1"', magic.decode(errors="replace"))
    payload = data[len(data) - trailer_size - payload_size : len(data) - trailer_size]
    check(len(payload) == payload_size, "payload length matches the trailer", f"{len(payload):,} B")
    check(
        sha256_bytes(payload) == digest.hex(),
        "payload SHA-256 matches the trailer",
        "the stub verifies this before writing to disk",
    )
    check(payload == exe, "payload is byte-identical to the published exe")


# --------------------------------------------------------------------------- APK signing


def apk_signing_block(data: bytes) -> dict[int, bytes] | None:
    """Parse the APK Signing Block that sits between the last entry and the central directory."""
    eocd = data.rfind(b"PK\x05\x06")
    if eocd == -1:
        return None
    cd_offset = struct.unpack_from("<I", data, eocd + 16)[0]
    if cd_offset == 0xFFFFFFFF:
        notes.append("zip64 APK: central-directory offset is in the zip64 EOCD, not parsed here")
        return None
    if cd_offset < 24 or data[cd_offset - 16 : cd_offset] != _APK_SIG_BLOCK_MAGIC:
        return None
    size_of_block = struct.unpack_from("<Q", data, cd_offset - 24)[0]
    block_start = cd_offset - size_of_block - 8
    if block_start < 0 or struct.unpack_from("<Q", data, block_start)[0] != size_of_block:
        return None
    out: dict[int, bytes] = {}
    pos = block_start + 8
    end = cd_offset - 24  # trailing size-of-block (8) + magic (16)
    while pos + 12 <= end:
        (pair_len,) = struct.unpack_from("<Q", data, pos)
        (pair_id,) = struct.unpack_from("<I", data, pos + 8)
        value = data[pos + 12 : pos + 8 + pair_len]
        out[pair_id] = value
        pos += 8 + pair_len
    return out


def _seq(buf: bytes, pos: int) -> tuple[bytes, int]:
    """Read one uint32-length-prefixed sequence out of an APK signing block."""
    (length,) = struct.unpack_from("<I", buf, pos)
    return buf[pos + 4 : pos + 4 + length], pos + 4 + length


def _asn1(buf: bytes, pos: int) -> tuple[int, bytes, int]:
    """Minimal DER reader: (tag, value, next position)."""
    tag = buf[pos]
    pos += 1
    length = buf[pos]
    pos += 1
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(buf[pos : pos + count], "big")
        pos += count
    return tag, buf[pos : pos + length], pos + length


def rsa_public_key(spki: bytes) -> tuple[int, int]:
    """(modulus, exponent) out of a DER SubjectPublicKeyInfo for an RSA key."""
    _tag, body, _next = _asn1(spki, 0)                     # SEQUENCE
    _tag, _algorithm, after_algorithm = _asn1(body, 0)      # AlgorithmIdentifier
    _tag, bit_string, _next = _asn1(body, after_algorithm)  # BIT STRING
    _tag, rsa_key, _next = _asn1(bit_string[1:], 0)         # drop the unused-bits octet
    pos = 0
    _tag, modulus, pos = _asn1(rsa_key, pos)
    _tag, exponent, pos = _asn1(rsa_key, pos)
    return int.from_bytes(modulus, "big"), int.from_bytes(exponent, "big")


# PKCS#1 v1.5 DigestInfo prefixes for the two RSA digests apksigner uses with v2/v3.
_DIGEST_INFO = {
    "sha256": bytes.fromhex("3031300d060960864801650304020105000420"),
    "sha512": bytes.fromhex("3051300d060960864801650304020305000440"),
}

# Signature algorithm ids from the APK Signature Scheme v2/v3 spec. Only the RSA ones can
# be verified here; EC/DSA are reported rather than silently waved through.
_SIG_ALGORITHMS = {
    0x0103: ("RSA PKCS#1 v1.5 + SHA-256", "sha256"),
    0x0104: ("RSA PKCS#1 v1.5 + SHA-512", "sha512"),
    0x0201: ("ECDSA + SHA-256", None),
    0x0301: ("DSA + SHA-256", None),
}


def rsa_pkcs1_verify(modulus: int, exponent: int, signed: bytes, signature: bytes, hash_name: str) -> bool:
    """Verify an RSASSA-PKCS1-v1_5 signature over `signed` - no crypto library needed."""
    key_len = (modulus.bit_length() + 7) // 8
    if len(signature) != key_len:
        return False
    encoded = pow(int.from_bytes(signature, "big"), exponent, modulus).to_bytes(key_len, "big")
    digest_info = _DIGEST_INFO[hash_name]
    digest = hashlib.new(hash_name, signed).digest()
    padding = key_len - 3 - len(digest_info) - len(digest)
    if padding < 8:  # EMSA-PKCS1-v1_5 requires at least 8 octets of padding
        return False
    return encoded == b"\x00\x01" + b"\xff" * padding + b"\x00" + digest_info + digest


def zip_signed_sections(data: bytes) -> tuple[bytes, bytes, bytes] | None:
    """The three sections an APK Signature Scheme v2/v3 digest covers.

    The APK Signing Block itself is deliberately not one of them - it cannot sign itself.
    The EOCD's "offset of central directory" field is hashed as though it pointed at the
    signing block, which is what the platform verifier does: without a signing block, that
    is where the central directory would start.
    """
    eocd = data.rfind(b"PK\x05\x06")
    if eocd == -1:
        return None
    cd_offset = struct.unpack_from("<I", data, eocd + 16)[0]
    if cd_offset < 24 or data[cd_offset - 16 : cd_offset] != _APK_SIG_BLOCK_MAGIC:
        return None
    size_of_block = struct.unpack_from("<Q", data, cd_offset - 24)[0]
    block_start = cd_offset - size_of_block - 8
    if block_start < 0:
        return None
    eocd_section = bytearray(data[eocd:])
    struct.pack_into("<I", eocd_section, 16, block_start)
    return data[:block_start], data[cd_offset:eocd], bytes(eocd_section)


def apk_content_digest(data: bytes, hash_name: str = "sha256") -> bytes | None:
    """Recompute the chunked digest the signer committed to, from the file's own bytes.

    Sections are cut into 1 MiB chunks; each chunk digests as 0xA5 | uint32 length | bytes,
    and those digests combine as 0x5A | uint32 chunk count | concatenation.
    """
    sections = zip_signed_sections(data)
    if sections is None:
        return None
    chunk_size = 1 << 20
    pieces: list[bytes] = []
    for section in sections:
        for offset in range(0, len(section), chunk_size):
            chunk = section[offset : offset + chunk_size]
            piece = hashlib.new(hash_name)
            piece.update(b"\xa5")
            piece.update(struct.pack("<I", len(chunk)))
            piece.update(chunk)
            pieces.append(piece.digest())
    top = hashlib.new(hash_name)
    top.update(b"\x5a")
    top.update(struct.pack("<I", len(pieces)))
    for piece in pieces:
        top.update(piece)
    return top.digest()


class Signer:
    """One signer of a v2 or v3 signing block, with everything needed to verify it."""

    def __init__(
        self,
        signed_data: bytes,
        digest_algorithm: int,
        expected_digest: bytes,
        signature_algorithm: int,
        signature: bytes,
        public_key: bytes,
        certificate: bytes,
    ) -> None:
        self.signed_data = signed_data
        self.digest_algorithm = digest_algorithm
        self.expected_digest = expected_digest
        self.signature_algorithm = signature_algorithm
        self.signature = signature
        self.public_key = public_key
        self.certificate = certificate


def parse_signer(block: bytes, v3: bool) -> Signer | None:
    """Parse the first signer. v3 inserts minSdk/maxSdk between signedData and signatures."""
    try:
        signers, _ = _seq(block, 0)
        signer, _ = _seq(signers, 0)
        signed_data, pos = _seq(signer, 0)
        if v3:
            pos += 8  # minSdkVersion (u32) + maxSdkVersion (u32)
        signatures, pos = _seq(signer, pos)
        public_key, _ = _seq(signer, pos)

        digests, after_digests = _seq(signed_data, 0)
        certificates, _ = _seq(signed_data, after_digests)
        certificate, _ = _seq(certificates, 0)

        digest_entry, _ = _seq(digests, 0)
        (digest_algorithm,) = struct.unpack_from("<I", digest_entry, 0)
        expected_digest, _ = _seq(digest_entry, 4)

        signature_entry, _ = _seq(signatures, 0)
        (signature_algorithm,) = struct.unpack_from("<I", signature_entry, 0)
        signature, _ = _seq(signature_entry, 4)

        return Signer(
            signed_data, digest_algorithm, expected_digest, signature_algorithm, signature, public_key, certificate
        )
    except (struct.error, IndexError, ValueError):
        return None


def audit_apk_signature(path: pathlib.Path, data: bytes) -> None:
    """Prove the APK is signed by the pinned key AND has not changed since it was signed."""
    print(f"\n{path.name} - signature")
    block = apk_signing_block(data)
    if block is None:
        check(False, "APK Signing Block found", "v1-only APK: this would not install on API 30+")
        return
    check(_SIG_V2_ID in block, "APK Signature Scheme v2 block present")
    check(_SIG_V3_ID in block, "APK Signature Scheme v3 block present")

    for scheme_id, label, v3 in ((_SIG_V2_ID, "v2", False), (_SIG_V3_ID, "v3", True)):
        if scheme_id not in block:
            continue
        signer = parse_signer(block[scheme_id], v3)
        if not check(signer is not None, f"{label}: signer parsed"):
            continue
        assert signer is not None

        cert_digest = sha256_bytes(signer.certificate)
        check(
            cert_digest == EXPECTED_CERT_SHA256,
            f"{label}: signer certificate is the pinned release key",
            f"{cert_digest[:16]}... - upgrades install in place",
        )

        name, hash_name = _SIG_ALGORITHMS.get(signer.digest_algorithm, (f"0x{signer.digest_algorithm:04x}", "sha256"))
        computed = apk_content_digest(data, hash_name)
        check(
            computed is not None and computed == signer.expected_digest,
            f"{label}: file contents still match the signed digest",
            f"{name}; one changed byte anywhere in the entries, the central directory or the "
            f"EOCD breaks this",
        )

        sig_name, sig_hash = _SIG_ALGORITHMS.get(
            signer.signature_algorithm, (f"0x{signer.signature_algorithm:04x}", None)
        )
        if sig_hash is None:
            notes.append(
                f"{label}: signature algorithm {sig_name} is not RSA, so this auditor reports "
                f"the certificate and content digest but cannot verify the signature itself - "
                f"apksigner verify remains the authority for that scheme"
            )
            continue
        try:
            modulus, exponent = rsa_public_key(signer.public_key)
        except (IndexError, ValueError, struct.error) as exc:
            check(False, f"{label}: signer public key parsed", str(exc))
            continue
        check(
            rsa_pkcs1_verify(modulus, exponent, signer.signed_data, signer.signature, sig_hash),
            f"{label}: signature over signedData verifies with the signer's own key",
            f"{sig_name}, {modulus.bit_length()}-bit key",
        )


# --------------------------------------------------------------------------- binary AXML


def _uleb128(buf: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7


def axml_strings(data: bytes) -> list[str]:
    """Read the string pool out of a binary AndroidManifest.xml."""
    if struct.unpack_from("<H", data, 0)[0] != 0x0003:
        return []
    pos = 8
    while pos + 8 <= len(data):
        chunk_type, header_size, chunk_size = struct.unpack_from("<HHI", data, pos)
        if chunk_size < 8 or pos + chunk_size > len(data):
            break
        if chunk_type == 0x0001:  # RES_STRING_POOL_TYPE
            string_count, _style_count, flags, strings_start, _styles_start = struct.unpack_from(
                "<IIIII", data, pos + 8
            )
            offsets = struct.unpack_from(f"<{string_count}I", data, pos + 28)
            utf8 = bool(flags & (1 << 8))
            base = pos + strings_start
            out: list[str] = []
            for off in offsets:
                p = base + off
                if utf8:
                    # u16-len is stored as one or two bytes: char count, then byte count.
                    n = data[p]
                    p += 2 if n & 0x80 else 1
                    m = data[p]
                    p += 2 if m & 0x80 else 1
                    if m & 0x80:
                        m = ((m & 0x7F) << 8) | data[p]
                        p += 1
                    out.append(data[p : p + m].decode("utf-8", "replace"))
                else:
                    (n,) = struct.unpack_from("<H", data, p)
                    p += 2
                    if n & 0x8000:
                        n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", data, p)[0]
                        p += 2
                    out.append(data[p : p + n * 2].decode("utf-16-le", "replace"))
            return out
        pos += chunk_size
    return []


def axml_manifest_version(data: bytes) -> tuple[int | None, str | None]:
    """versionCode / versionName from the AXML start-element attributes of <manifest>."""
    strings = axml_strings(data)
    if not strings:
        return None, None
    # RES_XML_RESOURCE_MAP_TYPE maps string index -> android: resource id.
    res_ids: list[int] = []
    pos = 8
    while pos + 8 <= len(data):
        chunk_type, _header_size, chunk_size = struct.unpack_from("<HHI", data, pos)
        if chunk_size < 8 or pos + chunk_size > len(data):
            break
        if chunk_type == 0x0180:
            count = (chunk_size - 8) // 4
            res_ids = list(struct.unpack_from(f"<{count}I", data, pos + 8))
            break
        pos += chunk_size

    version_code: int | None = None
    version_name: str | None = None
    pos = 8
    while pos + 8 <= len(data):
        chunk_type, _header_size, chunk_size = struct.unpack_from("<HHI", data, pos)
        if chunk_size < 8 or pos + chunk_size > len(data):
            break
        if chunk_type == 0x0102:  # RES_XML_START_ELEMENT_TYPE
            attr_count = struct.unpack_from("<H", data, pos + 28)[0]
            attr_start = pos + 36
            for i in range(attr_count):
                base = attr_start + i * 20
                (name_idx,) = struct.unpack_from("<I", data, base + 4)
                data_type = data[base + 15]
                (value,) = struct.unpack_from("<I", data, base + 16)
                rid = res_ids[name_idx] if name_idx < len(res_ids) else 0
                if rid == 0x0101021B and data_type == 0x10:  # versionCode, INT_DEC
                    version_code = value
                elif rid == 0x0101021C and data_type == 0x03:  # versionName, STRING
                    version_name = strings[value] if value < len(strings) else None
        pos += chunk_size
    return version_code, version_name


# --------------------------------------------------------------------------- DEX


def _build_opcode_widths() -> list[int]:
    """Instruction width in 16-bit code units, indexed by opcode.

    Only the *first* code unit of an instruction holds an opcode; every later unit is an
    operand whose bytes are arbitrary data. Testing every unit's low byte therefore finds
    hundreds of phantom `invoke-custom`s, so the stream has to be walked instruction by
    instruction - which needs this table. Widths are from the Dalvik bytecode reference.
    """
    widths = [1] * 256  # unused/reserved opcodes are never emitted; 1 keeps the walk aligned

    def span(table: list[int], lo: int, hi: int, width: int) -> None:
        for op in range(lo, hi + 1):
            table[op] = width

    # 1 unit: 10x, 12x, 11n, 11x, 10t
    # 0x04 move-wide is format 12x - a long/double register pair still fits in one code
    # unit. Getting this wrong (and 0x05/0x06 with it) slides the whole walk out of
    # alignment, because Kotlin emits move-wide constantly, and then operand words get
    # read as opcodes and phantom invoke-customs appear.
    for op in (0x00, 0x01, 0x04, 0x07, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
               0x12, 0x1D, 0x1E, 0x21, 0x27, 0x28):
        widths[op] = 1
    span(widths, 0x7B, 0x8F, 1)   # unop
    span(widths, 0xB0, 0xCF, 1)   # binop/2addr

    # 2 units: 22x, 21t, 21s, 21h, 21c, 23x, 22b, 22t, 22s, 22c, 22cs
    for op in (0x02, 0x05, 0x08, 0x13, 0x15, 0x16, 0x19, 0x1A, 0x1C, 0x1F, 0x20, 0x22,
               0x23, 0x29, 0xFE, 0xFF):
        widths[op] = 2
    span(widths, 0x2D, 0x31, 2)   # cmp*
    span(widths, 0x32, 0x37, 2)   # if-test
    span(widths, 0x38, 0x3D, 2)   # if-testz
    span(widths, 0x44, 0x51, 2)   # array get/put
    span(widths, 0x52, 0x5F, 2)   # instance field get/put
    span(widths, 0x60, 0x6D, 2)   # static field get/put
    span(widths, 0x90, 0xAF, 2)   # binop
    span(widths, 0xD0, 0xD7, 2)   # binop/lit16
    span(widths, 0xD8, 0xE2, 2)   # binop/lit8

    # 3 units: 32x, 31i, 35c, 35ms, 35mi, 3rc, 31t, 30t
    for op in (0x03, 0x06, 0x09, 0x14, 0x17, 0x1B, 0x24, 0x25, 0x26, 0x2A, 0x2B, 0x2C,
               0xFB, 0xFC):        # 0xFB invoke-custom, 0xFC invoke-custom/range
        widths[op] = 3
    span(widths, 0x6E, 0x78, 3)   # invoke-kind and its /range forms

    # 4 units: 45cc, 4rcc (invoke-polymorphic)
    widths[0xFA] = 4
    widths[0xFD] = 4

    # 5 units: 51l - only const-wide/const-wide-high16's full 64-bit form
    widths[0x18] = 5
    return widths


OPCODE_WIDTHS = _build_opcode_widths()


class DexReport:
    """What one classes.dex says about the invokedynamic guarantee.

    The count is split by owner because the guarantee is not "no invoke-custom anywhere".
    Prebuilt kotlin-stdlib classes legitimately contain it - `dx` lowers those sites into a
    runtime `java.lang.invoke.LambdaMetafactory` call, which ART resolves from API 26 on, and
    26 is this app's minSdk. What must never appear is invoke-custom in *first-party* code:
    Kotlin >= 2.2 compiles lambdas that way by default, `dx` cannot encode them for our DEX
    version, and the result dies the instant ART touches the class ("ChainIt Sibling keeps
    stopping"). That is why the build uses -Xlambdas=class -Xsam-conversions=class, and this
    is what re-proves it from the shipped artifact.
    """

    def __init__(self) -> None:
        self.descriptors: list[str] = []
        self.first_party_bad = 0
        self.library_bad = 0
        self.library_owners: set[str] = set()


def dex_audit(name: str, data: bytes) -> DexReport:
    """Walk one classes.dex: class descriptors plus bad opcodes, attributed to their owner.

    Opcodes are scanned only at real instruction boundaries inside code items - walking
    class_data -> encoded_method -> code_item -> insns - because a raw byte grep finds
    0xFB inside string data, and an operand word is not an opcode either.
    """
    report = DexReport()
    if data[:4] != b"dex\n":
        return report
    (string_ids_size, string_ids_off) = struct.unpack_from("<II", data, 56)
    (type_ids_size, type_ids_off) = struct.unpack_from("<II", data, 64)
    (class_defs_size, class_defs_off) = struct.unpack_from("<II", data, 96)

    def string_at(idx: int) -> str:
        (off,) = struct.unpack_from("<I", data, string_ids_off + idx * 4)
        _len, p = _uleb128(data, off)
        end = data.index(b"\x00", p)
        return data[p:end].decode("utf-8", "replace")

    for i in range(class_defs_size):
        (class_idx,) = struct.unpack_from("<I", data, class_defs_off + i * 32)
        (type_string_idx,) = struct.unpack_from("<I", data, type_ids_off + class_idx * 4)
        descriptor = string_at(type_string_idx)
        report.descriptors.append(descriptor)
        first_party = descriptor.startswith(FIRST_PARTY_PREFIX)
        (class_data_off,) = struct.unpack_from("<I", data, class_defs_off + i * 32 + 24)
        if class_data_off == 0:
            continue
        p = class_data_off
        static_fields, p = _uleb128(data, p)
        instance_fields, p = _uleb128(data, p)
        direct_methods, p = _uleb128(data, p)
        virtual_methods, p = _uleb128(data, p)
        for _ in range(static_fields + instance_fields):
            _, p = _uleb128(data, p)  # field_idx_diff
            _, p = _uleb128(data, p)  # access_flags
        for _ in range(direct_methods + virtual_methods):
            _, p = _uleb128(data, p)  # method_idx_diff
            _, p = _uleb128(data, p)  # access_flags
            code_off, p = _uleb128(data, p)
            if code_off == 0:
                continue
            (insns_size,) = struct.unpack_from("<I", data, code_off + 12)
            words = struct.unpack_from(f"<{insns_size}H", data, code_off + 16)
            unit = 0
            while unit < insns_size:
                word = words[unit]
                # Payload pseudo-instructions live inline in insns and are identified by the
                # whole code unit, not by an opcode: 0x0100 packed-switch, 0x0200
                # sparse-switch, 0x0300 fill-array-data. Walking them as `nop` (their low
                # byte is 0x00) would drop the walk into the middle of their data.
                if word == 0x0100 and unit + 2 <= insns_size:
                    unit += 4 + 2 * words[unit + 1]
                    continue
                if word == 0x0200 and unit + 2 <= insns_size:
                    unit += 2 + 4 * words[unit + 1]
                    continue
                if word == 0x0300 and unit + 4 <= insns_size:
                    element_width = words[unit + 1]
                    (count,) = struct.unpack_from("<I", data, code_off + 16 + (unit + 2) * 2)
                    payload_bytes = count * element_width
                    unit += 4 + (payload_bytes + 1) // 2
                    continue
                opcode = word & 0xFF
                if opcode in BAD_OPCODES:
                    if first_party:
                        report.first_party_bad += 1
                        notes.append(f"{name}: {BAD_OPCODES[opcode]} in first-party {descriptor}")
                    else:
                        report.library_bad += 1
                        report.library_owners.add(descriptor)
                width = OPCODE_WIDTHS[opcode]
                if width < 1:  # a mis-parse would loop forever; bail out loudly instead
                    notes.append(f"{name}: unrecognised opcode 0x{opcode:02x}, walk stopped")
                    break
                unit += width
    assert string_ids_size and type_ids_size  # parsed, and non-zero in any real dex
    return report


def audit_apk_contents(path: pathlib.Path, version: str, code: int) -> None:
    print(f"\n{path.name} - contents")
    with zipfile.ZipFile(path) as zf:
        names = zf.namelist()
        manifest = zf.read("AndroidManifest.xml")
        dexes = {n: zf.read(n) for n in sorted(names) if re.fullmatch(r"classes\d*\.dex", n)}

    version_code, version_name = axml_manifest_version(manifest)
    check(version_name == version, f"binary manifest versionName is {version}", str(version_name))
    check(version_code == code, f"binary manifest versionCode is {code}", str(version_code))
    check("resources.arsc" in names, "resources.arsc present")
    check(any(n.startswith("res/") for n in names), "res/ present")

    descriptors: list[str] = []
    first_party_bad = 0
    library_bad = 0
    library_owners: set[str] = set()
    for name, data in dexes.items():
        report = dex_audit(name, data)
        descriptors += report.descriptors
        first_party_bad += report.first_party_bad
        library_bad += report.library_bad
        library_owners |= report.library_owners
        print(f"       {name}: {len(report.descriptors):,} classes, {len(data):,} B")
    check(bool(dexes), "at least one classes.dex present", f"{len(dexes)} found")

    first_party = sorted(d for d in descriptors if d.startswith(FIRST_PARTY_PREFIX))
    check(len(first_party) > 0, "first-party classes present in the dex", f"{len(first_party)} found")
    missing = [c for c in REQUIRED_CLASSES if c not in descriptors]
    check(not missing, "native PC browser + telemetry classes present", ", ".join(missing) or "all found")

    detail = ", ".join(f"{BAD_OPCODES[o]} (0x{o:02x})" for o in sorted(BAD_OPCODES))
    check(
        first_party_bad == 0,
        f"no {detail} in first-party code items",
        f"{first_party_bad} found - dx cannot encode these and ART aborts on launch"
        if first_party_bad
        else f"{len(first_party)} first-party classes scanned instruction by instruction",
    )
    if library_bad:
        notes.append(
            f"{library_bad} invoke-custom site(s) in {len(library_owners)} prebuilt library "
            f"classes (kotlin stdlib); dx lowers them to a runtime LambdaMetafactory call that "
            f"ART resolves from API 26, which is this app's minSdk - tolerated by design, and "
            f"the reason first-party code is compiled with -Xlambdas=class"
        )


# --------------------------------------------------------------------------- driver


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", help="expected version (default: releases/version.txt)")
    parser.add_argument("--code", type=int, help="expected apk_version_code (default: version.txt)")
    parser.add_argument(
        "--releases-dir",
        type=pathlib.Path,
        default=RELEASES,
        help="directory holding the artifacts (default: releases/) - point it at a copy to "
        "rehearse a broken release without touching the published one",
    )
    args = parser.parse_args()

    releases = args.releases_dir
    manifest = read_manifest(releases)
    version = args.version or manifest.get("version", "")
    code = args.code if args.code is not None else int(manifest.get("apk_version_code", "0"))
    if not version:
        print("ERROR: no version to audit (pass --version or fix releases/version.txt)", file=sys.stderr)
        return 1

    print(f"ChainIt release audit - {version} (apk versionCode {code})")
    print(f"artifacts:  {releases}")

    exe_path = releases / manifest.get("exe", "ChainIt-Windows.exe")
    apk_path = releases / manifest.get("apk", "ChainIt-Android.apk")
    setup_path = releases / manifest.get("setup", f"ChainIt-Setup-{version}.exe")

    print("\nreleases/version.txt vs the bytes on disk")
    for label, path in (("exe", exe_path), ("apk", apk_path), ("setup", setup_path)):
        if not path.exists():
            if label == "setup":
                notes.append(f"{setup_path.name} is absent - installer not published for this version")
                continue
            check(False, f"{label} exists", str(path))
            continue
        data = path.read_bytes()
        want_hash = manifest.get(f"{label}_sha256", "")
        want_bytes = int(manifest.get(f"{label}_bytes", "-1"))
        check(len(data) == want_bytes, f"{path.name} size", f"{len(data):,} B")
        check(sha256_bytes(data) == want_hash, f"{path.name} SHA-256", sha256_bytes(data)[:16] + "...")

    exe_data = audit_exe(exe_path, version) if exe_path.exists() else b""
    if setup_path.exists() and exe_data:
        audit_installer(setup_path, version, exe_data)
    if apk_path.exists():
        apk_data = apk_path.read_bytes()
        audit_apk_signature(apk_path, apk_data)
        audit_apk_contents(apk_path, version, code)

    if notes:
        print("\nnotes")
        for note in notes:
            print(f"  - {note}")

    print("\n" + "=" * 68)
    if failures:
        print(f"AUDIT FAILED - {len(failures)} check(s):")
        for f in failures:
            print(f"  * {f}")
        return 1
    print("AUDIT GREEN - every claim in the release table is true of the shipped bytes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
