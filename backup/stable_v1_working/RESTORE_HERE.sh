#!/bin/bash
set -e
echo "Відновлення вихідного коду стабільної версії 1.0..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

cp -r "$SCRIPT_DIR/src/"* "$ROOT_DIR/src/"
cp "$SCRIPT_DIR/platformio.ini" "$ROOT_DIR/"
cp "$SCRIPT_DIR/lv_conf.h" "$ROOT_DIR/"

echo "Вихідний код успішно відновлено! Запуск збірки та прошивки..."
cd "$ROOT_DIR"
pio run -t upload --upload-port /dev/cu.usbmodem101
echo "Готово! Стабільна версія 1.0 знову працює на пристрої."
