#!/usr/bin/env python3
"""Firmware update for the bridge over the server session (#24).

Sends an application image (build/tado-bridge.bin) to a bridge as CoAP
Block1 blocks of 1024 bytes, POST b/fw, through tanoclo's command API,
POST /api/send-raw (tanoclo/ws-server/lib/command-api.js:517-537), which
puts a raw WebSocket bridge frame on the bridge's connection. The firmware
side: src/fwupd.h, src/session.c; the format: docs/design.md
"Implementation: self-update, application side (#24)".

  block 0       the 64-byte slot header (src/fwimg.h: magic TFW1, version,
                length, size, fw_version, load address, SHA-256, CRC-32),
                then zeros to 1024 bytes
  block n >= 1  the image at (n - 1) x 1024; the last one shorter

The bridge answers each block once it is in flash: 2.31 Continue, and 2.04
Changed for the last one (verified, previous image saved, boot state
PENDING); then it restarts and the bootloader installs the image. GET b/fw
answers a status line (running image id, boot state, transfer).

Seeing the answers needs tanoclo to hand the bridge's ACK back to the HTTP
caller. Unchanged tanoclo does not: send-raw registers no Message ID, and
the bridge's ACK ends in handleAckReceived() with nobody waiting
(lib/coap-transport.js:241-300). The additive change asked for in #38 (an
optional "waitAck" on send-raw, docs/design.md "Uploader and tanoclo")
makes the answer the HTTP response. Without it, --blind sends at a fixed
pace and can only tell a restart from GET /api/clients; a lost or early
block makes the bridge refuse the rest, and nothing is marked unless the
whole image arrived and matched its SHA-256.

The command API answers only from localhost (api/server.js:320-345); from
the bench host use --docker CONTAINER, which runs every request inside the
tanoclo container (docker exec ... node), or run this tool there.

Commands:
  info IMAGE                     check the image, print its id and header
  body IMAGE OUT                 write the transfer body (all blocks) to OUT
  frames IMAGE OUT --ipv6 ADDR   write every WebSocket frame, each with a
                                 2-byte big-endian length, to OUT (tests)
  status [--bridge SERIAL]       GET b/fw
  upload IMAGE [--bridge SERIAL] [--blind] [--watch]
"""
import argparse
import hashlib
import ipaddress
import json
import secrets
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zlib

# src/fwimg.h
APP_BASE = 0x08010000
APP_MAX = 448 * 1024
RAM_BASE, RAM_SIZE = 0x20000000, 128 * 1024
DESC_OFFSET, DESC_MAGIC, DESC_VERSION, DESC_LEN = 0x200, 0x31445442, 1, 32
HEAD_LEN = DESC_OFFSET + DESC_LEN
HDR_MAGIC, HDR_VERSION, HDR_LEN = 0x31574654, 1, 64
ID_LEN = 16
# src/fwupd.h
BLOCK, SZX = 1024, 6
# CoAP (RFC 7252, RFC 7959)
CON, ACK = 0, 2
POST, GET = 0x02, 0x01
OPT_URI_PATH, OPT_BLOCK1 = 11, 27
# Message IDs below tanoclo's own range (0x7000-0xFFFF, lib/coap-transport.js
# getNextMid), so an ACK of ours never cancels one of its retries.
MID_LO, MID_HI = 0x0100, 0x6FFF


class Fail(Exception):
    pass


# --- Image -------------------------------------------------------------------
def check_image(img):
    """fwimg_app_check() of src/fwimg.c, on the whole file."""
    n = len(img)
    if n <= HEAD_LEN or n > APP_MAX or n % 4:
        raise Fail(f"image size {n}: must be {HEAD_LEN + 4}..{APP_MAX} and a multiple of 4")
    magic, version, dlen, end, fw = struct.unpack_from("<IHHII", img, DESC_OFFSET)
    if magic != DESC_MAGIC or version != DESC_VERSION or dlen != DESC_LEN:
        raise Fail("no BTD1 descriptor at 0x200: not an application image for 0x08010000")
    if end != APP_BASE + n:
        raise Fail(f"descriptor image_end {end:#x} does not match the file ({APP_BASE + n:#x})")
    sp, pc = struct.unpack_from("<II", img, 0)
    if not (RAM_BASE < sp <= RAM_BASE + RAM_SIZE) or sp % 4:
        raise Fail(f"initial stack pointer {sp:#x} not in RAM")
    if not pc & 1 or (pc & ~1) < APP_BASE + HEAD_LEN or (pc & ~1) >= end:
        raise Fail(f"reset vector {pc:#x} not in the image")
    return fw


