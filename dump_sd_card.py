#!/usr/bin/env python3
"""
Скрипт для прямого копіювання файлів із MicroSD карти плати JC3248W535C через USB кабель на Mac.
"""

import os
import sys
import time
import serial

PORT = "/dev/cu.usbmodem101"
BAUD = 115200
DEST_DIR = "sd_backup"

def main():
    print("==================================================")
    print(" 📥 JC3248W535C: Прямий дамп MicroSD через USB")
    print(f" Порт: {PORT}")
    print(f" Папка для збереження: {DEST_DIR}/")
    print("==================================================")

    try:
        ser = serial.Serial(PORT, BAUD, timeout=2)
    except Exception as e:
        print(f"❌ Не вдалося відкрити порт {PORT}: {e}")
        sys.exit(1)

    time.sleep(1.5)
    ser.reset_input_buffer()

    # Перевірка зв'язку
    print("🔌 Перевірка зв'язку з платою...")
    connected = False
    for _ in range(5):
        ser.write(b"PING\n")
        line = ser.readline().decode(errors="ignore").strip()
        if "PONG" in line:
            connected = True
            break
        time.sleep(0.3)

    if not connected:
        print("❌ Плата не відповідає на команду PING. Переконайтеся, що на неї прошито скетч dumper!")
        ser.close()
        sys.exit(1)

    # Перевірка наявності SD картки
    ser.write(b"STATUS\n")
    status_line = ser.readline().decode(errors="ignore").strip()
    if "SD_OK" not in status_line:
        print("⚠️ УВАГА: MicroSD картку не виявлено в роз'ємі плати!")
        print("Вставте картку MicroSD у роз'єм на зворотному боці плати та спробуйте ще раз.")
        ser.close()
        sys.exit(1)

    print("✅ Картку MicroSD знайдено та змонтовано!")
    print("🔍 Сканування списку файлів на картці...")

    ser.write(b"LIST\n")
    files = []
    while True:
        line = ser.readline().decode(errors="ignore").strip()
        if not line:
            continue
        if line == "END_LIST":
            break
        if line.startswith("FILE:"):
            parts = line.split(":")
            if len(parts) >= 3:
                fpath = parts[1]
                fsize = int(parts[2])
                files.append((fpath, fsize))

    if not files:
        print("ℹ️ На картці не знайдено файлів або вона порожня.")
        ser.close()
        sys.exit(0)

    total_bytes = sum(sz for _, sz in files)
    print(f"\n📁 Знайдено файлів: {len(files)} (Загальний об'єм: {total_bytes / (1024*1024):.2f} MB)")
    for p, sz in files:
        print(f"  - {p} ({sz / 1024:.1f} KB)")

    print(f"\n🚀 Початок копіювання у папку '{DEST_DIR}/'...\n")
    os.makedirs(DEST_DIR, exist_ok=True)

    copied = 0
    for idx, (fpath, fsize) in enumerate(files, 1):
        rel_path = fpath.lstrip("/")
        local_path = os.path.join(DEST_DIR, rel_path)
        os.makedirs(os.path.dirname(local_path), exist_ok=True)

        if os.path.exists(local_path) and os.path.getsize(local_path) == fsize:
            print(f"[{idx}/{len(files)}] ⏩ Вже завантажено: {fpath}")
            copied += 1
            continue

        print(f"[{idx}/{len(files)}] Завантаження: {fpath} ({fsize / 1024:.1f} KB)... ", end="", flush=True)

        ser.write(f"GET {fpath}\n".encode())

        # Читаємо заголовок START_FILE
        start_line = ser.readline().decode(errors="ignore").strip()
        while not start_line.startswith("START_FILE:"):
            if "ERR:" in start_line:
                break
            start_line = ser.readline().decode(errors="ignore").strip()

        if "ERR:" in start_line:
            print("❌ Помилка відкриття файлу на картці")
            continue

        parts = start_line.split(":")
        actual_size = int(parts[2]) if len(parts) >= 3 else fsize

        # Зчитуємо сирі байти
        received = bytearray()
        bytes_left = actual_size
        ser.timeout = 10
        while bytes_left > 0:
            chunk = ser.read(min(bytes_left, 4096))
            if not chunk:
                break
            received.extend(chunk)
            bytes_left -= len(chunk)

        # Читаємо завершальний рядок END_FILE
        ser.readline()

        if len(received) == actual_size:
            with open(local_path, "wb") as out_f:
                out_f.write(received)
            print("✅ OK")
            copied += 1
        else:
            print(f"⚠️ Неповний файл (отримано {len(received)} / {actual_size} байт)")

    ser.close()
    print("\n==================================================")
    print(f"🎉 Завершено! Успішно скопійовано {copied} з {len(files)} файлів.")
    print(f"📂 Всі файли збережено в: {os.path.abspath(DEST_DIR)}")
    print("==================================================")

if __name__ == "__main__":
    main()

