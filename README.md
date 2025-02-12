# Anker Updater

## Getting Started
- Start hawkbit docker container. Go to hawkbit folder and run
```bash
sudo docker-compose up -d
```
- In Hawkbit settings allow that targets can be authorized by a token.
- Register device in Hawkbit. (Could be automated in a production process)
- Generate device-binaries by creating or using a csv file like in device_bin folder (Could be automated in a production process).
- Create binary file by running
```bash
python3 nvs_partition_gen.py generate c_anker_device.csv device.bin 0x3000
```
- Flash the binary file with
```bash
esptool.py -p /dev/ttyUSB0 write_flash --flash_mode=dio 0x210000 device.bin
```
- Flash firmware with
```bash
pio run --target upload --upload-port /dev/ttyUSB0
```
- Upload 2 firmwares with different versions to hawkbit
- Assign Hawkbit-Disto to C-Anker