def header(img, fw_version):
    """fwimg_hdr_build() of src/fwimg.c."""
    sha = hashlib.sha256(img).digest()
    raw = struct.pack("<IHHIII", HDR_MAGIC, HDR_VERSION, HDR_LEN, len(img), fw_version, APP_BASE)
    raw += sha + bytes(8)
    return raw + struct.pack("<I", zlib.crc32(raw) & 0xFFFFFFFF), sha


def blocks(img):
    fw = check_image(img)
    hdr, sha = header(img, fw)
    out = [hdr + bytes(BLOCK - len(hdr))]
    out += [img[i:i + BLOCK] for i in range(0, len(img), BLOCK)]
    return out, sha


# --- CoAP and the bridge frame -----------------------------------------------
def opt_nibble(v):
    if v < 13:
        return v, b""
    if v < 269:
        return 13, bytes([v - 13])
    return 14, struct.pack(">H", v - 269)


def coap(mtype, code, mid, token, options, payload=b""):
    out = bytes([0x40 | mtype << 4 | len(token), code]) + struct.pack(">H", mid) + token
    last = 0
    for num, val in options:  # in ascending order
        d, de = opt_nibble(num - last)
        ln, le = opt_nibble(len(val))
        out += bytes([d << 4 | ln]) + de + le + val
        last = num
    if payload:
        out += b"\xff" + payload
    return out


def block1_value(num, more, szx=SZX):
    v = num << 4 | (8 if more else 0) | szx
    n = 0 if v == 0 else 1 if v < 0x100 else 2 if v < 0x10000 else 3
    return v.to_bytes(n, "big") if n else b""


def frame(ipv6, coap_bytes):
    """The 28-byte bridge frame (tanoclo/ws-server/lib/ws-bridge.js:9-15),
    server to bridge, addressed to the bridge itself."""
    return (struct.pack(">HB", 0x0002, 16) + ipv6 + struct.pack(">HBHHH", 0, 0, 5683, 0, len(coap_bytes))
            + coap_bytes)


def parse_ack(code_str, payload_hex):
    return code_str, bytes.fromhex(payload_hex) if payload_hex else b""


# --- tanoclo command API ------------------------------------------------------
NODE_RELAY = r"""
const http = require('http'); const rl = require('readline').createInterface({input: process.stdin});
rl.on('line', (line) => {
  const r = JSON.parse(line);
  const req = http.request({host: '127.0.0.1', port: r.port, path: r.path, method: r.method,
    headers: {'Content-Type': 'application/json'}}, (res) => {
      let b = ''; res.on('data', (c) => b += c);
      res.on('end', () => process.stdout.write(JSON.stringify({status: res.statusCode, body: b}) + '\n'));
  });
  req.on('error', (e) => process.stdout.write(JSON.stringify({status: 0, body: String(e)}) + '\n'));
  if (r.body) req.write(r.body); req.end();
});
"""


