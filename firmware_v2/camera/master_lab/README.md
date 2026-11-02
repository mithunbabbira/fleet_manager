# Master lab SoftAP (ESP32-C6)

See parent [`../README.md`](../README.md).

Flash to the **fleet carrier C6** USB port (lab only — replaces whatever is on the device until you reflash `fleet_v2_master`).

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
cd firmware_v2/camera/master_lab
pio run -t upload --upload-port /dev/cu.usbmodem1101
```
