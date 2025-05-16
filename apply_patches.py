#!/usr/bin/env python3
from __future__ import annotations
from typing import List
from dataclasses import dataclass, field
import os
import sys
import subprocess
import signal
"""
Применение всех изменений из ветки в которой находится этот скрипт в ветку TARGET_BRANCH
    с ограничением коммитов от START_COMMIT до END_COMMIT (включительно)
    с сохранением авторства и оригинальных хэшей коммитов

Производится поиск всех измененных файлов в указанном диапазоне (START-END), после
    чего эти пути файлов будут разложены по указанным заданным группам

Желаемые группы файлов описаны в словаре GROUPS
  - в GROUPS можно указывать как папки целиком (т.е. файл должен начинаться с
    указанной папки), так и конкретные файлы
  - измененные файлы в указанных промежутках, но не входящие в GROUPS НЕ(!) будут
    обработаны, но будут отображены в уведомлении (т.е. можно умышленно пропускать
    какие-то файлы)

Подсказка: если нужно что-то проигнорировать или начать с прерванной группы, то
    можно просто закомментировать ненужные группы в GROUPS (а перед запуском
    удалить папку ./patches/)

Для каждого найденного файла в группе - будут найдены все коммиты и созданы патчи
    в том порядке, в каком они были сделаны

Патчи будут применены последовательно для группы целиком, после чего будут закоммичены
    с объедененным комментарием

В процессе применения патчей через git am - могут возникать ошибки.
    При возникновении ошибки:
        - можно поправить соответствующий patch-файл ручками и попробовать
            применить его еще раз этим скриптом
        - можно применить patch-файл вручную (параллельно в другой консоли),
            а тут пропустить его и перейти к следующему.
"""

# Игнорируем Ctrl+C (SIGINT):
signal.signal(signal.SIGINT, signal.SIG_IGN)

#TODO вынести в параметры запуска START/END/TARGET
START_COMMIT = "9d7ac3887e14"
#END_COMMIT = "ecc33b0"
END_COMMIT = "HEAD"
TARGET_BRANCH = "features/SOFT-4884-v6.14.y"

GROUPS = {
    "Drivers: WBEC": [
        "drivers/mfd/",
        "include/linux/mfd/",
        "Documentation/driver-api/wbec.rst"
    ],
    "Drivers: Power": [
        "drivers/power/supply/",
        "drivers/regulator/"
    ],
    "Drivers: Pincontrol": [
        "drivers/pinctrl/pinctrl-mcp23s08.c",
        "drivers/pinctrl/sunxi/pinctrl-sunxi.c",
        "drivers/pinctrl/sunxi/pinctrl-sunxi.h"
    ],
    "Drivers: NVMEM": [
        "drivers/nvmem/sunxi_sid.c",
    ],
    "Drivers: GPIO": [
        "drivers/gpio/",
        "include/asm-generic/gpio.h",
    ],
    "Drivers: ADC": [
        "drivers/iio/adc/",
        "include/linux/iio/adc/"
    ],
    "Drivers: PWM": [
        "drivers/pwm/",
    ],
    "Drivers: CAN": [
        "drivers/net/can/"
    ],
    "Drivers: RTC": [
        "drivers/rtc/",
    ],
    "Drivers: Watchdog": [
        "drivers/watchdog/",
    ],
    "Drivers: MMC": [
        "drivers/mmc/host/sunxi-mmc.c",
    ],
    "Drivers: NTC-resistor": [
        "include/linux/platform_data/ntc_thermistor.h",
        "drivers/hwmon/ntc_thermistor.c",
        "Documentation/hwmon/ntc_thermistor.rst"
    ],
    "Drivers: UART": [
        "drivers/tty/serial/",
        "include/linux/serial_8250.h",
        "include/linux/serial_core.h",
        "include/uapi/linux/serial_core.h"
    ],
    "Drivers: Input": [
        "drivers/input/",
    ],
    "Drivers: Clock": [
        "drivers/clk/",
        "drivers/clk/sunxi-ng/",
        "include/linux/clk-provider.h"
    ],
    "Drivers: W1": [
        "drivers/w1/"
    ],
    "Drivers: Ethernet": [
        "drivers/net/ethernet/"
    ],
    "Drivers: Bluetooth": [
        "drivers/bluetooth/"
    ],
    "Drivers: USB": [
        "drivers/usb/",
        "include/linux/usb/",
        "drivers/phy/allwinner/phy-sun4i-usb.c",
        "Documentation/usb/gadget-testing.rst"
    ],
    "Scripts": [
        "scripts/",
        ".gitignore",
        ".gitmodules",
        "Jenkinsfile"
    ],
    "Drivers: WIFI": [
        "drivers/net/wireless/realtek/rtl8733bu",
        "drivers/net/wireless/realtek/rtl8723bu",
        "drivers/net/wireless/realtek/Kconfig",
        "drivers/net/wireless/realtek/Makefile"
    ],
    "Configs": [
        "arch/arm/configs/",
    ],
    "Device Tree": [
        "arch/arm/boot/dts/",
        "arch/arm64/boot/dts/",
        "Documentation/devicetree/",
        "drivers/of/overlay.c",
        "arch/arm/Makefile",
        "arch/arm/boot/dts/Makefile"
    ],
}

