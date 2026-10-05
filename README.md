# Safe Claude

Небольшая Windows-утилита, которая сбрасывает локальные идентификаторы устройства Claude (десктоп-приложение и Claude Code) и запускает Claude заново. **История чатов и сессия аккаунта при этом сохраняются.**

## Что делает

Перед очисткой закрывает все процессы `claude.exe`, затем обрабатывает выбранные пункты:

| Цель | Что происходит |
|------|----------------|
| `~/.claude.json` | удаляются ключи `machineID`, `userID`, `anonymousId` |
| `~/.claude/.claude.json` | то же самое (вложенные ID) |
| `%APPDATA%\Claude\ant-did` | удаляется (ID устройства приложения) |
| `%APPDATA%\Claude\ant-device-registry.json` | удаляется |
| `%APPDATA%\Claude\remote-control-state.json` | удаляется |
| `%APPDATA%\Claude\config.json` | удаляется только `pairedDeviceId`, токен логина остаётся |

- **Clean and launch Claude** — очистить и сразу запустить Claude.
- **Clean only** — только очистить.
- Данное приложение поможет избежать блокировки от Anthropic для русских пользователей. Она не снизит шанс к нулю, но может увеличить ваши шансы не потерять проекты.
Кнопки:

## Что НЕ удаляется

- `%APPDATA%\Claude\claude-code-sessions\` — локальные записи сессий и чаты. Раньше этот пункт стирался целиком, теперь утилита его не трогает.
- `oauth:tokenCacheV2` и `lastKnownAccountUuid` в `config.json` — вы остаётесь залогинены.
- `~/.claude/projects/` — история Claude Code в CLI.

## Сборка

Нужны CMake 3.16+, Ninja и MinGW-w64 (g++ с поддержкой C++17).

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

Результат: `build/Safe Claude.exe` (статическая линковка, без внешних DLL).

## Использование

1. Запустить `Safe Claude.exe`.
2. Отметить нужные пункты (по умолчанию выбраны все).
3. Нажать **Clean and launch Claude** или **Clean only**.

## Структура проекта

```
CMakeLists.txt   сборка
src/main.cpp     вся логика и GDI+ интерфейс
src/app.rc       ресурсы (иконка)
src/app.manifest манифест
```

## Предупреждение

Утилита завершает все процессы `claude.exe` без сохранения. Закройте несохранённую работу перед запуском. Используйте на свой страх и риск.

## Автор

@hiopat, @wucaz, @kierradev
