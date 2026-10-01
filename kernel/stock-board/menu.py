#!/usr/bin/env python3
"""Local interactive frontend; delegates VM ownership and shutdown to the runtime."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import tempfile
import time

import guest_control
import profiles
import settings
import vendor_identity

HERE = Path(__file__).resolve().parent
DEFAULT_STATE = Path.home() / 'Library/Application Support/Pinology/menu.json'


def clean_env():
    # A Terminal session's exported settings must not override menu choices.
    defaults = set(settings.DEFAULTS) | {'SERIAL'}
    return {k: v for k, v in os.environ.items() if k not in defaults}


def instance_model(root):
    path = root / 'instance.json'
    if path.is_symlink():
        raise ValueError('Ссылка вместо instance.json не поддерживается')
    return profiles.validate_manifest(json.loads(path.read_text()))[0]


def validate(item):
    root = Path(item['path']).expanduser().resolve(strict=True)
    if ',' in str(root) or '\n' in str(root):
        raise ValueError('Путь не должен содержать запятую или перевод строки')
    model = instance_model(root)
    for key in ('http', 'smb'):
        if type(item[key]) is not int or not 1024 <= item[key] <= 65535:
            raise ValueError('Порты должны быть в диапазоне 1024–65535')
    if item['http'] == item['smb']:
        raise ValueError('HTTP и SMB требуют разных портов')
    if type(item.get('experimental', False)) is not bool or type(item.get('nic2', False)) is not bool:
        raise ValueError('Некорректные экспериментальные настройки')
    if model != 'DS423' and item.get('nic2'):
        raise ValueError('Второй адаптер доступен только для DS423')
    return root, model


def command(bundle, *args):
    return [str(bundle / 'dsm'), *map(str, args)]


def run_command(bundle, item):
    root, model = validate(item)
    if model == 'DS423' and not item.get('experimental'):
        raise ValueError('Для DS423 нужно подтвердить экспериментальный режим в настройках')
    args = command(bundle, 'run', root, '--accel', 'hvf', '--flash',
                   '--console-socket', '--restart-on-guest-reset',
                   '--bind-address', '127.0.0.1', '--network', 'user',
                   '--port', item['http'], '--smb-port', item['smb'])
    if model == 'DS423':
        args.append('--experimental-ds423')
    if item.get('nic2'):
        args.append('--experimental-second-nic')
    return args


def start(bundle, item):
    args = run_command(bundle, item)
    root = Path(item['path']).resolve(strict=True)
    if not guest_control.stopped(root):
        raise ValueError('Стенд уже занят или осталась active.lease; повторный запуск запрещён')
    for port in (item['http'], item['smb']):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', port))
    # Runtime rechecks locks/lease and binding; preflight is not an ownership lock.
    logdir = root / 'logs'
    if logdir.is_symlink():
        raise ValueError('Ссылка вместо каталога logs не поддерживается')
    logdir.mkdir(exist_ok=True, mode=0o700)
    fd, name = tempfile.mkstemp(prefix='menu-', suffix='.log', dir=logdir)
    with os.fdopen(fd, 'w') as log:
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, env=clean_env(),
                                cwd=root, start_new_session=True)
    time.sleep(1)
    if proc.poll() is not None:
        raise ValueError(f'Запуск завершился с кодом {proc.returncode}; журнал: {name}')
    print(f'Запуск в фоне, PID {proc.pid}. Это ещё не готовность DSM.\nЖурнал: {name}')
    print('Меню и Terminal можно закрыть. Для выключения используйте DSM или пункт 5.')


def save(path, item):
    validate(item)
    fd, name = tempfile.mkstemp(prefix='.menu-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(item, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def ask(label, default=''):
    value = input(f'{label}' + (f' [{default}]' if default != '' else '') + ': ').strip()
    return value or str(default)


def ask_path(label):
    value = ask(label)
    if not value:
        raise ValueError('Путь не указан')
    # Accept Finder drag-and-drop quoted paths, but never evaluate shell text.
    if value[0:1] in ('"', "'") and value[-1:] == value[0]:
        value = value[1:-1]
    return Path(value).expanduser().resolve()


def configure(root, previous=None):
    model = instance_model(root)
    old = previous or {}
    item = dict(path=str(root), http=int(ask('HTTP-порт', old.get('http', 15520))),
                smb=int(ask('SMB-порт', old.get('smb', 14460))),
                experimental=False, nic2=False)
    if model == 'DS423':
        print('DS423 — экспериментальная эмуляция. Полная совместимость не гарантируется.')
        item['experimental'] = ask('Разрешить экспериментальный DS423? да/нет',
                                    'да' if old.get('experimental') else 'нет') == 'да'
        item['nic2'] = ask('Экспериментальный второй Ethernet? да/нет',
                           'да' if old.get('nic2') else 'нет') == 'да'
    validate(item)
    return item


def create(bundle):
    model = ask('Модель DS223/DS423', 'DS223').upper()
    profiles.profile(model)
    root = ask_path('Новая папка стенда (не должна существовать)')
    if root.exists() or root.with_name(root.name + '.media').exists():
        raise ValueError('Стенд или его .media уже существует; выберите новое имя')
    if not root.parent.is_dir() or ',' in str(root) or '\n' in str(root):
        raise ValueError('Нужен допустимый путь в существующем родительском каталоге')
    size = ask('Размер одного нового диска', '32G')
    serial = vendor_identity.validate_serial(ask('Серийный номер (необязательно)'))
    settings.load(environ={'DISK_SIZE': size})
    print('Будет создан один разреженный qcow2-диск. Пул Basic создаётся в мастере DSM.')
    print('Установка 7.2.2: официальный PAT проверяется по SHA-256. DSM/PAT в комплект не входят.')
    print('Для распаковки PAT нужен Docker, но сама DSM работает нативно через HVF.')
    args = command(bundle, 'init', root, '--model', model, '--disk-size', size)
    if serial:
        args += ['--serial', serial]
    if ask('Есть проверенные распакованные файлы PAT? да/нет', 'нет') == 'да':
        args += ['--pat', str(ask_path('Файл PAT')), '--artifacts', str(ask_path('Папка распакованных файлов'))]
    else:
        subprocess.run(['docker', 'info'], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)
    if ask('Скачать/подготовить установку? да/нет', 'нет') != 'да':
        return None
    subprocess.run(args, env=clean_env(), check=True)
    print('Носители готовы. Запустите стенд и завершите мастер установки в браузере.')
    return configure(root)


def open_dsm(item):
    root, _ = validate(item)
    # QMP identity prevents blindly opening an unrelated process on an old port.
    guest_control.qmp_status(root)
    state = guest_control.read_state(root)
    if state.get('network') != 'user':
        raise ValueError('Сеть DSM выключена')
    subprocess.run(['open', f'http://127.0.0.1:{state["port"]}/'], check=True)


def stop(bundle, item):
    root, _ = validate(item)
    print('Безопасное выключение: меню пользователя DSM → Выключить.')
    print('Или укажите существующий файл учётных данных JSON (0600). Пароль здесь не сохраняется.')
    if ask('Использовать файл для штатного выключения? да/нет', 'нет') != 'да':
        open_dsm(item)
        return
    path = ask_path('Файл account/password')
    guest_control.credentials(path)  # validate without displaying values
    if ask('Выключить выбранную DSM сейчас? да/нет', 'нет') == 'да':
        subprocess.run(command(bundle, 'stop', root, '--shutdown-credentials', path),
                       env=clean_env(), check=True)


def menu(bundle, state):
    item = json.loads(state.read_text()) if state.exists() else None
    while True:
        print('\nDSM Lab · Apple Silicon · QEMU + HVF')
        print('Стенд: ' + (item['path'] if item else 'не выбран'))
        print('1 Выбрать существующий   2 Создать новый   3 Запустить\n'
              '4 Открыть DSM            5 Выключить       6 Состояние\n'
              '7 Порты и опции          8 Журналы         9 Холодная копия\n'
              '0 Выйти (DSM продолжит работать)')
        try:
            choice = ask('Действие', '0')
            if choice == '0':
                return
            if choice == '1':
                root = ask_path('Папка стенда с instance.json')
                old = {}
                if (root / 'runtime.json').exists():
                    old['http'] = guest_control.read_state(root)['port']
                candidate = configure(root, old)
                save(state, candidate)
                item = candidate
            elif choice == '2':
                candidate = create(bundle)
                if candidate:
                    save(state, candidate)
                    item = candidate
            elif choice not in ('3', '4', '5', '6', '7', '8', '9'):
                print('Неизвестный пункт')
            elif not item:
                print('Сначала выберите или создайте стенд')
            else:
                root, _ = validate(item)
                if choice == '3':
                    start(bundle, item)
                elif choice == '4':
                    open_dsm(item)
                elif choice == '5':
                    stop(bundle, item)
                elif choice == '6':
                    print(guest_control.health(root)[1])
                    print('Холодная копия: ' + ('допустима' if guest_control.stopped(root)
                                                else 'стенд занят или lease требует проверки'))
                elif choice == '7':
                    if not guest_control.stopped(root):
                        raise ValueError('Для изменения настроек сначала штатно выключите DSM')
                    candidate = configure(root, item)
                    save(state, candidate)
                    item = candidate
                elif choice == '8':
                    subprocess.run(['open', str(root / 'logs')], check=True)
                elif choice == '9':
                    if not guest_control.stopped(root):
                        raise ValueError('Копия разрешена только после штатного выключения')
                    destination = ask_path('Новая папка холодной копии')
                    subprocess.run(command(bundle, 'checkpoint', root, destination),
                                   env=clean_env(), check=True)
        except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
            print(f'Не выполнено: {error}')
        except KeyboardInterrupt:
            print('\nДействие прервано. Работающая DSM не выключена.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, default=HERE.parent)
    parser.add_argument('--state', type=Path, default=DEFAULT_STATE)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('Нужен Mac с Apple Silicon; fallback на TCG отключён')
    bundle, state = args.bundle.resolve(strict=True), args.state.expanduser().absolute()
    if not (bundle / 'dsm').is_file():
        parser.error('Не найден комплект dsm рядом с меню')
    state.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(str(state) + '.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if state.is_symlink():
                raise ValueError('Ссылка вместо настроек меню не поддерживается')
            menu(bundle, state)
    except BlockingIOError:
        parser.error('Меню уже открыто в другом окне')
    except (EOFError, KeyboardInterrupt):
        print('\nМеню закрыто. Работающая DSM не выключена.')
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
