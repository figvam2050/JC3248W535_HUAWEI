#!/usr/bin/env bash
# Скрипт для повного відновлення рідної заводської прошивки JC3248W535C

PORT="/dev/cu.usbmodem101"
BACKUP_FILE="backup/jc3248w535c_factory_full_16MB.bin"

if [ ! -f "$BACKUP_FILE" ]; then
    echo "Помилка: Файл бекапу $BACKUP_FILE не знайдено!"
    exit 1
fi

echo "=========================================================="
echo " Початок відновлення заводської прошивки (16 МБ)..."
echo " Порт: $PORT"
echo " Файл: $BACKUP_FILE"
echo "=========================================================="

esptool --port "$PORT" write-flash 0x0 "$BACKUP_FILE"

echo ""
echo "Готово! Плата повернута до стану з заводу."

