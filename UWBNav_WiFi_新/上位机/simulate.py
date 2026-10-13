"""明确标识的模拟器：只用于验证软件链路，不代表硬件定位精度。"""
import argparse
import json
import math
import random
import socket
import time
from pathlib import Path
from server import crc16, default_config


def frame(anchor_id, meters):
    data = b'\x45\x91\x01\x00\x01' + int(anchor_id, 16).to_bytes(4, 'big')
    data += max(1, min(65535, round(meters*100))).to_bytes(2, 'big')
    return data + crc16(data).to_bytes(2, 'big')


def main():
    ap = argparse.ArgumentParser(description='发送模拟测距数据，不是实测数据')
    ap.add_argument('--ip', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=7788)
    args = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    session = random.randrange(2**32)
    sequences = [0, 0]
    start = time.time()
    print('正在发送【模拟数据】，标签ID A1 / A2。Ctrl+C停止。')
    try:
        while True:
            path = Path(__file__).with_name('config.json')
            c = json.loads(path.read_text(encoding='utf-8')) if path.exists() else default_config()
            aa = c['anchors']; elapsed = time.time()-start
            xmin, xmax = min(a['x'] for a in aa), max(a['x'] for a in aa)
            ymin, ymax = min(a['y'] for a in aa), max(a['y'] for a in aa)
            for i, identity in enumerate(('A1', 'A2')):
                x = (xmin+xmax)/2 + (xmax-xmin)*.23*math.sin(elapsed*.35+i*2)
                y = (ymin+ymax)/2 + (ymax-ymin)*.23*math.cos(elapsed*.27+i*2)
                for a in aa:
                    r = math.sqrt((x-a['x'])**2+(y-a['y'])**2+(c.get('tag_z', 0)-a['z'])**2)
                    packet = dict(v=1, device='SIMULATED-'+identity, session=session,
                                  seq=sequences[i], role='tag', self_id=identity, records=1,
                                  hex=frame(a['id'], r).hex())
                    sock.sendto(json.dumps(packet).encode(), (args.ip, args.port))
                    sequences[i] += 1
            time.sleep(.5)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()


if __name__ == '__main__':
    main()
