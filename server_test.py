#!/usr/bin/env python3
"""
server_mau.py - Server mẫu cho Hộp cơm thông minh (1 thiết bị): nhận dữ liệu, ghi CSV, gửi lệnh

Cài đặt:   pip install paho-mqtt
Chạy:      python server_mau.py                         (mặc định broker.hivemq.com, root hopcom-khkt2026)
           python server_mau.py --broker 192.168.1.10   (broker riêng trong mạng LAN)
Gửi lệnh:  python server_mau.py --cmd set_food food=thit
           python server_mau.py --cmd new_session food=ca
           python server_mau.py --cmd set_interval s=30
           python server_mau.py --cmd calibrate_mq
--root phải GIỐNG MQTT_TOPIC_ROOT trong app_config.h.

Topic:  <root>/status    (thiết bị gửi, retain)  online / offline
        <root>/telemetry (thiết bị gửi)          số đo + kết quả thiết bị TỰ đánh giá
        <root>/cmd       (server gửi)            lệnh điều khiển
Thiết bị tự đo và tự đánh giá (food_eval.c); server chỉ nhận, in ra và ghi vào du_lieu_do.csv
để làm bảng thực nghiệm trong báo cáo.
"""
import argparse
import csv
import json
import os
import time
import uuid

import paho.mqtt.client as mqtt

ROOT = "hopcom-khkt2026"   # đổi bằng --root; phải giống MQTT_TOPIC_ROOT của firmware
CSV_FILE = r"E:\KHKT\du_lieu_do.csv"   # đổi bằng --csv
COLUMNS = ["food", "age_s", "budget_s", "t", "rh", "thermal", "slope", "co2", "voc", "nh3", "h2s",
           "score", "gas_level", "time_level", "level", "msg"]


def log_csv(t):
    new = not os.path.exists(CSV_FILE)
    with open(CSV_FILE, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["thoi_gian"] + COLUMNS)
        w.writerow([time.strftime("%Y-%m-%d %H:%M:%S")] + [t.get(k) for k in COLUMNS])


def on_connect(client, userdata, flags, reason_code, properties):
    print(f"[server] kết nối broker: {reason_code}")
    client.subscribe(f"{ROOT}/telemetry", qos=1)
    client.subscribe(f"{ROOT}/status", qos=1)


def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode("utf-8")) if msg.payload else {}
    except ValueError:
        print(f"[server] JSON lỗi trên {msg.topic}")
        return

    if msg.topic == f"{ROOT}/telemetry":
        log_csv(data)
        print(f"[server] {data.get('food')} T={data.get('t')}C ({data.get('thermal')}, {data.get('slope')} C/ph) "
              f"RH={data.get('rh')}% CO2={data.get('co2')} "
              f"VOC={data.get('voc')} NH3={data.get('nh3')} H2S={data.get('h2s')} | "
              f"S={data.get('score')} -> {data.get('level')} ({data.get('msg')})")
    elif msg.topic == f"{ROOT}/status":
        print(f"[server] thiết bị: {data.get('state')} ip={data.get('ip', '-')}")


def make_client(broker, port, user, password):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"hopcom-server-{uuid.uuid4().hex[:6]}",
                    protocol=mqtt.MQTTv5)
    if user:
        c.username_pw_set(user, password)
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect(broker, port, keepalive=30)
    return c


def send_cmd(c, cmd, kv):
    payload = {"cmd": cmd}
    for item in kv:
        k, _, v = item.partition("=")
        payload[k] = int(v) if v.isdigit() else v
    c.publish(f"{ROOT}/cmd", json.dumps(payload), qos=1).wait_for_publish(5)
    print(f"[server] đã gửi lệnh: {payload}")


def main():
    global ROOT, CSV_FILE
    ap = argparse.ArgumentParser(description="Server mẫu Hộp cơm thông minh")
    ap.add_argument("--csv", default=CSV_FILE, help="file CSV ghi dữ liệu đo")
    ap.add_argument("--broker", default="broker.hivemq.com")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--root", default=ROOT, help="topic gốc, giống MQTT_TOPIC_ROOT trong app_config.h")
    ap.add_argument("--user", default="")
    ap.add_argument("--password", default="")
    ap.add_argument("--cmd", nargs="+", metavar="CMD",
                    help="gửi lệnh rồi thoát, vd: --cmd set_food food=ca")
    a = ap.parse_args()
    ROOT = a.root
    CSV_FILE = a.csv
    if os.path.dirname(CSV_FILE):
        os.makedirs(os.path.dirname(CSV_FILE), exist_ok=True)
    print(f"[server] broker {a.broker}:{a.port}, topic gốc '{ROOT}'")

    c = make_client(a.broker, a.port, a.user, a.password)
    if a.cmd:
        c.loop_start()
        send_cmd(c, a.cmd[0], a.cmd[1:])
        c.loop_stop()
        return
    print(f"[server] đang chạy, ghi dữ liệu vào {CSV_FILE}. Ctrl+C để dừng.")
    c.loop_forever()


if __name__ == "__main__":
    main()