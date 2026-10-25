r"""Open the Otto Sensor update page when an AWS IoT device reports UPDATE_AVAILABLE.

Set AWS_IOT_ENDPOINT if using a different AWS IoT endpoint. Device credentials
default to the PEM files in certs/ and can be overridden with the AWS_IOT_*_FILE
environment variables.
"""
import json
import os
import ssl
import webbrowser
from pathlib import Path

import paho.mqtt.client as mqtt

HOST = os.environ.get("AWS_IOT_ENDPOINT", "a2y7jhiel7juf2-ats.iot.us-west-2.amazonaws.com")
PORT = int(os.environ.get("AWS_IOT_PORT", "8883"))
CERT_DIR = Path(__file__).resolve().parents[1] / "certs"
CA_FILE = os.environ.get("AWS_IOT_CA_FILE", str(CERT_DIR / "AmazonRootCA1.pem"))
CERT_FILE = os.environ.get("AWS_IOT_CERT_FILE", str(CERT_DIR / "device.cert.pem"))
KEY_FILE = os.environ.get("AWS_IOT_KEY_FILE", str(CERT_DIR / "device.private.key"))
TOPIC = os.environ.get("AWS_IOT_STATE_TOPIC", "ottosensors/device/+/state")

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
client.tls_set(
    ca_certs=CA_FILE,
    certfile=CERT_FILE,
    keyfile=KEY_FILE,
    cert_reqs=ssl.CERT_REQUIRED,
)
client.on_connect = on_connect
client.on_message = on_message
client.connect(HOST, PORT)
client.loop_forever()