groups = list()
# ------------------------

@dataclass
class Commit:
    hsh: str  # ХЭШ коммита
    comment: str

@dataclass
class File:
    name: str
    patches: List[str] = field(default_factory=list)
    commits: List[Commit] = field(default_factory=list)

@dataclass
class Group:
    name: str
    files: List[File] = field(default_factory=list)
    paths: List[str] = field(default_factory=list)

# --- Утилиты ---
def run_cmd(cmd:List[str], capture:bool=False):
    """Выполнить команду Git"""
    # print(f"[RUN] {' '.join(cmd)}")
    if capture:
        return subprocess.check_output(cmd).decode().strip()
    return subprocess.run(cmd, check=True)


def get_commits_for_file(file:File) -> List[Commit]:
    commits = []
    result = run_cmd([
        "git", "log", "--pretty=format:%H %s", "--reverse",
        f"{START_COMMIT}^..{END_COMMIT}",
        "--", file.name
    ], capture=True)

    if result:
        for line in result.strip().splitlines():
            hsh, *rest = line.split(' ', maxsplit=1)
            comment = rest[0] if rest else ""
            commits.append(Commit(hsh, comment.strip()))
    return commits

def prepare_group() -> None:
    ### Шаг 0: Подготовить структуру куда буду сливать все наработки
    for category, paths in GROUPS.items():
        group = Group(name=category)
        groups.append(group)
        for path in paths:
            if os.path.exists(path):
                group.paths.append(path)

            else:
                print(f"❌ Нет пути: {path} для категории '{category}'")
                sys.exit(1)

def get_changed_files_in_range() -> List[str]:
    ### Шаг 1: Получить все изменённые файлы в рамках диапазона
    result = run_cmd([
        "git", "diff", "--name-only", f"{START_COMMIT}^..{END_COMMIT}"
    ], capture=True)
    result_files = result.splitlines()
    return [f for f in result_files if f != "debian/changelog"]

def categorize_files(files:List[str]) -> List[str]:
    ### Шаг 2: Разбиваю найденные файлы на группы
    not_founded_files = list()

    for file_name in files:
        matched = False
        for group in groups:
            for path in group.paths:
                if file_name.startswith(path):
                    group.files.append(
                        File(file_name)
                    )
                    matched = True
                    break
            if matched:
                # Нет смысла прогонять оставшиеся группы:
                break
        if not matched:
            not_founded_files.append(file_name)

    return not_founded_files

def print_group() -> None:
    # Вывожу всю группу на экран:
    for group in groups:
        print(f"\n📦 {group.name}:")
        for file in group.files:
            print(f"  📄 {file.name}")

            for patch, commit in zip(file.patches, file.commits):
                print(f"     ┌ {commit.hsh}: {commit.comment}")
                print(f"     └ {patch}")

def create_patches_for_group(group: Group, index: int) -> None:
    # Собрать патчи для группы и положить их в одну папку
    patch_dir = os.path.join("patches", f"{index:02d}.{group.name.replace(':', '').replace(' ', '_')}")
    os.makedirs(patch_dir, exist_ok=True)

    patch_num = 0
    for index_file, file in enumerate(group.files):
        for index_patch, commit in enumerate(file.commits):
            patch_num += 1
            patch_name = f"{patch_num:03d}.patch"
            patch_path = os.path.join(patch_dir, patch_name)
            file.patches.append(patch_path)
            with open(patch_path, "w") as f:
                # Сохраняю format-patch для указанного коммита для указанного
                # файла
                f.write(run_cmd([
                    "git", "format-patch", "-1", "--stdout", commit.hsh, "--", file.name
                ], capture=True))

def confirmation_request() -> str:
    """
    Выводит интерфейс для пользователя и ожидает команду.
    :return: 'retry', 'skip' или 'abort'
    """
    while True:
        choice = input("🔁 [r]etry / ⏩ [s]kip / ❌ [a]bort? ").strip().lower()
        if choice in ('r', 'retry'):
            return 'retry'
        elif choice in ('s', 'skip'):
            return 'skip'
        elif choice in ('a', 'abort'):
            return 'abort'
        else:
            print("❌ Неверный выбор. Пожалуйста, введите r, s или a.")

