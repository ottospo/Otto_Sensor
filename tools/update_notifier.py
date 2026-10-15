r"""Pops up the Otto Sensor update page on this PC when a device reports UPDATE_AVAILABLE.

Setup:  pip install paho-mqtt
Run:    set OTTO_MQTT_PASS=<your mqtt password>
        python tools\update_notifier.py
The password is read from the environment so it is never stored in the repo.
"""
import json
import os
import ssl
import webbrowser

import paho.mqtt.client as mqtt

HOST = os.environ.get("OTTO_MQTT_HOST", "d8ef6c1c.ala.us-east-1.emqxsl.com")
PORT = int(os.environ.get("OTTO_MQTT_PORT", "8883"))
USER = os.environ.get("OTTO_MQTT_USER", "Otto_Sponring")
PASSWORD = os.environ.get("OTTO_MQTT_PASS", "")
TOPIC = "/ottosensors-dev/device/+/state"

opened = set()


def on_connect(client, userdata, flags, rc, *args):
    print("connected" if rc == 0 else f"connect failed rc={rc}")
    client.subscribe(TOPIC)


def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload)
        print("MQTT state:", data)
    except ValueError:
        return
    if data.get("state") != "UPDATE_AVAILABLE":
        return
    key = (data.get("imei"), data.get("new_version"))
    if key in opened:
        return
    opened.add(key)
    url = data.get("url") or data.get("url_ip")
    print(f"Update {data.get('new_version')} available for {data.get('imei')}: {url}")
    webbrowser.open(url)


try:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
except AttributeError:
    client = mqtt.Client()
client.username_pw_set(USER, PASSWORD)
client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
client.on_connect = on_connect
client.on_message = on_message
client.connect(HOST, PORT)
client.loop_forever()
