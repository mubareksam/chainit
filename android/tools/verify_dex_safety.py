#!/usr/bin/env python3
"""
ChainIt Android dex-safety verifier.

Why this exists
---------------
Kotlin >= 2.2 compiles lambdas and SAM conversions to `invokedynamic`
(`CONSTANT_InvokeDynamic` + `LambdaMetafactory`) by default. The `dx` dexer that this project uses
(no Gradle / no Android Studio in the build environment) cannot encode `InvokeCustom` in the DEX
v038 container required by minSdk 26, so any app class compiled that way dies the instant ART
touches it — the user-visible symptom is "ChainIt Sibling keeps stopping" on every launch.

`android/build-apk.sh` therefore compiles with `-Xlambdas=class -Xsam-conversions=class`, and this
script *proves* it worked by walking real class-file constant pools instead of grepping strings:

  * HARD FAIL if any first-party class contains `CONSTANT_InvokeDynamic`.
  * REPORT the prebuilt kotlin-stdlib methods that do use invokedynamic. dx lowers those into a
    runtime `java.lang.invoke.LambdaMetafactory` call, which resolves fine on API 26+ (our minSdk),
    but first-party code should still prefer plain calls over those exact methods.

Finally it cross-checks the first-party *sources* against that report, because a handful of very
convenient stdlib functions (`File.copyRecursively`, `compareBy { }`, `File.readLines`, `windowed`,
`withIndex`, `Regex.findAll`, ...) are on the emulated list. ChainIt replaces those with the plain
loops in `com.chainit.android.vfs.FileOps`, and this check keeps it that way.

Usage:
    verify_dex_safety.py <app-classes-dir> <runtime-libs.jar> <report.txt> [source-dir]
"""

import os
import struct
import sys

# Total instruction length per opcode (including the opcode byte). -1 marks variable-length.
OPLEN = {}
for op in range(0x00, 0xCA):
    OPLEN[op] = 1
for op in (0x10, 0x12, 0x15, 0x16, 0x17, 0x18, 0x19, 0x36, 0x37, 0x38, 0x39, 0x3A,
           0x84, 0xA9, 0xBC):
    OPLEN[op] = 2
for op in (0x11, 0x13, 0x14, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1,
           0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
           0xB7, 0xB8, 0xBB, 0xBD, 0xC0, 0xC1, 0xC6, 0xC7):
    OPLEN[op] = 3
for op in (0xB9, 0xBA, 0xC5, 0xC8, 0xC9):
    OPLEN[op] = 5 if op != 0xC5 else 4
OPLEN[0xAA] = -1  # tableswitch
OPLEN[0xAB] = -1  # lookupswitch
OPLEN[0xC4] = -1  # wide


