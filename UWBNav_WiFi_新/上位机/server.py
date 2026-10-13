"""UWBNav 本地上位机：标准库 HTTP + UDP，不需要第三方 Python 包。"""
import argparse
import copy
import json
import math
import socket
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
# HTTP验证
import base64
import hmac
import os

ROOT = Path(__file__).resolve().parent
LABELS = 'ABCD'
PAIRS = ('AB', 'BC', 'CD', 'DA', 'AC', 'BD')


def crc16(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c


def module_id(value):
    text = str(value).strip().upper()
    if text.startswith('0X'):
        text = text[2:]
    if not text or len(text) > 8 or any(c not in '0123456789ABCDEF' for c in text):
        raise ValueError('模块ID必须为1~FFFFFFFF的十六进制')
    n = int(text, 16)
    if not 1 <= n <= 0xFFFFFFFF:
        raise ValueError('模块ID不能为0')
    return format(n, 'X')


def number(value, name, low=-10000, high=10000):
    try:
        n = float(value)
    except (ValueError, TypeError):
        raise ValueError(name + '必须为数字')
    if not math.isfinite(n) or not low <= n <= high:
        raise ValueError(name + '超出允许范围')
    return n


class FrameParser:
    def __init__(self, records=1):
        self.records = records
        self.buffer = bytearray()
        self.bad_crc = 0

    def feed(self, data):
        self.buffer.extend(data)
        frames = []
        length = 7 + 6 * self.records
        while True:
            start = self.buffer.find(b'\x45\x91')
            if start < 0:
                self.buffer[:] = b'\x45' if self.buffer.endswith(b'\x45') else b''
                break
            del self.buffer[:start]
            if len(self.buffer) < length:
                break
            f = bytes(self.buffer[:length])
            if f[4] > 1 or crc16(f[:-2]) != int.from_bytes(f[-2:], 'big'):
                self.bad_crc += 1
                del self.buffer[0]
                continue
            del self.buffer[:length]
            rows = []
            for i in range(self.records):
                off = 5 + 6 * i
                rows.append({'peer_id': format(int.from_bytes(f[off:off + 4], 'big'), 'X'),
                             'meters': int.from_bytes(f[off + 4:off + 6], 'big') / 100})
            frames.append({'peer_role': 'tag' if f[4] == 0 else 'anchor',
                           'rows': rows, 'hex': f.hex(' ').upper()})
        return frames


def solve_position(anchors, distances, tag_z=0):
    """四距离二维非线性最小二乘。固定标签高度，按真实三维斜距拟合x/y。"""
    a0 = anchors[0]
    d0 = distances[0]
    xx = xy = yy = bx = by = 0.0
    for a, d in zip(anchors[1:], distances[1:]):
        u, v = 2 * (a['x'] - a0['x']), 2 * (a['y'] - a0['y'])
        q = (d0 * d0 - d * d + a['x'] ** 2 + a['y'] ** 2 - a0['x'] ** 2 - a0['y'] ** 2
             + (tag_z - a['z']) ** 2 - (tag_z - a0['z']) ** 2)
        xx += u * u;
        xy += u * v;
        yy += v * v;
        bx += u * q;
        by += v * q
    det = xx * yy - xy * xy
    if det <= max(1e-12, xx * yy * 1e-8):
        raise ValueError('基站近似共线，无法稳定进行二维定位')
    x, y = (bx * yy - by * xy) / det, (by * xx - bx * xy) / det

    def loss(px, py):
        return sum((math.sqrt((px - a['x']) ** 2 + (py - a['y']) ** 2 + (tag_z - a['z']) ** 2) - d) ** 2
                   for a, d in zip(anchors, distances))

    for _ in range(30):
        hxx = hxy = hyy = gx = gy = 0.0
        for a, d in zip(anchors, distances):
            pred = max(1e-9, math.sqrt((x - a['x']) ** 2 + (y - a['y']) ** 2 + (tag_z - a['z']) ** 2))
            jx, jy = (x - a['x']) / pred, (y - a['y']) / pred
            r = pred - d
            hxx += jx * jx;
            hxy += jx * jy;
            hyy += jy * jy;
            gx += jx * r;
            gy += jy * r
        det = hxx * hyy - hxy * hxy
        if det < 1e-12:
            break
        dx, dy = (gx * hyy - gy * hxy) / det, (gy * hxx - gx * hxy) / det
        scale = 1.0
        current = loss(x, y)
        while scale > 1 / 128 and loss(x - scale * dx, y - scale * dy) > current:
            scale /= 2
        x -= scale * dx;
        y -= scale * dy
        if math.hypot(scale * dx, scale * dy) < 1e-6:
            break
    return {'x': x, 'y': y, 'rms': math.sqrt(loss(x, y) / 4)}


def default_config():
    return {'anchors': [dict(label=k, id=str(i + 1), x=x, y=y, z=0)
                        for i, (k, x, y) in enumerate([('A', 0, 20), ('B', 10, 20),
                                                       ('C', 10, 0), ('D', 0, 0)])],
            'pois': [], 'tag_names': {}, 'tag_z': 0, 'stale_seconds': 4,
            'max_span_seconds': 2.5}


def validate_config(c):
    anchors = c.get('anchors', [])
    if len(anchors) != 4 or [a.get('label') for a in anchors] != list(LABELS):
        raise ValueError('需要按A、B、C、D顺序配置四个基站')
    out = default_config()
    out['anchors'] = [dict(label=a['label'], id=module_id(a['id']),
                           x=number(a['x'], 'X'), y=number(a['y'], 'Y'),
                           z=number(a.get('z', 0), 'Z')) for a in anchors]
    if len(set(a['id'] for a in out['anchors'])) != 4:
        raise ValueError('四个基站ID不能重复')
    solve_position(out['anchors'], [1] * 4, 0)  # 检查几何退化
    out['tag_z'] = number(c.get('tag_z', 0), '标签高度')
    out['stale_seconds'] = number(c.get('stale_seconds', 4), '过期秒数', .5, 60)
    out['max_span_seconds'] = number(c.get('max_span_seconds', 2.5), '时间跨度', .1, 60)
    names = c.get('tag_names', {})
    if not isinstance(names, dict) or len(names) > 100:
        raise ValueError('标签名称配置不合法')
    out['tag_names'] = {module_id(k): str(v)[:40] for k, v in names.items()}
    pois = c.get('pois', [])
    if not isinstance(pois, list) or len(pois) > 100:
        raise ValueError('固定物体数量最多100个')
    out['pois'] = [dict(name=str(p.get('name', '目标'))[:40], kind=str(p.get('kind', '目标'))[:20],
                        x=number(p['x'], '物体X'), y=number(p['y'], '物体Y')) for p in pois]
    return out


def layout_from_distances(data):
    """以A为原点、AB为正X轴，C/D相对AB的上下位置解除镜像歧义。"""
    ds = {k: number(data[k], k + '距离', .01, 10000) for k in PAIRS}
    L = ds['AB']
    coords = [(0., 0.), (L, 0.)]
    for label, da, db in [('C', ds['AC'], ds['BC']), ('D', ds['DA'], ds['BD'])]:
        x = (da * da - db * db + L * L) / (2 * L)
        y2 = da * da - x * x
        if y2 < -1e-6:
            raise ValueError(label + '的距离违反三角形约束，请重新测量')
        side = data.get(label + '_side', 'below')
        if side not in ('above', 'below'):
            raise ValueError('上下位置设置不合法')
        coords.append((x, math.sqrt(max(0., y2)) * (1 if side == 'above' else -1)))
    measured_cd = math.dist(coords[2], coords[3])
    warnings = []
    if abs(measured_cd - ds['CD']) > max(.2, ds['CD'] * .03):
        warnings.append(f'CD重建值{measured_cd:.2f}m与输入{ds["CD"]:.2f}m不一致；请复核或直接编辑坐标。')
    # 不一致数据返回警告，坐标仅为重建建议，由用户确认后保存。
    return {'coordinates': [dict(label=k, x=xy[0], y=xy[1]) for k, xy in zip(LABELS, coords)],
            'warnings': warnings}


class Store:
    def __init__(self, config_path):
        self.path = Path(config_path)
        self.lock = threading.RLock()
        self.config = default_config()
        if self.path.exists():
            self.config = validate_config(json.loads(self.path.read_text(encoding='utf-8')))
        self.devices = {}
        self.ranges = {}
        self.raw = deque(maxlen=80)
        self.errors = deque(maxlen=10)
        self.packet_count = 0
        self.frame_count = 0

    def save(self, c):
        clean = validate_config(c)
        with self.lock:
            temporary = self.path.with_suffix('.tmp')
            temporary.write_text(json.dumps(clean, ensure_ascii=False, indent=2), encoding='utf-8')
            temporary.replace(self.path)
            self.config = clean

    def ingest(self, packet, address, now=None):
        now = time.time() if now is None else now
        if packet.get('v') != 1 or packet.get('role') not in ('tag', 'anchor'):
            raise ValueError('不支持的数据版本或设备角色')
        device = packet.get('device')
        if not isinstance(device, str) or not 1 <= len(device) <= 64:
            raise ValueError('设备身份不合法')
        identity = module_id(packet['self_id'])
        count, seq, session = packet['records'], packet['seq'], packet['session']
        if type(count) is not int or not 1 <= count <= 10:
            raise ValueError('帧记录数不合法')
        if type(seq) is not int or not 0 <= seq < 2 ** 32 or type(session) is not int or not 0 <= session < 2 ** 32:
            raise ValueError('数据包序号或会话不合法')
        h = packet['hex']
        if not isinstance(h, str) or len(h) > 256:
            raise ValueError('原始字节长度不合法')
        data = bytes.fromhex(h)
        if len(data) > 128:
            raise ValueError('原始字节过长')
        with self.lock:
            for other, d in self.devices.items():
                if other != device and d['id'] == identity and d['role'] == packet['role'] and now - d['seen'] < 5:
                    raise ValueError(f'{packet["role"]} ID {identity}重复，请修改C3本机ID')
            old = self.devices.get(device)
            if old is None or old['session'] != session:
                old = dict(session=session, seq=None, parser=FrameParser(count), gaps=0)
                self.devices[device] = old
            elif old.get('id') != identity or old.get('role') != packet['role'] or old['parser'].records != count:
                raise ValueError('同一启动会话内设备身份或帧长度发生变化')
            previous = old['seq']
            if previous is not None:
                delta = (seq - previous) & 0xFFFFFFFF
                if delta == 0 or delta >= 0x80000000:
                    return  # 丢弃重复包/迟到包，避免旧测距覆盖新测距
                if delta > 1:
                    old['gaps'] += delta - 1
                    old['parser'].buffer.clear()  # 缺包之后不能将两段串口数据拼成一帧
            old.update(seq=seq, seen=now, id=identity, role=packet['role'], ip=address[0])
            self.packet_count += 1
            if data:
                self.raw.appendleft(
                    dict(time=now, device=device, id=identity, role=packet['role'], hex=data.hex(' ').upper()))
            for f in old['parser'].feed(data):
                self.frame_count += 1
                for r in f['rows']:
                    if packet['role'] == 'tag' and f['peer_role'] == 'anchor':
                        tag, anchor = identity, r['peer_id']
                    elif packet['role'] == 'anchor' and f['peer_role'] == 'tag':
                        tag, anchor = r['peer_id'], identity
                    else:
                        continue
                    if r['meters'] > 0:
                        self.ranges.setdefault(tag, {})[anchor] = dict(meters=r['meters'], time=now)

    def snapshot(self, now=None):
        now = time.time() if now is None else now
        with self.lock:
            c = copy.deepcopy(self.config)
            tags = []
            for tid, ranges in self.ranges.items():
                items = []
                for a in c['anchors']:
                    r = ranges.get(a['id'])
                    items.append(dict(anchor=a['label'], id=a['id'], meters=r['meters'] if r else None,
                                      age=max(0, now - r['time']) if r else None,
                                      fresh=bool(r and now - r['time'] <= c['stale_seconds'])))
                position = None
                status = '等待四基站距离'
                if all(r['fresh'] for r in items):
                    times = [ranges[a['id']]['time'] for a in c['anchors']]
                    if max(times) - min(times) > c['max_span_seconds']:
                        status = '四距离时间跨度过大'
                    else:
                        try:
                            position = solve_position(c['anchors'], [r['meters'] for r in items], c['tag_z'])
                            status = '定位中' if position['rms'] <= .75 else '距离不一致，位置可信度较低'
                        except ValueError as e:
                            status = str(e)
                elif any(r['meters'] is not None and not r['fresh'] for r in items):
                    status = '距离已过期，等待更新'
                tags.append(dict(id=tid, name=c['tag_names'].get(tid, '标签 ' + tid), ranges=items,
                                 position=position, status=status))
            devices = [dict(device=k, id=d['id'], role=d['role'], ip=d['ip'], age=max(0, now - d['seen']),
                            online=now - d['seen'] < 5, gaps=d['gaps'], bad_crc=d['parser'].bad_crc)
                       for k, d in self.devices.items()]
            return dict(config=c, tags=tags, devices=devices, raw=list(self.raw), errors=list(self.errors),
                        packet_count=self.packet_count, frame_count=self.frame_count, time=now)


def udp_receiver(store, sock):
    while True:
        data, address = sock.recvfrom(4096)
        try:
            packet = json.loads(data)
            if not isinstance(packet, dict):
                raise ValueError('JSON必须是对象')
            store.ingest(packet, address)
        except (ValueError, KeyError, TypeError, OverflowError) as e:
            with store.lock:
                store.errors.appendleft(str(e)[:180])


# def make_handler(store):
#     class Handler(BaseHTTPRequestHandler):
#         def log_message(self, *_):
#             pass
#
#         def respond(self, status, data, mime='application/json; charset=utf-8'):
#             if not isinstance(data, bytes):
#                 data = json.dumps(data, ensure_ascii=False, allow_nan=False).encode('utf-8')
#             self.send_response(status)
#             self.send_header('Content-Type', mime)
#             self.send_header('Content-Length', str(len(data)))
#             self.send_header('Cache-Control', 'no-store')
#             self.end_headers()
#             self.wfile.write(data)
#
#         def do_GET(self):
#             if self.path == '/api/state':
#                 self.respond(200, store.snapshot());
#                 return
#             assets = {'/': ('index.html', 'text/html; charset=utf-8'),
#                       '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
#                       '/style.css': ('style.css', 'text/css; charset=utf-8')}
#             if self.path not in assets:
#                 self.respond(404, {'error': '页面不存在'});
#                 return
#             filename, mime = assets[self.path]
#             self.respond(200, (ROOT / 'static' / filename).read_bytes(), mime)
#
#         def do_POST(self):
#             try:
#                 length = int(self.headers.get('Content-Length', 0))
#                 if not 0 < length <= 64000:
#                     raise ValueError('请求大小不合法')
#                 data = json.loads(self.rfile.read(length))
#                 if not isinstance(data, dict):
#                     raise ValueError('请求必须为对象')
#                 if self.path == '/api/config':
#                     store.save(data);
#                     result = {'ok': True}
#                 elif self.path == '/api/layout':
#                     result = layout_from_distances(data)
#                 else:
#                     self.respond(404, {'error': '接口不存在'});
#                     return
#                 self.respond(200, result)
#             except (ValueError, KeyError, TypeError, OverflowError) as e:
#                 self.respond(400, {'error': str(e)})
#
#     return Handler

def make_handler(store):
    # 调用环境密钥鉴权————已废弃
    # 用法：export UWB_USER="admin"
    # 用法：export UWB_PASSWORD="123"
    # AUTH_USER = os.environ.get('UWB_USER', 'admin')
    # AUTH_PASSWORD = os.environ['UWB_PASSWORD']

    AUTH_USER = 'admin'
    AUTH_PASSWORD = 'Passw0rd@uwb'

    expected_auth = 'Basic ' + base64.b64encode(
        f'{AUTH_USER}:{AUTH_PASSWORD}'.encode('utf-8')
    ).decode('ascii')

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        # 浏览器原生身份验证
        def check_auth(self):
            received = self.headers.get('Authorization', '')

            if hmac.compare_digest(received, expected_auth):
                return True

            self.send_response(401)
            self.send_header(
                'WWW-Authenticate',
                'Basic realm="UWBNav", charset="UTF-8"'
            )
            self.send_header('Content-Length', '0')
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            return False

        def respond(self, status, data, mime='application/json; charset=utf-8'):
            if not isinstance(data, bytes):
                data = json.dumps(
                    data,
                    ensure_ascii=False,
                    allow_nan=False
                ).encode('utf-8')

            self.send_response(status)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if not self.check_auth():
                return

            if self.path == '/api/state':
                self.respond(200, store.snapshot())
                return

            assets = {
                '/': ('index.html', 'text/html; charset=utf-8'),
                '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
                '/style.css': ('style.css', 'text/css; charset=utf-8')
            }

            if self.path not in assets:
                self.respond(404, {'error': '页面不存在'})
                return

            filename, mime = assets[self.path]
            self.respond(
                200,
                (ROOT / 'static' / filename).read_bytes(),
                mime
            )

        def do_POST(self):
            if not self.check_auth():
                return

            try:
                length = int(self.headers.get('Content-Length', 0))

                if not 0 < length <= 64000:
                    raise ValueError('请求大小不合法')

                data = json.loads(self.rfile.read(length))

                if not isinstance(data, dict):
                    raise ValueError('请求必须为对象')

                if self.path == '/api/config':
                    store.save(data)
                    result = {'ok': True}

                elif self.path == '/api/layout':
                    result = layout_from_distances(data)

                else:
                    self.respond(404, {'error': '接口不存在'})
                    return

                self.respond(200, result)

            except (ValueError, KeyError, TypeError, OverflowError) as e:
                self.respond(400, {'error': str(e)})

    return Handler


def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('192.0.2.1', 1))
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description='UWBNav 本地上位机')
    ap.add_argument('--http-port', type=int, default=8000)
    ap.add_argument('--udp-port', type=int, default=7788)
    args = ap.parse_args()
    store = Store(ROOT / 'config.json')
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(('0.0.0.0', args.udp_port))
    server = ThreadingHTTPServer(('0.0.0.0', args.http_port), make_handler(store))
    threading.Thread(target=udp_receiver, args=(store, udp), daemon=True).start()
    # print(f'浏览器打开：http://127.0.0.1:{args.http_port}')
    local_ip = get_local_ip()
    print(f'本机访问：http://127.0.0.1:{args.http_port}')
    print(f'局域网访问：http://{local_ip}:{args.http_port}')
    print(f'ESP32 UDP目标端口：{args.udp_port}')
    # 候选 IP 功能————已弃用
    # ips = set()
    # try:
    #     ips.update(socket.gethostbyname_ex(socket.gethostname())[2])
    #     probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    #     probe.connect(('192.0.2.1', 1));
    #     ips.add(probe.getsockname()[0]);
    #     probe.close()
    # except OSError:
    #     pass
    # 候选 IP 功能————正在使用
    ips = set()
    ips.add(get_local_ip())

    try:
        ips.update(socket.gethostbyname_ex(socket.gethostname())[2])
    except OSError as e:
        print(f'主机名解析失败：{e}')
    # 候选 IP 功能————结束
    print('电脑候选局域网IP：' + ', '.join(ip for ip in ips if not ip.startswith('127.')))
    print('请选择连接同一Wi-Fi的网卡IPv4填写到C3的PC_IP；候选IP可能包含VPN/其他网卡。')
    print('允许防火墙入站 UDP；Ctrl+C 停止。')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close();
        udp.close()


if __name__ == '__main__':
    main()