class Api:
    def __init__(self, args):
        self.url = args.api.rstrip("/")
        self.proc = None
        if args.docker:
            port = int(self.url.rsplit(":", 1)[1]) if self.url.count(":") >= 2 else 3111
            self.port = port
            self.proc = subprocess.Popen(["docker", "exec", "-i", args.docker, "node", "-e", NODE_RELAY],
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def call(self, method, path, body=None, timeout=60):
        data = json.dumps(body) if body is not None else None
        if self.proc:
            self.proc.stdin.write(json.dumps({"method": method, "path": path, "port": self.port,
                                              "body": data}) + "\n")
            self.proc.stdin.flush()
            line = self.proc.stdout.readline()
            if not line:
                raise Fail("the relay in the container stopped (docker exec)")
            r = json.loads(line)
            status, text = r["status"], r["body"]
        else:
            req = urllib.request.Request(self.url + path, method=method,
                                         data=data.encode() if data else None,
                                         headers={"Content-Type": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=timeout) as res:
                    status, text = res.status, res.read().decode()
            except urllib.error.HTTPError as e:
                status, text = e.code, e.read().decode()
            except (urllib.error.URLError, OSError) as e:
                raise Fail(f"{self.url}{path}: {e}") from e
        try:
            j = json.loads(text)
        except ValueError:
            j = {"raw": text}
        if status == 403:
            raise Fail("tanoclo's command API answers localhost only: use --docker CONTAINER")
        return status, j

    def bridge(self, serial):
        status, j = self.call("GET", "/api/clients")
        if status != 200:
            raise Fail(f"GET /api/clients: {status} {j}")
        cl = [c for c in j.get("clients", []) if (c.get("deviceId") == serial if serial
                                                  else str(c.get("deviceId", "")).startswith("IB"))]
        if len(cl) != 1:
            raise Fail(f"{len(cl)} matching bridges connected; name one with --bridge"
                       if not serial else f"bridge {serial} not connected")
        c = cl[0]
        return c["deviceId"], ipaddress.IPv6Address(c["ipv6"]).packed, c.get("connectedAt")


class Link:
    """One request at a time to the bridge, through send-raw."""

    def __init__(self, api, serial, blind):
        self.api = api
        self.serial, self.ipv6, self.connected_at = api.bridge(serial)
        self.blind = blind
        self.mid = MID_LO + secrets.randbelow(MID_HI - MID_LO)

    def next_mid(self):
        self.mid = MID_LO if self.mid >= MID_HI else self.mid + 1
        return self.mid

    def request(self, code, options, payload=b"", timeout_s=35, tries=5):
        """Send a CON request; with acks, return (code, payload) of the
        bridge's answer, resending the same message on a timeout."""
        mid, token = self.next_mid(), secrets.token_bytes(4)
        ws = frame(self.ipv6, coap(CON, code, mid, token, options, payload)).hex()
        body = {"deviceId": self.serial, "wsHex": ws}
        if not self.blind:
            body.update({"waitAck": True, "timeoutMs": int(timeout_s * 1000)})
        for attempt in range(tries):
            status, j = self.api.call("POST", "/api/send-raw", body, timeout=timeout_s + 10)
            if self.blind:
                if status != 200:
                    raise Fail(f"send-raw: {status} {j}")
                return None, b""
            if status == 200 and "ack" not in j:
                raise Fail("tanoclo did not wait for the bridge's answer: its send-raw has no "
                           "'waitAck' yet (#38). Use --blind, or add the handler (docs/design.md)")
            if status == 200:
                a = j["ack"]
                return parse_ack(a.get("code"), a.get("payloadHex"))
            if status in (504, 500) and "Timeout" in str(j):
                print(f"  mid {mid:#06x}: no answer in {timeout_s} s, sending again ({attempt + 1})")
                continue
            raise Fail(f"send-raw: {status} {j}")
        raise Fail(f"no answer after {tries} tries")


def fw_options(num=None, more=None):
    o = [(OPT_URI_PATH, b"b"), (OPT_URI_PATH, b"fw")]
    if num is not None:
        o.append((OPT_BLOCK1, block1_value(num, more)))
    return o


def status(link):
    code, p = link.request(GET, fw_options(), timeout_s=10)
    if code not in ("2.05", "2.5"):
        raise Fail(f"GET b/fw: {code} {p!r}")
    return p.decode(errors="replace")


# --- Commands -------------------------------------------------------------------
def cmd_info(a):
    img = open(a.image, "rb").read()
    bl, sha = blocks(img)
    print(f"image {a.image}: {len(img)} bytes, {len(bl) - 1} image blocks + header block")
    print(f"id {sha[:ID_LEN].hex()} (SHA-256 {sha.hex()})")
    print(f"header {bl[0][:HDR_LEN].hex()}")


def cmd_body(a):
    bl, _ = blocks(open(a.image, "rb").read())
    open(a.out, "wb").write(b"".join(bl))


def cmd_frames(a):
    bl, _ = blocks(open(a.image, "rb").read())
    ipv6 = ipaddress.IPv6Address(a.ipv6).packed
    out = b""
    for n, b in enumerate(bl):
        f = frame(ipv6, coap(CON, POST, MID_LO + n, bytes([n & 0xFF, 1, 2, 3]),
                             fw_options(n, n < len(bl) - 1), b))
        out += struct.pack(">H", len(f)) + f
    open(a.out, "wb").write(out)


def cmd_status(a):
    link = Link(Api(a), a.bridge, False)
    print(status(link))


def wait_restart(api, link, timeout_s):
    t0 = time.time()
    seen_gone = False
    while time.time() - t0 < timeout_s:
        time.sleep(3)
        try:
            serial, ipv6, at = api.bridge(link.serial)
        except Fail:
            seen_gone = True
            continue
        if seen_gone or at != link.connected_at:
            link.ipv6, link.connected_at = ipv6, at
            return True
    return False


def cmd_upload(a):
    img = open(a.image, "rb").read()
    bl, sha = blocks(img)
    api = Api(a)
    link = Link(api, a.bridge, a.blind)
    new_id = sha[:ID_LEN].hex()
    print(f"bridge {link.serial}; image {len(img)} bytes, id {new_id}, {len(bl)} blocks")
    if not a.blind:
        print(f"status before: {status(link)}")
    t0 = time.time()
    for n, b in enumerate(bl):
        last = n == len(bl) - 1
        # Block 0 may first save the running image (448 KB at most: about
        # 100 s at the datasheet maxima); the last one hashes both slots.
        timeout = 180 if n == 0 or last else 35
        while True:
            code, p = link.request(POST, fw_options(n, not last), b, timeout_s=timeout)
            if code != "5.03":
                break
            time.sleep(1)  # the bridge is still writing the block before
        if a.blind:
            time.sleep(a.pace0 if n == 0 else a.pace)
            if n % 32 == 0 or last:
                print(f"  block {n}/{len(bl) - 1} sent (blind)")
            continue
        want = "2.04" if last else "2.31"
        if code != want:
            raise Fail(f"block {n}: {code} {p.decode(errors='replace')}")
        if n == 0 or n % 32 == 0 or last:
            print(f"  block {n}/{len(bl) - 1}: {code} ({time.time() - t0:.1f} s)"
                  + (f" {p.decode(errors='replace')}" if p else ""))
    print(f"sent in {time.time() - t0:.1f} s; waiting for the restart")
    if not wait_restart(api, link, 180):
        raise Fail("the bridge did not reconnect within 180 s")
    print(f"bridge reconnected ({link.connected_at})")
    if a.blind:
        print("blind mode: no status available; check the RTT log ('update:' lines)")
        return
    st = status(link)
    print(f"status after: {st}")
    if not a.watch:
        return
    # Under test: confirmed after 60 s healthy (FWUPD_CONFIRM_MS), or rolled
    # back after 3 restarts without a confirmation (10 min each at most).
    t1 = time.time()
    while time.time() - t1 < a.watch_s:
        time.sleep(10)
        try:
            link.serial, link.ipv6, link.connected_at = api.bridge(link.serial)
            st = status(link)
        except Fail as e:
            print(f"  {time.time() - t1:5.0f} s: {e}")
            continue
        print(f"  {time.time() - t1:5.0f} s: {st}")
        if " boot confirmed " in st and st.startswith(f"run {new_id}"):
            print("confirmed: the new image runs")
            return
        if " boot rolled_back " in st or " boot rejected " in st or " boot no_rollback " in st:
            raise Fail(f"not installed: {st}")
    raise Fail("no confirmation within the watch time")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--api", default="http://127.0.0.1:3111", help="tanoclo's command API")
    ap.add_argument("--docker", metavar="CONTAINER",
                    help="run the API requests inside this container (tanoclo-ws on the bench)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("image")
    p = sub.add_parser("body")
    p.add_argument("image")
    p.add_argument("out")
    p = sub.add_parser("frames")
    p.add_argument("image")
    p.add_argument("out")
    p.add_argument("--ipv6", required=True)
    p = sub.add_parser("status")
    p.add_argument("--bridge")
    p = sub.add_parser("upload")
    p.add_argument("image")
    p.add_argument("--bridge")
    p.add_argument("--blind", action="store_true",
                   help="tanoclo without waitAck (#38): fixed pace, no answers seen")
    p.add_argument("--pace", type=float, default=1.5, help="blind: seconds per block")
    p.add_argument("--pace0", type=float, default=120.0, help="blind: seconds after block 0")
    p.add_argument("--watch", action="store_true", help="follow the test until CONFIRMED")
    p.add_argument("--watch-s", type=float, default=2400.0)
    a = ap.parse_args()
    try:
        {"info": cmd_info, "body": cmd_body, "frames": cmd_frames, "status": cmd_status,
         "upload": cmd_upload}[a.cmd](a)
    except Fail as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