class ClassFile:
    def __init__(self, data: bytes):
        self.d = data
        self.ok = data[:4] == b"\xca\xfe\xba\xbe"
        self.cp = {}
        self.has_indy = False
        self.indy_methods = []
        if self.ok:
            self._parse()

    def _u2(self, o):
        return struct.unpack_from(">H", self.d, o)[0]

    def _u4(self, o):
        return struct.unpack_from(">I", self.d, o)[0]

    def _parse(self):
        d = self.d
        count = self._u2(8)
        i, idx = 10, 1
        while idx < count:
            tag = d[i]
            if tag == 1:  # Utf8
                ln = self._u2(i + 1)
                self.cp[idx] = d[i + 3:i + 3 + ln].decode("utf-8", "replace")
                i += 3 + ln
            elif tag in (7, 8, 16, 19, 20):  # Class/String/MethodType/Module/Package
                self.cp[idx] = ("ref", self._u2(i + 1))
                i += 3
            elif tag == 15:  # MethodHandle
                self.cp[idx] = ("mh", d[i + 1], self._u2(i + 2))
                i += 4
            elif tag == 18:  # InvokeDynamic
                self.cp[idx] = ("indy", self._u2(i + 1), self._u2(i + 3))
                self.has_indy = True
                i += 5
            elif tag in (9, 10, 11):  # Field/Method/InterfaceMethod ref
                self.cp[idx] = ("member", self._u2(i + 1), self._u2(i + 3))
                i += 5
            elif tag == 12:  # NameAndType
                self.cp[idx] = ("nat", self._u2(i + 1), self._u2(i + 3))
                i += 5
            elif tag in (3, 4):  # Integer/Float
                i += 5
            elif tag in (5, 6):  # Long/Double take two slots
                i += 9
                idx += 1
            else:
                self.ok = False
                return
            idx += 1

        i += 6  # access_flags, this_class, super_class
        i += 2 + 2 * self._u2(i)  # interfaces

        i = self._skip_members(i)  # fields
        methods_start = i
        i = self._scan_methods(i)  # methods (records invokedynamic users)
        self.methods_offset = methods_start

    def _skip_members(self, i):
        n = self._u2(i)
        i += 2
        for _ in range(n):
            # access_flags, name_index, descriptor_index — _skip_attrs reads attributes_count itself
            i += 6
            i = self._skip_attrs(i)
        return i

    def _scan_methods(self, i):
        n = self._u2(i)
        i += 2
        for _ in range(n):
            name_idx, desc_idx = self._u2(i + 2), self._u2(i + 4)
            i += 6
            i, code = self._scan_attrs(i, want_code=True)
            if code and self._code_has_indy(code):
                self.indy_methods.append("%s%s" % (self.utf(name_idx), self.utf(desc_idx)))
        return i

    def _skip_attrs(self, i):
        n = self._u2(i)
        i += 2
        for _ in range(n):
            ln = self._u4(i + 2)
            i += 6 + ln
        return i

    def _scan_attrs(self, i, want_code=False):
        n = self._u2(i)
        i += 2
        code_bytes = None
        for _ in range(n):
            name_idx = self._u2(i)
            ln = self._u4(i + 2)
            body = i + 6
            if want_code and self.utf(name_idx) == "Code":
                code_len = self._u4(body + 4)
                code_bytes = self.d[body + 8: body + 8 + code_len]
            i = body + ln
        return i, code_bytes

    def utf(self, k):
        v = self.cp.get(k)
        return v if isinstance(v, str) else ""

    def _code_has_indy(self, code: bytes) -> bool:
        """Decode the instruction stream; a byte scan would produce false positives."""
        p, n = 0, len(code)
        while p < n:
            op = code[p]
            if op == 0xBA:
                return True
            ln = OPLEN.get(op, 1)
            if ln == -1:
                if op == 0xAA:  # tableswitch
                    pad = (4 - ((p + 1) % 4)) % 4
                    base = p + 1 + pad
                    low = struct.unpack_from(">i", code, base + 4)[0]
                    high = struct.unpack_from(">i", code, base + 8)[0]
                    p = base + 12 + 4 * (high - low + 1)
                elif op == 0xAB:  # lookupswitch
                    pad = (4 - ((p + 1) % 4)) % 4
                    base = p + 1 + pad
                    npairs = struct.unpack_from(">i", code, base + 4)[0]
                    p = base + 8 + 8 * npairs
                elif op == 0xC4:  # wide
                    op2 = code[p + 1]
                    p += 6 if op2 == 0x84 else 4
                else:
                    p += 1
                continue
            p += ln
        return False


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 2
    app_dir, runtime_jar, report_path = sys.argv[1], sys.argv[2], sys.argv[3]

    # 1. First-party classes: zero tolerance.
    offenders, scanned = [], 0
    for root, _, files in os.walk(app_dir):
        for f in files:
            if not f.endswith(".class"):
                continue
            scanned += 1
            cf = ClassFile(open(os.path.join(root, f), "rb").read())
            if cf.ok and cf.has_indy:
                offenders.append(os.path.join(root, f))
    if offenders:
        print("FATAL: %d first-party class(es) contain invokedynamic lambdas:" % len(offenders),
              file=sys.stderr)
        for o in sorted(offenders):
            print("   " + o, file=sys.stderr)
        print("This APK would crash at launch. Rebuild with -Xlambdas=class -Xsam-conversions=class.",
              file=sys.stderr)
        return 1
    print("    OK  %d first-party classes verified invokedynamic-free" % scanned)

    # 2. Prebuilt runtime jars: report (dx emulates these via LambdaMetafactory on API 26+).
    import zipfile
    lines = []
    classes_with_indy = 0
    if os.path.exists(runtime_jar):
        with zipfile.ZipFile(runtime_jar) as z:
            for name in sorted(z.namelist()):
                if not name.endswith(".class"):
                    continue
                cf = ClassFile(z.read(name))
                if cf.ok and cf.has_indy:
                    classes_with_indy += 1
                    for m in cf.indy_methods:
                        lines.append("%s.%s" % (name[:-6].replace("/", "."), m))
    with open(report_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + ("\n" if lines else ""))
    print("    OK  runtime jars: %d classes / %d methods use invokedynamic "
          "(dx lowers these to a LambdaMetafactory call, available on API 26+) -> %s"
          % (classes_with_indy, len(lines), report_path))

    # 3. Cross-check first-party sources against the emulated list.
    if len(sys.argv) > 4 and os.path.isdir(sys.argv[4]):
        risky = sorted({m.rsplit(".", 1)[-1].split("(")[0] for m in lines
                        if "$" not in m and not m.endswith("<clinit>") and not m.endswith("<init>")})
        # Overloads of these names exist in java.lang / plain Kotlin and are perfectly safe
        # (e.g. Int.toString(), Editable.toString()); only flag the collection/file ones.
        skip = {"toString", "plus", "iterator", "asString", "writeReplace", "computeIfAbsent"}
        risky = [r for r in risky if r not in skip]
        findings = []
        for root, _, files in os.walk(sys.argv[4]):
            for f in files:
                if not f.endswith(".kt"):
                    continue
                path = os.path.join(root, f)
                text = open(path, encoding="utf-8", errors="replace").read()
                for lineno, line in enumerate(text.splitlines(), 1):
                    stripped = line.strip()
                    if stripped.startswith("*") or stripped.startswith("//"):
                        continue
                    for name in risky:
                        if ("." + name + "(") in line or (name + "<") in line:
                            findings.append("%s:%d  %s()  |  %s" % (path, lineno, name, stripped[:90]))
        if findings:
            print("    WARN  first-party code touches %d invokedynamic-emulated stdlib call(s):"
                  % len(findings))
            for f in findings:
                print("          " + f)
            print("          Prefer com.chainit.android.vfs.FileOps equivalents where possible.")
        else:
            print("    OK  first-party sources avoid every invokedynamic-emulated stdlib method")
    return 0


if __name__ == "__main__":
    sys.exit(main())