def apply_patches_for_group(group: Group) -> None:
    """
    Применить все подготовленные патчи для переданной группы

    1. Создаю временную ветку на основе TARGET_BRANCH и переключаюсь в нее
    2. Применяю все подготовленные ранее патчи для всех файлов группы
    3. Добавляю все измененные файлы группы в индекс
    4. Фиксирую коммит с итоговым описанием
    5. Переключаюсь обратно в TARGET_BRANCH
    -6. Мержу временную ветку в TARGET_BRANCH
    -7. Удаляю временную метку
    """

    # 1. Создаю временную ветку на основе TARGET_BRANCH и переключаюсь в нее:
    temp_branch = f"temp_group_{group.name.replace(' ', '_').replace(':', '')}"
    print(f"♆ Создание временной ветки: {temp_branch}")
    run_cmd(["git", "checkout", "-b", temp_branch])

    # 2. Применяю все подготовленные ранее патчи для всех файлов группы:
    for file in group.files:
        print(f"🔄 {file.name}:")
        for patch in file.patches:
            print(f"  - Применение патча {patch}")
            while True:
                try:
                    run_cmd([
                        "git", "am",
                        "-q",
                        "--3way",
                        "--ignore-space-change",
                        "--ignore-whitespace",
                        patch
                    ])
                except subprocess.CalledProcessError as e:
                    print(f"⚠️ Ошибка при применении {patch}: {e}")
                    print(f"💡 Вы можете попробовать вручную: git am {patch}\n")
                    run_cmd(["git", "am", "--show-current-patch=diff"])

                    choice = confirmation_request()
                    if choice == 'retry':
                        # Пользователь поправил patch-файл и решил попробовать
                        # еще раз
                        continue  # Повторить попытку
                    elif choice == 'skip':
                        # Пользователь применил git am и можно продолжить
                        # следущий файл
                        print(f"⏩ Пропускаем патч: {patch}")
                        break  # Переход к следующему патчу
                    elif choice == 'abort':
                        # Пользователь решил прервать работу
                        print("🛑 Прерываю выполнение...")
                        sys.exit(1)
                else:
                    # Успешно применился git am
                    break  # Переход к следующему патчу

    # 3. Добавляю все измененные файлы группы в индекс:
    print("📦 Добавление файлов из группы в индекс:")
    for file in group.files:
        run_cmd(["git", "add", file.name])

    # 4. Фиксирую коммит с итоговым описанием
    combined_message = f"Apply changes for {group.name}\n\nOriginal commits:\n"
    for file in group.files:
        combined_message += f"  {file.name:}\n"
        for commit in file.commits:
            combined_message += f"    {commit.hsh}: {commit.comment}\n"

    combined_commit_file = f"combined_commit_msg_{group.name}.txt"
    with open(combined_commit_file, "w") as f:
        f.write(combined_message)

    print(f"🔗 Слияние в {TARGET_BRANCH}")
    # 5. Переключаюсь обратно в TARGET_BRANCH:
    run_cmd(["git", "checkout", TARGET_BRANCH])

    # 6. Мержу временную ветку в TARGET_BRANCH:
    # print(f"💾 Merge ветки {temp_branch} в целевую ветку {TARGET_BRANCH}")
    # run_cmd(["git", "merge", "--no-ff", "--no-edit", "-F", combined_commit_file, temp_branch])
    # os.remove(combined_commit_file)

    # 7. Удаляю временную метку:
    # run_cmd(["git", "branch", "-D", temp_branch])

    print(f"✅ Группа '{group.name}' успешно применена\n")

# --- Основная логика ---
if __name__ == "__main__":
    ### Шаг 0: Подготовить структуру куда буду сливать все наработки
    prepare_group()

    ### Шаг 1: Получить все изменённые файлы в рамках диапазона
    print(f"🔍 Получение всех изменённых файлов между коммитами {START_COMMIT} и {END_COMMIT} (включительно)")
    print(f"Через: git diff --name-only {START_COMMIT}^..{END_COMMIT}\n")
    changed_files = get_changed_files_in_range()
    print(f"✅ Найдено {len(changed_files)} файлов (исключён debian/changelog)")

    ### Шаг 2: Разбиваю найденные файлы на группы
    # Распихиваю все файлы по заданным категориям
    print(f"🔍 Разнос найденных файлов по категориям")
    not_founded_files = categorize_files(changed_files)
    if not_founded_files:
        print("\n\n❗❗❗ ВНИМАНИЕ ❗❗❗\n")
        print(f"🚨 Не найдены группы для файлов (они будут пропущены):")
        print("  📄 " + "\n  📄 ".join(not_founded_files) + "\n")

    # Собираю все коммиты и комментарии для каждого файла в правильном порядке:
    for group in groups:
        for file in group.files:
            file.commits = get_commits_for_file(file)

    # Шаг 3: Собрать патчи для каждой группы
    print("\n💡  Сбор патчей для групп...")

    for index, group in enumerate(groups):
        create_patches_for_group(group, index)

    print_group()

    # Шаг 4: Подтвердить применение до переключения ветки
    print("⚠️ Подтверждение применения всех указанных патчей:")
    print("   [y] - применить все")
    print("   [q] - выйти")
    choice = input("   Введите y или q: ").lower()
    if choice == "q":
        print("🛑 Операция отменена пользователем")
        sys.exit(0)
    print("✅ Применение подтверждено\n")

    # Шаг 5: Переключиться на целевую ветку
    print(f"SetBranch Переключаюсь на ветку {TARGET_BRANCH}")
    run_cmd(["git", "switch", TARGET_BRANCH])

    # Шаг 6: Применить патчи

    for index_group, group in enumerate(groups):
        print(f"🔁 Применяю патчи для группы {group.name}")
        apply_patches_for_group(group)

    print(f"\n🎉 Завершено")
